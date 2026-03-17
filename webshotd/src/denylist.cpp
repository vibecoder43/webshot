/**
 * @file
 * @brief Host denylist checks and persistence backed by Postgres.
 */

#include "denylist.hpp"
#include "prefix_utils.hpp"
#include <exception>
#include <fmt/format.h>
#include <string>
#include <userver/logging/level.hpp>
#include <userver/logging/log.hpp>
#include <userver/logging/log_helper.hpp>
#include <userver/storages/postgres/cluster.hpp>
#include <userver/storages/postgres/cluster_types.hpp>
#include <userver/storages/postgres/component.hpp>
#include <userver/storages/postgres/io/buffer_io.hpp>
#include <userver/storages/postgres/io/traits.hpp>
#include <userver/storages/postgres/postgres_fwd.hpp>
#include <userver/storages/postgres/result_set.hpp>
#include <userver/utils/assert.hpp>
#include <userver/utils/zstring_view.hpp>
#include <userver/yaml_config/merge_schemas.hpp>
#include <webshot/sql_queries.hpp>

namespace us = userver;
namespace pg = us::storages::postgres;
namespace sql = webshot::sql;

namespace v1 {

us::yaml_config::Schema Denylist::GetStaticConfigSchema()
{
    return us::yaml_config::MergeSchemas<us::components::ComponentBase>(R"(
type: object
description: 'denylist component'
additionalProperties: false
properties: {}
)");
}

struct Denylist::Impl {
    explicit Impl(
        const us::components::ComponentConfig &, const us::components::ComponentContext &context
    )
        : cluster(context.FindComponent<us::components::Postgres>("shared_state_db").GetCluster())
    {
    }
    pg::ClusterPtr cluster;
};

Denylist::Denylist(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : us::components::ComponentBase(config, context), impl(std::make_unique<Impl>(config, context))
{
}

Denylist::~Denylist() = default;

bool Denylist::isAllowedPrefix(const String &prefixKey) noexcept
{
    try {
        const auto tree = prefix::makePrefixTree(prefixKey);
        const auto blocked =
            impl->cluster
                ->Execute(pg::ClusterHostType::kSlaveOrMaster, sql::kCheckDenylistTree, tree)
                .AsSingleRow<bool>();
        return !blocked;
    } catch (const std::exception &e) {
        LOG_ERROR() << fmt::format("denylist check failed: {}", e.what());
        return false;
    }
}

void Denylist::insertPrefix(const String &prefixKey, const String &reason)
{
    try {
        const auto tree = prefix::makePrefixTree(prefixKey);
        static_cast<void>(impl->cluster->Execute(
            pg::ClusterHostType::kMaster, sql::kInsertDenylistHost, prefixKey, tree, reason
        ));
    } catch (const std::exception &e) {
        LOG_CRITICAL() << fmt::format("denylist insert failed for {}: {}", prefixKey, e.what());
        us::utils::AbortWithStacktrace("denylist insert failed");
    }
}

} // namespace v1
