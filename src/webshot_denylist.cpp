#include "webshot_denylist.hpp"
/**
 * @file
 * @brief Host denylist checks and persistence backed by Postgres.
 */
#include <webshot/sql_queries.hpp>

#include "text_postgres_formatter.hpp"
#include "webshot_prefix_utils.hpp"

#include <string>
#include <vector>

#include <fmt/format.h>

#include <userver/components/component.hpp>
#include <userver/logging/log.hpp>
#include <userver/storages/postgres/cluster.hpp>
#include <userver/storages/postgres/component.hpp>
#include <userver/utils/assert.hpp>
#include <userver/yaml_config/merge_schemas.hpp>
#include <userver/yaml_config/yaml_config.hpp>

namespace us = userver;
namespace pg = us::storages::postgres;
namespace sql = webshot::sql;

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
        : cluster(context.FindComponent<us::components::Postgres>("shared-state-db").GetCluster())
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

bool WebshotDenylist::isAllowedPrefix(const String &prefixKey) noexcept
{
    auto candidates = prefix::expandPrefixCandidates(prefixKey);
    std::vector<std::string> prefixes;
    prefixes.reserve(candidates.size());
    for (const auto &c : candidates)
        prefixes.push_back(c);
    try {
        return impl->cluster
                   ->Execute(pg::ClusterHostType::kSlaveOrMaster, sql::kCheckDenylist, prefixes)
                   .Size() == 0;
    } catch (const std::exception &e) {
        LOG_ERROR() << fmt::format("denylist check failed: {}", e.what());
        return false;
    }
}

void WebshotDenylist::insertPrefix(const String &prefixKey, const String &reason)
{
    try {
        static_cast<void>(impl->cluster->Execute(
            pg::ClusterHostType::kMaster, sql::kInsertDenylistHost, prefixKey, reason
        ));
    } catch (const std::exception &e) {
        LOG_CRITICAL() << fmt::format("denylist insert failed for {}: {}", prefixKey, e.what());
        us::utils::AbortWithStacktrace("denylist insert failed");
    }
}

} // namespace v1
