#include "webshot_denylist.hpp"
/**
 * @file
 * @brief Host denylist checks and persistence backed by Postgres.
 */
#include "sql.hpp"

#include <string>

#include <fmt/format.h>

#include <userver/components/component.hpp>
#include <userver/logging/log.hpp>
#include <userver/storages/postgres/cluster.hpp>
#include <userver/storages/postgres/component.hpp>
#include <userver/yaml_config/merge_schemas.hpp>
#include <userver/yaml_config/yaml_config.hpp>

namespace us = userver;
namespace pg = us::storages::postgres;

namespace v1 {

us::yaml_config::Schema WebshotDenylist::GetStaticConfigSchema()
{
    return us::yaml_config::MergeSchemas<us::components::ComponentBase>(R"(
type: object
description: 'webshot denylist component'
additionalProperties: false
properties: {}
)");
}

struct WebshotDenylist::Impl {
    explicit Impl(
        const us::components::ComponentConfig &, const us::components::ComponentContext &context
    )
        : cluster(context.FindComponent<us::components::Postgres>("denylist-db").GetCluster())
    {
    }
    pg::ClusterPtr cluster;
};

WebshotDenylist::WebshotDenylist(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : us::components::ComponentBase(config, context), impl(std::make_unique<Impl>(config, context))
{
}

WebshotDenylist::~WebshotDenylist() = default;

bool WebshotDenylist::isAllowedHost(const std::string &host) noexcept
{
    try {
        return impl->cluster
                   ->Execute(pg::ClusterHostType::kSlaveOrMaster, sql::kCheckDenylist.data(), host)
                   .Size() == 0;
    } catch (const std::exception &e) {
        LOG_ERROR() << fmt::format("denylist check failed: {}", e.what());
        return false;
    }
}

void WebshotDenylist::insertHost(const std::string &host, const std::string &reason)
{
    try {
        static_cast<void>(impl->cluster->Execute(
            pg::ClusterHostType::kMaster, sql::kInsertDenylistHost.data(), host, reason
        ));
    } catch (const std::exception &e) {
        LOG_ERROR() << fmt::format("denylist insert failed for {}: {}", host, e.what());
        throw;
    }
}

} // namespace v1
