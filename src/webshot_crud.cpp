#include "include/webshot_crud.hpp"
#include "include/container_guard.hpp"
#include "include/host_policy.hpp"
#include "include/link.hpp"
#include "include/s3_secdist.hpp"
#include "include/server_errors.hpp"
#include "include/sql.hpp"
#include "include/utils.hpp"
#include "include/webshot_denylist.hpp"
#include "schemas/webshot.hpp"

#include <chrono>
#include <exception>
#include <optional>
#include <string>
#include <utility>

#include <boost/uuid/uuid.hpp>

#include <fmt/format.h>

#include <userver/clients/dns/component.hpp>
#include <userver/clients/http/component.hpp>
#include <userver/components/component.hpp>
#include <userver/components/component_base.hpp>
#include <userver/concurrent/background_task_storage.hpp>
#include <userver/crypto/base64.hpp>
#include <userver/engine/semaphore.hpp>
#include <userver/engine/subprocess/process_starter.hpp>
#include <userver/engine/task/current_task.hpp>
#include <userver/engine/task/task_processor_fwd.hpp>
#include <userver/formats/json.hpp>
#include <userver/fs/blocking/read.hpp>
#include <userver/fs/blocking/temp_directory.hpp>
#include <userver/fs/blocking/write.hpp>
#include <userver/fs/read.hpp>
#include <userver/fs/write.hpp>
#include <userver/logging/log.hpp>
#include <userver/storages/postgres/cluster.hpp>
#include <userver/storages/postgres/io/chrono.hpp>
#include <userver/storages/postgres/io/row_types.hpp>
#include <userver/storages/postgres/io/uuid.hpp>
#include <userver/storages/postgres/postgres.hpp>
#include <userver/storages/secdist/component.hpp>
#include <userver/storages/secdist/secdist.hpp>
#include <userver/utils/async.hpp>
#include <userver/utils/boost_uuid4.hpp>
#include <userver/utils/datetime/timepoint_tz.hpp>
#include <userver/yaml_config/merge_schemas.hpp>
#include <userver/yaml_config/yaml_config.hpp>

#include <userver/s3api/clients/s3api.hpp>
#include <userver/s3api/models/s3api_connection_type.hpp>

namespace pg = us::storages::postgres;
namespace engine = us::engine;
namespace concurrent = userver::concurrent;
namespace utils = us::utils;
namespace b64 = us::crypto::base64;
namespace json = us::formats::json;
namespace chrono = std::chrono;
using namespace v1;
using Uuid = boost::uuids::uuid;
using chrono::system_clock;

us::yaml_config::Schema WebshotCrud::GetStaticConfigSchema()
{
    return us::yaml_config::MergeSchemas<us::components::ComponentBase>(R"(
type: object
description: '.'
additionalProperties: false
properties:
    webshots-page-max:
        type: integer
        minimum: 1
        description: '.'
    webshots-per-link-max:
        type: integer
        minimum: 1
        description: 'Max captures per link in a prefix page'
    webshots-links-per-page-max:
        type: integer
        minimum: 1
        description: 'Max distinct links in a prefix page'
    crawler-network:
        type: string
        description: 'Name of the Docker network to run crawlers on (scoped egress rules)'
    crawl-concurrency:
        type: integer
        minimum: 1
        description: 'Max concurrent crawls; blocks above this'
    storage-mode:
        type: string
        enum: [s3]
        description: 'Storage backend'
    s3-bucket:
        type: string
    s3-endpoint:
        type: string
    s3-region:
        type: string
        description: 'Optional region label'
    public-base-url:
        type: string
        description: 'Public HTTP base for stored objects (bucket path-style)'
    s3-timeout-ms:
        type: integer
        minimum: 1
        description: 'HTTP timeout to S3'
)");
}

