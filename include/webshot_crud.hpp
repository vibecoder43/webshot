#pragma once

#include "link.hpp"
#include "schema/webshot.hpp"
#include "text.hpp"

#include <string_view>
#include <vector>

#include <boost/uuid/uuid.hpp>

#include <userver/components/component_base.hpp>
#include <userver/yaml_config/schema.hpp>

namespace us = userver;
using Uuid = boost::uuids::uuid;

namespace v1 {
/**
 * @brief Persistence and background-crawl facade.
 *
 * Coordinates crawl scheduling, stores metadata, and exposes query methods
 * used by HTTP handlers.
 */
class [[nodiscard]] WebshotCrud : public us::components::ComponentBase {
public:
    static constexpr std::string_view kName = "webshot_crud";
    explicit WebshotCrud(
        const us::components::ComponentConfig &config,
        const us::components::ComponentContext &context
    );

    ~WebshotCrud();

    /**
     * @brief Run a crawl for the given link and persist metadata.
     *
     * On success returns a single capture descriptor including UUID, creation
     * time and normalized link.
     */
    [[nodiscard]] dto::UuidWithTimeLink createWebshot(Link link);

    /**
     * @brief Enqueue a crawl job for the given link and return its job descriptor.
     *
     * Assigns a UUID that will also be used as the capture id once the job
     * succeeds. Job execution is scheduled asynchronously; callers should poll
     * job status via getCrawlJob().
     */
    [[nodiscard]] dto::WebshotJob createWebshotJob(Link link);
    /** @brief Look up a capture by id. */
    [[nodiscard]] std::optional<Link> findWebshot(Uuid uuid);

    /** @brief Look up a crawl job by id. */
    [[nodiscard]] std::optional<dto::WebshotJob> findCrawlJob(Uuid uuid);

    /** @brief All capture ids for a link (newest first). */
    [[nodiscard]] std::vector<dto::UuidWithTime> findWebshotByLink(const Link &link);
    /** @brief Paged variant for capture ids by link. */
    [[nodiscard]] dto::PagedFindWebshotByUrlResponse
    findWebshotByLinkPage(const Link &link, String pageToken);
    /** @brief Paged list of captures grouped by normalized link prefix. */
    [[nodiscard]] dto::PagedFindWebshotByPrefixResponse
    findWebshotsByPrefixPage(String normalizedPrefix, String pageToken);
    /** @brief Disallow a prefix and enqueue purge of its captures. */
    void disallowAndPurgePrefix(String prefixKey);
    /** @brief Static config schema for this component. */
    [[nodiscard]] static us::yaml_config::Schema GetStaticConfigSchema();

private:
    class Impl;
    std::unique_ptr<Impl> impl;
};
}; // namespace v1
