#pragma once
#include "text.hpp"

#include <memory>

#include <userver/components/component_base.hpp>
#include <userver/yaml_config/schema.hpp>

namespace v1 {

/**
 * @brief Host denylist management and purge helper.
 *
 * Provides host checks used by the ingestion path and an administrative purge
 * that deletes all captures for a host and its subhosts.
 */
class [[nodiscard]] Denylist : public userver::components::ComponentBase {
public:
    static constexpr std::string_view kName = "denylist";

    explicit Denylist(
        const userver::components::ComponentConfig &config,
        const userver::components::ComponentContext &context
    );

    ~Denylist() override;

    /** @brief Returns true if the normalized prefix key is not deny-listed. */
    [[nodiscard]] bool isAllowedPrefix(const String &prefixKey) noexcept;
    /** @brief Insert a prefix key into the denylist (noop if already present). */
    void insertPrefix(const String &prefixKey, const String &reason);
    static userver::yaml_config::Schema GetStaticConfigSchema();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace v1
