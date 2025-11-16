#include "include/webshot_denylist.hpp"
#include "include/s3_secdist.hpp"
#include "include/sql.hpp"

#include <string>

#include <boost/uuid/uuid.hpp>

#include <fmt/format.h>

#include <userver/clients/http/component.hpp>
#include <userver/components/component.hpp>
#include <userver/logging/log.hpp>
#include <userver/s3api/clients/s3api.hpp>
#include <userver/s3api/models/s3api_connection_type.hpp>
#include <userver/storages/postgres/cluster.hpp>
#include <userver/storages/postgres/component.hpp>
#include <userver/storages/secdist/component.hpp>
#include <userver/storages/secdist/secdist.hpp>
#include <userver/utils/boost_uuid4.hpp>

namespace us = userver;
namespace pg = us::storages::postgres;
using Uuid = boost::uuids::uuid;

namespace v1 {

struct WebshotDenylist::Impl {
    explicit Impl(
        const us::components::ComponentConfig &cfg, const us::components::ComponentContext &context
    )
        : cluster(context.FindComponent<us::components::Postgres>("denylist-db").GetCluster()),
          metaCluster(
              context.FindComponent<us::components::Postgres>("webshot-meta-db").GetCluster()
          ),
          httpClient(context.FindComponent<us::components::HttpClient>().GetHttpClient()),
          bucket(cfg["s3-bucket"].As<std::string>()),
          endpoint(cfg["s3-endpoint"].As<std::string>()),
          region(cfg["s3-region"].As<std::string>("local")),
          timeout(std::chrono::milliseconds(cfg["s3-timeout-ms"].As<int>(10000)))
    {
        auto conn = us::s3api::MakeS3Connection(
            httpClient, us::s3api::S3ConnectionType::kHttp, endpoint,
            us::s3api::ConnectionCfg(timeout, 1)
        );
        const auto &secdist = context.FindComponent<us::components::Secdist>().Get();
        const auto &creds = secdist.Get<v1::S3CredentialsSecdist>();
        if (!creds.access_key_id || !creds.secret_access_key)
            throw std::runtime_error("missing required S3 secdist credentials (s3_credentials)");
        auto auth = std::make_shared<us::s3api::authenticators::AccessKey>(
            *creds.access_key_id, us::s3api::Secret(*creds.secret_access_key)
        );
        s3 = us::s3api::GetS3Client(conn, auth, bucket);
    }
    pg::ClusterPtr cluster;
    pg::ClusterPtr metaCluster;
    us::clients::http::Client &httpClient;
    const std::string bucket;
    const std::string endpoint;
    const std::string region;
    const std::chrono::milliseconds timeout;
    us::s3api::ClientPtr s3;

    template <typename... Ts> [[nodiscard]] auto readonly(Ts &&...args)
    {
        return metaCluster->Execute(pg::ClusterHostType::kSlaveOrMaster, std::forward<Ts>(args)...);
    }

    template <typename... Ts> [[nodiscard]] auto readwrite(Ts &&...args)
    {
        return metaCluster->Execute(pg::ClusterHostType::kMaster, std::forward<Ts>(args)...);
    }
};

WebshotDenylist::WebshotDenylist(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : us::components::ComponentBase(config, context), impl(std::make_unique<Impl>(config, context))
{
}

WebshotDenylist::~WebshotDenylist() = default;

bool WebshotDenylist::isAllowedHost(const std::string &hostLowerPunycode) noexcept
{
    try {
        auto res = impl->cluster->Execute(
            pg::ClusterHostType::kSlaveOrMaster, sql::kCheckDenylist.data(), hostLowerPunycode
        );
        const bool matched = res.Size() > 0;
        return !matched;
    } catch (const std::exception &e) {
        LOG_INFO() << fmt::format("denylist check failed: {}", e.what());
        return false;
    }
}

void WebshotDenylist::purgeHostAndSubdomains(const std::string &hostLowerPunycode)
{
    std::string d_rev(hostLowerPunycode.rbegin(), hostLowerPunycode.rend());
    const std::size_t kBatch = 1000;
    while (true) {
        try {
            auto res = impl->readonly(
                sql::kSelectIdsByHostOrSubdomainsPaged.data(), d_rev, static_cast<int64_t>(kBatch)
            );
            std::vector<Uuid> ids;
            ids.reserve(res.Size());
            for (auto row : res) {
                ids.emplace_back(row[0].As<Uuid>());
            }
            if (ids.empty())
                break;
            for (auto &&id : ids) {
                const auto key = fmt::format("{}.wacz", us::utils::ToString(id));
                try {
                    impl->s3->DeleteObject(key);
                } catch (const std::exception &e) {
                    LOG_INFO() << fmt::format("S3 delete failed for key {}: {}", key, e.what());
                }
            }
            static_cast<void>(impl->readwrite(sql::kDeleteWebshotsByIds.data(), ids));
        } catch (const std::exception &e) {
            LOG_INFO() << fmt::format("denylist purge failed: {}", e.what());
            break;
        }
    }
}

} // namespace v1
