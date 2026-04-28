#pragma once

#include "expected.hpp"
#include "text.hpp"
#include "userver_namespaces.hpp"

#include <memory>

#include <userver/components/component_base.hpp>
#include <userver/yaml_config/schema.hpp>

namespace v1 {

enum class DenylistError {
    kDbFailure,
};

enum class AccessPolicyMode {
    kRegular,
    kAllowlistOnly,
};

enum class AccessDecisionReason {
    kAllowed,
    kDenylisted,
    kNotAllowlisted,
    kNonHttps,
};

struct [[nodiscard]] AccessDecision final {
    bool allowed;
    AccessDecisionReason reason;
};

[[nodiscard]] String accessDecisionMessage(AccessDecisionReason reason);

/**
 * @brief Host access-list management and purge helper.
 *
 * Provides host checks used by the ingestion path, allowlist/denylist
 * administration, and an administrative purge that deletes all captures for a
 * host and its subhosts.
 */
class [[nodiscard]] Denylist : public us::components::ComponentBase {
public:
    static constexpr std::string_view kName = "denylist";

    explicit Denylist(
        const us::components::ComponentConfig &config,
        const us::components::ComponentContext &context
    );

    ~Denylist() override;

    /** @brief Returns true if the normalized prefix key is not deny-listed. */
    [[nodiscard]] Expected<bool, DenylistError> isAllowedPrefix(const String &prefixKey);
    /** @brief Returns true if the normalized prefix key is deny-listed. */
    [[nodiscard]] Expected<bool, DenylistError> isDeniedPrefix(const String &prefixKey);
    /** @brief Returns true if the normalized prefix key is allow-listed. */
    [[nodiscard]] Expected<bool, DenylistError> isAllowlistedPrefix(const String &prefixKey);
    /** @brief Evaluate the normalized prefix key against allowlist and denylist policy. */
    [[nodiscard]] Expected<AccessDecision, DenylistError>
    evaluatePrefix(const String &prefixKey, AccessPolicyMode mode);
    /** @brief Insert a prefix key into the denylist (noop if already present). */
    [[nodiscard]] Expected<void, DenylistError>
    insertPrefix(const String &prefixKey, const String &reason);
    /** @brief Insert a prefix key into the allowlist (noop if already present). */
    [[nodiscard]] Expected<void, DenylistError>
    insertAllowlistPrefix(const String &prefixKey, const String &reason);
    /** @brief Remove a prefix key from the allowlist (noop if absent). */
    [[nodiscard]] Expected<void, DenylistError> removeAllowlistPrefix(const String &prefixKey);
    static us::yaml_config::Schema GetStaticConfigSchema();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace v1
