#pragma once

#include "expected.hpp"
#include "link.hpp"
#include "schema/webshot.hpp"
#include "server_errors.hpp"
#include "text.hpp"
#include "userver_namespaces.hpp"

#include <chrono>
#include <optional>
#include <string_view>
#include <vector>

#include <boost/uuid/uuid.hpp>

#include <userver/components/component_base.hpp>
#include <userver/yaml_config/schema.hpp>
using Uuid = boost::uuids::uuid;

namespace v1 {
enum class DenylistError;

struct [[nodiscard]] ClientIpCooldown final {
    std::chrono::milliseconds retryAfter;
};

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
    [[nodiscard]] Expected<dto::UuidWithTimeLink, errors::CrawlFailure> createCapture(Link link);

    /**
     * @brief Enqueue a crawl job for the given link and return its job descriptor.
     *
     * Assigns a UUID that will also be used as the capture id once the job
     * succeeds. Job execution is scheduled asynchronously; callers should poll
     * job status via findCaptureJob().
     */
    [[nodiscard]] Expected<dto::CaptureJob, errors::CreateJobError> createCaptureJob(Link link);
    /** @brief Acquire per-IP cooldown for an HTTP CRUD operation. */
    [[nodiscard]] Expected<std::optional<ClientIpCooldown>, errors::CrudError>
    acquireClientIpCooldown(String clientIp);
    /** @brief Look up a capture by id. */
    [[nodiscard]] Expected<std::optional<Link>, errors::CrudError> findCapture(Uuid uuid);

    /** @brief Look up a capture job by id. */
    [[nodiscard]] Expected<std::optional<dto::CaptureJob>, errors::CrudError>
    findCaptureJob(Uuid uuid);

    /** @brief All capture ids for a link (newest first). */
    [[nodiscard]] Expected<std::vector<dto::UuidWithTime>, errors::CrudError>
    findCapturesByLink(const Link &link);
    /** @brief Paged variant for capture ids by link. */
    [[nodiscard]] Expected<dto::PagedFindCapturesByUrlResponse, errors::CapturePageError>
    findCapturesByLinkPage(const Link &link, String pageToken);
    /** @brief Paged list of captures grouped by normalized link prefix. */
    [[nodiscard]] Expected<dto::PagedFindCapturesByPrefixResponse, errors::CapturePageError>
    findCapturesByPrefixPage(String normalizedPrefix, String pageToken);
    /** @brief Disallow a prefix and enqueue purge of its captures. */
    [[nodiscard]] Expected<void, DenylistError> disallowAndPurgePrefix(String prefixKey) noexcept;
    /** @brief Static config schema for this component. */
    [[nodiscard]] static us::yaml_config::Schema GetStaticConfigSchema();

private:
    class Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace v1
