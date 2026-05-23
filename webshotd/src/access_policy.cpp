#include "access_policy.hpp"
/**
 * @file
 * @brief Link prefix access policy checks and persistence backed by Postgres.
 */
#include "prefix_utils.hpp"
#include "shared_state_repo.hpp"
#include "text_postgres_formatter.hpp"
#include "try.hpp"

#include <format>
#include <string>
#include <utility>

#include <userver/components/component.hpp>
#include <userver/logging/log.hpp>
#include <userver/yaml_config/merge_schemas.hpp>
#include <userver/yaml_config/yaml_config.hpp>
namespace ws {
namespace us = userver;
using namespace text::literals;

String AccessDecisionMessage(AccessDecisionReason reason)
{
    using enum AccessDecisionReason;

    switch (reason) {
    case kAllowed:
        return "allowed"_t;
    case kDenylisted:
        return "link prefix in denylist"_t;
    case kNotAllowlisted:
        return "link not in allowlist"_t;
    case kNonHttps:
        return "non-HTTPS fetch blocked"_t;
    }
}

us::yaml_config::Schema AccessPolicyStore::GetStaticConfigSchema()
{
    return us::yaml_config::MergeSchemas<us::components::ComponentBase>(R"(
type: object
description: 'access policy store component'
additionalProperties: false
properties: {}
)");
}

struct AccessPolicyStore::Impl {
    explicit Impl(
        const us::components::ComponentConfig &, const us::components::ComponentContext &context
    )
        : repo(context.FindComponent<SharedStateRepo>())
    {
    }
    SharedStateRepo &repo;
};

AccessPolicyStore::AccessPolicyStore(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : us::components::ComponentBase(config, context), impl_(std::make_unique<Impl>(config, context))
{
}

AccessPolicyStore::~AccessPolicyStore() = default;

Expected<bool, AccessPolicyError> AccessPolicyStore::IsAllowedPrefix(const String &prefix_key)
{
    return !TRY(IsDeniedPrefix(prefix_key));
}

Expected<bool, AccessPolicyError> AccessPolicyStore::IsDeniedPrefix(const String &prefix_key)
{
    auto tree = prefix::MakePrefixTree(prefix_key);
    auto denied = impl_->repo.CheckDenylistTree(tree);
    if (!denied)
        LOG_ERROR() << std::format("access policy check failed: {}", denied.Error().what);
    return TRY_ERR_AS(std::move(denied), AccessPolicyError::kDbError);
}

Expected<bool, AccessPolicyError> AccessPolicyStore::IsAllowlistedPrefix(const String &prefix_key)
{
    auto tree = prefix::MakePrefixTree(prefix_key);
    auto allowlisted = impl_->repo.CheckAllowlistTree(tree);
    if (!allowlisted)
        LOG_ERROR() << std::format("allowlist check failed: {}", allowlisted.Error().what);
    return TRY_ERR_AS(std::move(allowlisted), AccessPolicyError::kDbError);
}

Expected<AccessDecision, AccessPolicyError>
AccessPolicyStore::EvaluatePrefix(const String &prefix_key, AccessPolicyMode mode)
{
    using enum AccessDecisionReason;
    using enum AccessPolicyMode;

    if (mode == kRegular) {
        auto allowlisted = TRY(IsAllowlistedPrefix(prefix_key));
        if (allowlisted)
            return AccessDecision{.allowed = true, .reason = kAllowed};

        auto denied = TRY(IsDeniedPrefix(prefix_key));
        if (denied)
            return AccessDecision{.allowed = false, .reason = kDenylisted};
        return AccessDecision{.allowed = true, .reason = kAllowed};
    }

    auto denied = TRY(IsDeniedPrefix(prefix_key));
    if (denied)
        return AccessDecision{.allowed = false, .reason = kDenylisted};

    auto allowlisted = TRY(IsAllowlistedPrefix(prefix_key));
    if (!allowlisted)
        return AccessDecision{.allowed = false, .reason = kNotAllowlisted};
    return AccessDecision{.allowed = true, .reason = kAllowed};
}

Expected<void, AccessPolicyError>
AccessPolicyStore::InsertPrefix(const String &prefix_key, const String &reason)
{
    auto tree = prefix::MakePrefixTree(prefix_key);
    auto inserted = impl_->repo.InsertDenylistPrefix(prefix_key, tree, reason);
    if (!inserted) {
        LOG_CRITICAL() << std::format(
            "denylist insert failed for {}: {}", prefix_key, inserted.Error().what
        );
        us::utils::AbortWithStacktrace("denylist insert failed");
    }
    return {};
}

Expected<void, AccessPolicyError>
AccessPolicyStore::InsertAllowlistPrefix(const String &prefix_key, const String &reason)
{
    auto tree = prefix::MakePrefixTree(prefix_key);
    auto inserted = impl_->repo.InsertAllowlistPrefix(prefix_key, tree, reason);
    if (!inserted) {
        LOG_ERROR() << std::format(
            "allowlist insert failed for {}: {}", prefix_key, inserted.Error().what
        );
        return Unex(AccessPolicyError::kDbError);
    }
    return {};
}

Expected<void, AccessPolicyError> AccessPolicyStore::RemoveAllowlistPrefix(const String &prefix_key)
{
    auto removed = impl_->repo.DeleteAllowlistPrefix(prefix_key);
    if (!removed) {
        LOG_ERROR() << std::format(
            "allowlist remove failed for {}: {}", prefix_key, removed.Error().what
        );
        return Unex(AccessPolicyError::kDbError);
    }
    return {};
}

} // namespace ws
