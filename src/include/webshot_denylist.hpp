#pragma once
#include <memory>
#include <string>

#include <userver/components/component_base.hpp>
#include <userver/yaml_config/schema.hpp>

namespace v1 {

/**
 * @brief Host denylist management and purge helper.
 *
 * Provides host checks used by the ingestion path and an administrative purge
 * that deletes all captures for a host and its subhosts.
 */
class [[nodiscard]] WebshotDenylist : public userver::components::ComponentBase {
public:
    static constexpr std::string_view kName = "webshot-denylist";

    explicit WebshotDenylist(
        const userver::components::ComponentConfig &config,
        const userver::components::ComponentContext &context
    );

    ~WebshotDenylist();

    /** @brief Returns true if the host is not deny‑listed. */
    [[nodiscard]] bool isAllowedHost(const std::string &host) noexcept;
    /** @brief Insert a host into the denylist (noop if already present). */
    void insertHost(const std::string &host);
    static userver::yaml_config::Schema GetStaticConfigSchema();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace v1
