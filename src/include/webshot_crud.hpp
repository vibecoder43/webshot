#pragma once

#include "link.hpp"
#include "schemas/webshot.hpp"
#include "webshot.hpp"

#include <string>
#include <string_view>
#include <vector>

#include <boost/uuid/uuid.hpp>

#include <userver/components/component_base.hpp>
#include <userver/yaml_config/schema.hpp>

namespace us = userver;
using Uuid = boost::uuids::uuid;

namespace v1 {
/**
 * @brief Persistence and background‑crawl facade.
 *
 * Coordinates crawl scheduling, stores metadata, and exposes query methods
 * used by HTTP handlers.
 */
class [[nodiscard]] WebshotCrud : public us::components::ComponentBase {
public:
    static constexpr std::string_view kName = "webshot-crud";
    explicit WebshotCrud(
        const us::components::ComponentConfig &config,
        const us::components::ComponentContext &context
    );

    ~WebshotCrud();

    /**
     * @brief Enqueue a crawl for the given link and persist metadata.
     *
     * Expects a normalized Link and a non-empty list of prevalidated public IP
     * addresses for its host (SSR F checks are done at handler level).
     */
    void createWebshot(Link link, std::vector<std::string> pinnedIps);
    /** @brief Look up a capture by id. */
    [[nodiscard]] std::optional<Webshot> findWebshot(Uuid uuid);

    /** @brief All capture ids for a link (newest first). */
    [[nodiscard]] std::vector<dto::UuidWithTime> findWebshotByLink(const Link &link);
    /** @brief Paged variant for capture ids by link. */
    [[nodiscard]] dto::PagedFindWebshotByUrlResponse
    findWebshotByLinkPage(const Link &link, const std::optional<std::string> &pageToken);
    /** @brief Paged list of captures grouped by normalized link prefix. */
    [[nodiscard]] dto::PagedFindWebshotByPrefixResponse findWebshotsByPrefixPage(
        const std::string &normalizedPrefix, const std::optional<std::string> &pageToken
    );
    /** @brief Disallow a domain and enqueue purge of its captures. */
    void disallowAndPurgeDomain(std::string domain);
    /** @brief Static config schema for this component. */
    [[nodiscard]] static us::yaml_config::Schema GetStaticConfigSchema();

private:
    class Impl;
    std::unique_ptr<Impl> impl;
};
}; // namespace v1
