#pragma once

#include "expected.hpp"
#include "link.hpp"
#include "schema/public/webshot.hpp"
#include "server_errors.hpp"
#include "text.hpp"

#include <chrono>
#include <optional>
#include <string_view>
#include <vector>

#include <boost/uuid/uuid.hpp>

#include <userver/components/component_base.hpp>
#include <userver/yaml_config/schema.hpp>
using Uuid = boost::uuids::uuid;

namespace ws {
namespace us = userver;
namespace datetime = us::utils::datetime;
enum class DenylistError;
class Config;

struct [[nodiscard]] ClientIpCooldown final {
    std::chrono::milliseconds retry_after;
};

struct [[nodiscard]] CaptureRecord final {
    Uuid uuid;
    datetime::TimePointTz created_at;
    String link;
    Url replay_url;
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
    [[nodiscard]] Expected<dto::UuidWithTimeLink, errors::CaptureError> CreateCapture(Link link);

    /**
     * @brief Enqueue a crawl job for the given link and return its job descriptor.
     *
     * Assigns a UUID that will also be used as the capture id once the job
     * succeeds. Job execution is scheduled asynchronously; callers should poll
     * job status via findCaptureJob().
     */
    [[nodiscard]] Expected<dto::CaptureJob, errors::CreateJobError> CreateCaptureJob(Link link);
    /** @brief Acquire per-IP cooldown for an HTTP CRUD operation. */
    [[nodiscard]] Expected<std::optional<ClientIpCooldown>, errors::CrudError>
    AcquireClientIpCooldown(String client_ip);
    /** @brief Look up capture metadata by id. */
    [[nodiscard]] Expected<std::optional<CaptureRecord>, errors::CrudError> FindCapture(Uuid uuid);

    /** @brief Look up a capture job by id. */
    [[nodiscard]] Expected<std::optional<dto::CaptureJob>, errors::CrudError>
    FindCaptureJob(Uuid uuid);

    /** @brief All capture ids for a link (newest first). */
    [[nodiscard]] Expected<std::vector<dto::UuidWithTime>, errors::CrudError>
    FindCapturesByLink(const Link &link);
    /** @brief Paged variant for capture ids by link. */
    [[nodiscard]] Expected<dto::PagedFindCapturesByUrlResponse, errors::CapturePageError>
    FindCapturesByLinkPage(const Link &link, String page_token);
    /** @brief Paged list of captures grouped by normalized link prefix. */
    [[nodiscard]] Expected<dto::PagedFindCapturesByPrefixResponse, errors::CapturePageError>
    FindCapturesByPrefixPage(String normalized_prefix, String page_token);
    /** @brief Disallow a prefix and enqueue purge of its captures. */
    [[nodiscard]] Expected<void, DenylistError> DisallowAndPurgePrefix(String prefix_key) noexcept;
    /** @brief Static config schema for this component. */
    [[nodiscard]] static us::yaml_config::Schema GetStaticConfigSchema();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace ws
