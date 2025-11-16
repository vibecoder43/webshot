#include "include/webshot_denylist.hpp"
#include "include/sql.hpp"

#include <string>

#include <userver/components/component.hpp>
#include <userver/logging/log.hpp>
#include <userver/storages/postgres/cluster.hpp>
#include <userver/storages/postgres/component.hpp>

namespace us = userver;
namespace pg = us::storages::postgres;

namespace v1 {

struct WebshotDenylist::Impl {
    explicit Impl(const us::components::ComponentContext &context)
        : cluster(context.FindComponent<us::components::Postgres>("denylist-db").GetCluster())
    {
    }
    pg::ClusterPtr cluster;
};

WebshotDenylist::WebshotDenylist(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : us::components::ComponentBase(config, context), impl(std::make_unique<Impl>(context))
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
        LOG_INFO() << "denylist check failed: " << e.what();
        return false;
    }
}

} // namespace v1