WebshotCrud::WebshotCrud(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : us::components::ComponentBase(config, context),
      impl(std::make_unique<WebshotCrud::Impl>(config, context))
{
}

WebshotCrud::~WebshotCrud() = default;

class [[nodiscard]] WebshotCrud::Impl {
public:
    const int64_t webshotsPageMax;
    const int64_t webshotsPerLinkMax;
    const int64_t webshotsLinksPerPageMax;
    const std::string crawlerNetwork;
    const std::string storageMode;
    const std::string s3Bucket;
    const std::string s3Endpoint;
    const std::string s3Region;
    const std::string publicBaseUrl;
    const std::chrono::milliseconds s3Timeout;
    pg::ClusterPtr cluster;
    us::clients::dns::Resolver &resolver;
    us::clients::http::Client &httpClient;
    us::s3api::ClientPtr s3Client;
    v1::WebshotDenylist *denylist;
    engine::CancellableSemaphore crawlSlots;
    // must die first
    concurrent::BackgroundTaskStorage backgroundTaskStorage;
    explicit Impl(
        const us::components::ComponentConfig &cfg, const us::components::ComponentContext &ctx
    )
        : webshotsPageMax(cfg["webshots-page-max"].As<int64_t>()),
          webshotsPerLinkMax(cfg["webshots-per-link-max"].As<int64_t>(2)),
          webshotsLinksPerPageMax(cfg["webshots-links-per-page-max"].As<int64_t>(10)),
          crawlerNetwork(cfg["crawler-network"].As<std::string>()),
          storageMode(cfg["storage-mode"].As<std::string>("s3")),
          s3Bucket(cfg["s3-bucket"].As<std::string>()),
          s3Endpoint(cfg["s3-endpoint"].As<std::string>()),
          s3Region(cfg["s3-region"].As<std::string>("")),
          publicBaseUrl(cfg["public-base-url"].As<std::string>()),
          s3Timeout(std::chrono::milliseconds(cfg["s3-timeout-ms"].As<int>(10000))),
          cluster(ctx.FindComponent<us::components::Postgres>("webshot-meta-db").GetCluster()),
          resolver(ctx.FindComponent<us::clients::dns::Component>().GetResolver()),
          httpClient(ctx.FindComponent<us::components::HttpClient>().GetHttpClient()),
          crawlSlots(cfg["crawl-concurrency"].As<size_t>(1)),
          backgroundTaskStorage(engine::current_task::GetBlockingTaskProcessor())
    {
        if (storageMode != "s3") {
            throw std::runtime_error("unsupported storage-mode; only 's3' is implemented");
        }
        // Build S3 connection and client
        auto conn = us::s3api::MakeS3Connection(
            httpClient, us::s3api::S3ConnectionType::kHttp, s3Endpoint,
            us::s3api::ConnectionCfg{s3Timeout, 1}
        );
        // Load creds from secdist if present
        const auto &secdist = ctx.FindComponent<us::components::Secdist>().Get();
        const auto &creds = secdist.Get<v1::S3CredentialsSecdist>();
        if (!creds.access_key_id || !creds.secret_access_key)
            throw std::runtime_error("missing required S3 secdist credentials (s3_credentials)");
        auto auth = std::make_shared<us::s3api::authenticators::AccessKey>(
            *creds.access_key_id, us::s3api::Secret(*creds.secret_access_key)
        );
        s3Client = us::s3api::GetS3Client(conn, auth, s3Bucket);
        denylist = &ctx.FindComponent<v1::WebshotDenylist>();
    }
    template <typename... Ts> [[nodiscard]] auto readonly(Ts &&...args)
    {
        return cluster->Execute(pg::ClusterHostType::kSlaveOrMaster, std::forward<Ts>(args)...);
    }

    template <typename... Ts> [[nodiscard]] auto readwrite(Ts &&...args)
    {
        return cluster->Execute(pg::ClusterHostType::kMaster, std::forward<Ts>(args)...);
    }
};

struct [[nodiscard]] Cursor {
    system_clock::time_point createdAt;
    Uuid id;
};

static std::string encodeToken(chrono::system_clock::time_point createdAt, const Uuid &id)
{
    const auto micros =
        chrono::duration_cast<chrono::microseconds>(createdAt.time_since_epoch()).count();
    dto::PaginationCursor cur(micros, id);
    return b64::Base64UrlEncode(
        json::ToString(json::ValueBuilder(cur).ExtractValue()), b64::Pad::kWithout
    );
}
static std::optional<Cursor> decodeToken(const std::string &token)
{
    try {
        const auto decoded = b64::Base64UrlDecode(token);
        const auto val = json::FromString(decoded);
        const auto cur = val.As<dto::PaginationCursor>();
        return {{system_clock::time_point(chrono::microseconds(cur.t)), cur.i}};
    } catch (std::exception &) {
        return {};
    }
}

void WebshotCrud::createWebshot(Link link)
{
    impl->backgroundTaskStorage.AsyncDetach(
        "create-webshot-lambda",
        [impl = impl.get(), link]() -> void {
            try {
                std::shared_lock<engine::CancellableSemaphore> slotLock(impl->crawlSlots);

                engine::subprocess::ProcessStarter starter(
                    engine::current_task::GetBlockingTaskProcessor()
                );

                std::vector<std::string> pinIps = hostpolicy::resolvePublic(
                    impl->resolver, link.host(), chrono::milliseconds(2000)
                );

                if (pinIps.empty()) {
                    LOG_INFO(
                    ) << fmt::format("crawl rejected: no public IPs for host {}", link.host());
                    return;
                }

                const auto cname = fmt::format(
                    "btcx-{}", utils::ToString(utils::generators::GenerateBoostUuid())
                );
                auto archiveRoot = us::fs::blocking::TempDirectory::Create();
                std::vector<std::string> create = {
                    "create",
                    "-v",
                    fmt::format("{}:/crawls", archiveRoot.GetPath()),
                    "-e",
                    "CHROME_FLAGS=\"--dns-over-https-mode=off\"",
                    "--shm-size",
                    "1g",
                };
                create.push_back("--network");
                create.push_back(impl->crawlerNetwork);
                for (auto &&ip : pinIps) {
                    create.push_back("--add-host");
                    create.push_back(fmt::format("{}:{}", link.host(), ip));
                }
                const std::string kWaczName = "1";
                create.push_back("--name");
                create.push_back(cname);
                create.push_back("webrecorder/browsertrix-crawler");
                create.insert(
                    create.end(),
                    {"crawl",
                     "--collection",
                     kWaczName,
                     "--generateWACZ",
                     "--workers",
                     "1",
                     "--headless",
                     "--scopeType",
                     "page-spa",
                     "--pageLimit",
                     "1",
                     "--pageLoadTimeout",
                     "10",
                     "--postLoadDelay",
                     "1",
                     "--netIdleWait",
                     "0",
                     "--pageExtraDelay",
                     "3",
                     "--behaviorTimeout",
                     "1",
                     "--waitUntil",
                     "load",
                     "--blockAds",
                     "--behaviors",
                     "siteSpecific",
                     "--lang",
                     "en",
                     "--context",
                     "general,worker,pageStatus,writer,storage,jsError,state,crawlStatus,fetch",
                     "wacz",
                     "--logLevel",
                     "debug,info",
                     "--logging",
                     "debug,stats,jserrors",
                     "--url",
                     link.httpUrl()}
                );
                ContainerGuard ctrGuard(starter, cname, create);

                auto start_proc = starter.Exec(
                    "docker", std::vector<std::string>{"start", "-a", cname},
                    engine::subprocess::ExecOptions{.use_path = true}
                );
                auto status = start_proc.Get();
                if (!status.IsExited() || status.GetExitCode() != 0) {
                    LOG_INFO(
                    ) << fmt::format("Failed to crawl {}, child process failed", link.httpUrl());
                    return;
                }
                // Remove container eagerly
                ctrGuard.remove();
                const auto pathToWaczFile = fmt::format(
                    "{0}/collections/{1}/{1}.wacz", archiveRoot.GetPath(), kWaczName
                );
                if (!us::fs::FileExists(
                        engine::current_task::GetBlockingTaskProcessor(), pathToWaczFile
                    )) {
                    LOG_INFO() << fmt::format("Failed to crawl {}, no WACZ", link.httpUrl());
                    return;
                }
                // Generate UUID upfront and upload to S3 as <uuid>.wacz
                const auto uuid = utils::generators::GenerateBoostUuid();
                const auto key = fmt::format("{}.wacz", utils::ToString(uuid));
                const auto data = us::fs::blocking::ReadFileContents(pathToWaczFile);
                try {
                    impl->s3Client->PutObject(
                        key, data, std::nullopt, "application/zip", std::nullopt, std::nullopt
                    );
                } catch (const std::exception &e) {
                    LOG_INFO() << fmt::format("S3 upload failed for {}: {}", key, e.what());
                    return;
                }
                // Build host_rev from normalized host (already lowercase/punycode by Link)
                auto host = link.host();
                std::string hostRev(host.rbegin(), host.rend());
                // Re-check denylist just before insert
                if (impl->denylist && !impl->denylist->isAllowedHost(host)) {
                    try {
                        impl->s3Client->DeleteObject(key);
                    } catch (std::exception &) {
                        // empty
                    }
                    LOG_INFO() << fmt::format("Host became denylisted during crawl: {}", host);
                    return;
                }
                const auto location = fmt::format("{}/{}", impl->publicBaseUrl, key);
                // Insert DB row with explicit id
                try {
                    impl->readwrite(
                            sql::kInsertWebshot.data(), uuid, link.normalized(), hostRev, location
                    )
                        .AsSingleRow<Uuid>();
                } catch (const std::exception &e) {
                    try {
                        impl->s3Client->DeleteObject(key);
                    } catch (std::exception &e) {
                        // empty
                    }
                    LOG_INFO() << fmt::format(
                        "DB insert failed for {}: {}", utils::ToString(uuid), e.what()
                    );
                    return;
                }
            } catch (const engine::SemaphoreLockCancelledError &e) {
                LOG_INFO() << "Crawl task cancelled while waiting for slot: " << e.what();
            }
        }
    );
}

std::optional<Webshot> WebshotCrud::findWebshot(Uuid uuid)
{
    const auto location =
        impl->readonly(sql::kSelectWebshot.data(), uuid).AsOptionalSingleRow<std::string>();
    if (!location) {
        LOG_INFO() << fmt::format("UUID not found: {}", us::utils::ToString(uuid));
        return {};
    }
    return {{*location}};
}

dto::PagedFindWebshotByUrlResponse
WebshotCrud::findWebshotByLinkPage(const Link &link, const std::optional<std::string> &pageToken)
{
    struct Row {
        Uuid uuid;
        pg::TimePointTz timepoint;
    };
    std::vector<Row> dbRows;
    const auto &norm = link.normalized();
    if (!pageToken || pageToken->empty()) {
        dbRows = impl->readonly(sql::kSelectWebshotByLinkFirst.data(), norm, impl->webshotsPageMax)
                     .AsContainer<std::vector<Row>>(pg::kRowTag);
    } else {
        auto cur = decodeToken(*pageToken);
        if (!cur)
            throw errors::InvalidPageTokenException("invalid page_token");
        dbRows = impl->readonly(
                         sql::kSelectWebshotByLinkNext.data(), norm, impl->webshotsPageMax,
                         pg::TimePointTz(cur->createdAt), cur->id
        )
                     .AsContainer<std::vector<Row>>(pg::kRowTag);
    }
    std::vector<dto::UuidWithTime> items;
    items.reserve(dbRows.size());
    for (const auto &row : dbRows) {
        items.emplace_back(
            row.uuid,
            utils::datetime::TimePointTz(static_cast<system_clock::time_point>(row.timepoint))
        );
    }
    std::optional<std::string> next;
    if (::ssize(items) == impl->webshotsPageMax && !items.empty()) {
        const auto &last = items.back();
        auto tp = last.created_at.GetTimePoint();
        next = encodeToken(tp, last.uuid);
    }
    return {items, next};
}

struct PrefixCursor {
    std::string prefix;
    std::string link;
    std::optional<system_clock::time_point> createdAt;
    std::optional<Uuid> id;
};

std::optional<PrefixCursor> decodePrefixToken(const std::string &token)
{
    try {
        const auto decoded = b64::Base64UrlDecode(token);
        const auto val = json::FromString(decoded);
        const auto cur = val.As<dto::PaginationPrefixCursor>();
        PrefixCursor out;
        out.prefix = cur.p;
        out.link = cur.l;
        if (cur.t && cur.i) {
            out.createdAt = system_clock::time_point(chrono::microseconds(*cur.t));
            out.id = *cur.i;
        }
        return out;
    } catch (std::exception &) {
        return {};
    }
}

static std::string EncodePrefixToken(const std::string &prefix, const std::string &link)
{
    dto::PaginationPrefixCursor cur(prefix, link);
    return b64::Base64UrlEncode(
        json::ToString(json::ValueBuilder(cur).ExtractValue()), b64::Pad::kWithout
    );
}

static std::string EncodePrefixToken(
    const std::string &prefix, const std::string &link, system_clock::time_point createdAt,
    const Uuid &id
)
{
    const auto micros =
        chrono::duration_cast<chrono::microseconds>(createdAt.time_since_epoch()).count();
    dto::PaginationPrefixCursor cur(prefix, link, micros, id);
    return b64::Base64UrlEncode(
        json::ToString(json::ValueBuilder(cur).ExtractValue()), b64::Pad::kWithout
    );
}

static std::optional<std::string> upperExclusiveBound(std::string s)
{
    for (int64_t i = ::ssize(s) - 1; i >= 0; i--) {
        unsigned char c = static_cast<unsigned char>(s[static_cast<size_t>(i)]);
        if (c < 0xFF) {
            s[static_cast<size_t>(i)] = static_cast<char>(c + 1);
            s.resize(static_cast<size_t>(i) + 1);
            return s;
        }
    }
    return {};
}

dto::PagedFindWebshotByPrefixResponse WebshotCrud::findWebshotsByPrefixPage(
    const std::string &normalizedPrefix, const std::optional<std::string> &pageToken
)
{
    const auto lower = normalizedPrefix;
    const auto upperOpt = upperExclusiveBound(normalizedPrefix);

    std::optional<PrefixCursor> cur;
    if (pageToken && !pageToken->empty()) {
        cur = decodePrefixToken(*pageToken);
        if (!cur || cur->prefix != normalizedPrefix)
            throw errors::InvalidPageTokenException("invalid page_token");
    }

    std::vector<std::string> links;
    links.reserve(static_cast<size_t>(impl->webshotsLinksPerPageMax));
    if (cur && cur->createdAt) {
        links.push_back(cur->link);
        if (impl->webshotsLinksPerPageMax > 1) {
            const auto more = impl->readonly(
                                      sql::kSelectDistinctLinksByPrefixNext.data(), lower, upperOpt,
                                      cur->link, impl->webshotsLinksPerPageMax - 1
            )
                                  .AsContainer<std::vector<std::string>>();
            links.insert(end(links), begin(more), end(more));
        }
    } else if (cur && !cur->createdAt) {
        const auto more = impl->readonly(
                                  sql::kSelectDistinctLinksByPrefixNext.data(), lower, upperOpt,
                                  cur->link, impl->webshotsLinksPerPageMax
        )
                              .AsContainer<std::vector<std::string>>();
        links.insert(end(links), begin(more), end(more));
    } else {
        const auto first = impl->readonly(
                                   sql::kSelectDistinctLinksByPrefixFirst.data(), lower, upperOpt,
                                   impl->webshotsLinksPerPageMax
        )
                               .AsContainer<std::vector<std::string>>();
        links.insert(end(links), begin(first), end(first));
    }

    struct Row {
        Uuid uuid;
        pg::TimePointTz tp;
    };
    std::vector<dto::UuidWithTimeLink> items;
    items.reserve(links.size() * static_cast<size_t>(impl->webshotsPerLinkMax));
    bool endedMidLink = false;
    std::string lastLink;
    std::optional<Row> lastRow;

    for (size_t idx = 0; idx < links.size(); idx++) {
        const auto &link = links[idx];
        std::vector<Row> rows;
        if (idx == 0 && cur && cur->createdAt && cur->id) {
            rows = impl->readonly(
                           sql::kSelectWebshotByLinkNext.data(), link, impl->webshotsPerLinkMax,
                           pg::TimePointTz(*cur->createdAt), *cur->id
            )
                       .AsContainer<std::vector<Row>>(pg::kRowTag);
        } else {
            rows = impl->readonly(
                           sql::kSelectWebshotByLinkFirst.data(), link, impl->webshotsPerLinkMax
            )
                       .AsContainer<std::vector<Row>>(pg::kRowTag);
        }
        for (auto &&r : rows) {
            items.emplace_back(
                r.uuid, utils::datetime::TimePointTz(static_cast<system_clock::time_point>(r.tp)),
                link
            );
        }
        if (!rows.empty()) {
            lastRow = rows.back();
            lastLink = link;
            if (::ssize(rows) == impl->webshotsPerLinkMax && idx == links.size() - 1) {
                endedMidLink = true;
            }
        } else {
            lastLink = link;
        }
    }

    std::optional<std::string> next;
    if (!items.empty()) {
        if (endedMidLink && lastRow) {
            const auto tp = static_cast<system_clock::time_point>(lastRow->tp);
            next = EncodePrefixToken(normalizedPrefix, lastLink, tp, lastRow->uuid);
        } else {
            next = EncodePrefixToken(normalizedPrefix, lastLink);
        }
    }

    return {items, next};
}
