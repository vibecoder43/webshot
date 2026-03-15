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
class [[nodiscard]] Crud : public us::components::ComponentBase {
public:
    static constexpr std::string_view kName = "crud";
    explicit Crud(
        const us::components::ComponentConfig &config,
        const us::components::ComponentContext &context
    );

    ~Crud() override;

    /**
     * @brief Run a crawl for the given link and persist metadata.
     *
     * On success returns a single capture descriptor including UUID, creation
     * time and normalized link.
     */
    [[nodiscard]] dto::UuidWithTimeLink createCapture(Link link);

    /**
     * @brief Enqueue a crawl job for the given link and return its job descriptor.
     *
     * Assigns a UUID that will also be used as the capture id once the job
     * succeeds. Job execution is scheduled asynchronously; callers should poll
     * job status via findCaptureJob().
     */
    [[nodiscard]] dto::CaptureJob createCaptureJob(Link link);
    /** @brief Look up a capture by id. */
    [[nodiscard]] std::optional<Link> findCapture(Uuid uuid);

    /** @brief Look up a capture job by id. */
    [[nodiscard]] std::optional<dto::CaptureJob> findCaptureJob(Uuid uuid);

    /** @brief All capture ids for a link (newest first). */
    [[nodiscard]] std::vector<dto::UuidWithTime> findCapturesByLink(const Link &link);
    /** @brief Paged variant for capture ids by link. */
    [[nodiscard]] dto::PagedFindCapturesByUrlResponse
    findCapturesByLinkPage(const Link &link, String pageToken);
    /** @brief Paged list of captures grouped by normalized link prefix. */
    [[nodiscard]] dto::PagedFindCapturesByPrefixResponse
    findCapturesByPrefixPage(String normalizedPrefix, String pageToken);
    /** @brief Disallow a prefix and enqueue purge of its captures. */
    void disallowAndPurgePrefix(String prefixKey);
    /** @brief Static config schema for this component. */
    [[nodiscard]] static us::yaml_config::Schema GetStaticConfigSchema();

private:
    class Impl;
    std::unique_ptr<Impl> impl;
};
}; // namespace v1
