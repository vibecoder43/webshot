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

/**
 * @brief Host denylist management and purge helper.
 *
 * Provides host checks used by the ingestion path and an administrative purge
 * that deletes all captures for a host and its subhosts.
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
    /** @brief Insert a prefix key into the denylist (noop if already present). */
    [[nodiscard]] Expected<void, DenylistError>
    insertPrefix(const String &prefixKey, const String &reason);
    static us::yaml_config::Schema GetStaticConfigSchema();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace v1
