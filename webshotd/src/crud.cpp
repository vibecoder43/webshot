#include "crud.hpp"
/**
 * @file
 * @brief Implementation of storage and crawl orchestration.
 *
 * Implements the `Crud` component, including background crawl startup,
 * metadata writes, and various paged queries.
 */
#include "access_policy.hpp"
#include "capture_meta_repo.hpp"
#include "config.hpp"
#include "crawler/error.hpp"
#include "crawler/runner.hpp"
#include "grab_value.hpp"
#include "integers.hpp"
#include "invariant.hpp"
#include "link.hpp"
#include "metrics.hpp"
#include "pagination.hpp"
#include "prefix_pagination.hpp"
#include "prefix_utils.hpp"
#include "s3/client.hpp"
#include "s3/refresh_utils.hpp"
#include "s3/secdist.hpp"
#include "s3/sts_client.hpp"
#include "schema/common/common.hpp"
#include "schema/public/webshot.hpp"
#include "server_errors.hpp"
#include "shared_state_repo.hpp"
#include "text.hpp"
#include "try.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <format>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <boost/uuid/uuid.hpp>

#include <userver/clients/dns/component.hpp>
#include <userver/clients/http/component.hpp>
#include <userver/components/component.hpp>
#include <userver/components/component_base.hpp>
#include <userver/components/process_starter.hpp>
#include <userver/concurrent/background_task_storage.hpp>
#include <userver/engine/semaphore.hpp>
#include <userver/engine/sleep.hpp>
#include <userver/engine/task/cancel.hpp>
#include <userver/engine/task/current_task.hpp>
#include <userver/engine/task/task_processor_fwd.hpp>
#include <userver/formats/json.hpp>
#include <userver/logging/log.hpp>
#include <userver/rcu/rcu.hpp>
#include <userver/storages/postgres/io/bytea.hpp>
#include <userver/storages/postgres/io/chrono.hpp>
#include <userver/storages/postgres/io/row_types.hpp>
#include <userver/storages/postgres/io/uuid.hpp>
#include <userver/storages/postgres/transaction.hpp>
#include <userver/storages/secdist/component.hpp>
#include <userver/storages/secdist/secdist.hpp>
#include <userver/utils/assert.hpp>
#include <userver/utils/async.hpp>
#include <userver/utils/boost_uuid4.hpp>
#include <userver/utils/datetime.hpp>
#include <userver/utils/datetime/from_string_saturating.hpp>
#include <userver/utils/datetime/timepoint_tz.hpp>
#include <userver/utils/periodic_task.hpp>
#include <userver/yaml_config/merge_schemas.hpp>
#include <userver/yaml_config/yaml_config.hpp>

namespace ws {
namespace us = userver;
namespace eng = us::engine;
namespace datetime = us::utils::datetime;
namespace concurrent = us::concurrent;
namespace pg = us::storages::postgres;
namespace httpc = us::clients::http;
namespace rcu = us::rcu;
namespace chrono = std::chrono;
} // namespace ws

using namespace ws;
using namespace text::literals;
using namespace std::chrono_literals;
using Uuid = boost::uuids::uuid;
using chrono::system_clock;

namespace {
constexpr auto kCrawlerSeedAttemptsMax = 2;
constexpr i64 kGiB = 1024_i64 * 1024_i64 * 1024_i64;

using CaptureJobRow = SharedCrawlJobRow;

enum class JobStatus {
    kPending,
    kRunning,
    kSucceeded,
    kFailed,
};

enum class JobErrorKind {
    kSizeLimit,
    kCrawler,
    kS3Upload,
    kDbInsert,
    kInternalServer,
};

[[nodiscard]] String MakeCaptureObjectKey(const String &bucket, Uuid id)
{
    LOG_DEBUG() << text::Format("{}/{}", bucket, us::utils::ToString(id));
    return text::Format("{}/{}.wacz", bucket, us::utils::ToString(id));
}

[[nodiscard]] std::optional<crawler::CgroupLimits>
ComputeCrawlerLimits(i64 cpu_cores, i64 memory_gib)
{
    if (cpu_cores == 0_i64 && memory_gib == 0_i64)
        return {};
    Invariant(
        cpu_cores > 0_i64 && memory_gib > 0_i64, "crawler limits must be both > 0 or both 0"_t
    );
    return crawler::CgroupLimits{.cpu_cores = cpu_cores, .memory_bytes = memory_gib * kGiB};
}

template <typename F> [[nodiscard]] Expected<void, std::string> RunS3Operation(F &&operation)
{
    try {
        std::forward<F>(operation)();
        return {};
    } catch (const std::exception &e) {
        if (eng::current_task::IsCancelRequested())
            throw;
        return Unex(std::string(e.what()));
    }
}

[[nodiscard]] dto::CaptureJob
MakePendingCaptureJob(Uuid uuid, const String &link, const datetime::TimePointTz &created_at)
{
    return {
        .uuid = uuid,
        .link = link.ToBytes(),
        .status = dto::CaptureJob::Status::kPending,
        .created_at = created_at,
    };
}

[[nodiscard]] JobStatus ParseJobStatus(const std::string &status)
{
    if (status == "pending")
        return JobStatus::kPending;
    if (status == "running")
        return JobStatus::kRunning;
    if (status == "succeeded")
        return JobStatus::kSucceeded;
    if (status == "failed")
        return JobStatus::kFailed;
    Invariant(text::Format("unexpected crawl_job.status '{}'", status));
}

[[nodiscard]] std::optional<JobErrorKind>
ParseJobErrorKind(const std::optional<std::string> &error_category)
{
    if (!error_category)
        return {};
    if (*error_category == "size_limit")
        return JobErrorKind::kSizeLimit;
    if (*error_category == "crawler_failed")
        return JobErrorKind::kCrawler;
    if (*error_category == "s3_upload_failed")
        return JobErrorKind::kS3Upload;
    if (*error_category == "db_insert_failed")
        return JobErrorKind::kDbInsert;
    if (*error_category == "internal_server_error")
        return JobErrorKind::kInternalServer;
    Invariant(text::Format("unexpected crawl_job.error_category '{}'", *error_category));
}

[[nodiscard]] dto::CaptureJob::Status ToDtoJobStatus(JobStatus status)
{
    switch (status) {
    case JobStatus::kPending:
        return dto::CaptureJob::Status::kPending;
    case JobStatus::kRunning:
        return dto::CaptureJob::Status::kRunning;
    case JobStatus::kSucceeded:
        return dto::CaptureJob::Status::kSucceeded;
    case JobStatus::kFailed:
        return dto::CaptureJob::Status::kFailed;
    }
}

[[nodiscard]] std::string PublicJobErrorMessage(std::optional<JobErrorKind> error)
{
    if (!error)
        return "internal server error";

    using enum JobErrorKind;
    switch (*error) {
    case kSizeLimit:
        return "capture exceeded archive size limit";
    case kCrawler:
        return "capture failed";
    case kS3Upload:
    case kDbInsert:
    case kInternalServer:
        return "internal server error";
    }
}

[[nodiscard]] dto::CaptureJob MakeCaptureJob(CaptureJobRow row)
{
    auto status = ParseJobStatus(row.status);
    dto::CaptureJob job{
        .uuid = row.uuid,
        .link = row.link.ToBytes(),
        .status = ToDtoJobStatus(status),
        .created_at = datetime::TimePointTz{row.created_at.GetUnderlying()},
    };
    if (row.started_at)
        job.started_at = datetime::TimePointTz(row.started_at->GetUnderlying());
    if (row.finished_at)
        job.finished_at = datetime::TimePointTz(row.finished_at->GetUnderlying());
    if (row.result_created_at) {
        job.result_created_at = datetime::TimePointTz(row.result_created_at->GetUnderlying());
    }
    if (status == JobStatus::kFailed) {
        // Never expose internal diagnostics (crawler details, exception text, etc) to API clients.
        dto::ErrorEnvelope::Error err{PublicJobErrorMessage(ParseJobErrorKind(row.error_category))};
        job.error = dto::ErrorEnvelope{err};
    }
    if (status == JobStatus::kSucceeded && job.result_created_at) {
        Invariant(row.result_capture_id, "succeeded job must have result_capture_id"_t);
        job.result = dto::UuidWithTimeLink(
            *row.result_capture_id, *job.result_created_at, job.link
        );
    }
    return job;
}

[[nodiscard]] s3::Credentials MakeStaticS3Credentials(
    const s3::AccessKeyId &access_key_id, const s3::SecretAccessKey &secret_access_key
)
{
    return {access_key_id, secret_access_key, {}};
}

} // namespace

us::yaml_config::Schema Crud::GetStaticConfigSchema()
{
    return us::yaml_config::MergeSchemas<us::components::ComponentBase>(R"(
type: object
description: '.'
additionalProperties: false
properties:
    captures_page_max:
        type: integer
        minimum: 1
        description: '.'
    captures_per_link_max:
        type: integer
        minimum: 1
        description: 'Max captures per link in a prefix page'
    captures_links_per_page_max:
        type: integer
        minimum: 1
        description: 'Max distinct links in a prefix page'
    crawler_run_timeout_sec:
        type: integer
        minimum: 1
        description: 'Timeout sent to a single blocking crawler run in seconds'
    crawler_cpu_cores:
        type: integer
        minimum: 0
        description: 'Crawler browser CPU quota in whole cores; 0 disables limits'
    crawler_memory_gib:
        type: integer
        minimum: 0
        description: 'Crawler browser memory limit in GiB; 0 disables limits'
    crawler_job_overhead_timeout_sec:
        type: integer
        minimum: 1
        description: 'Extra timeout budget added around crawler runs for upload and metadata persistence'
    crawler_post_load_delay_sec:
        type: integer
        minimum: 0
        description: 'Extra delay after page load event before reading DOM and resources in seconds; 0 disables'
    crawler_net_idle_wait_sec:
        type: integer
        minimum: 0
        description: 'How long to wait for network to go idle after behaviors in seconds; 0 disables'
    crawler_page_extra_delay_sec:
        type: integer
        minimum: 0
        description: 'Extra delay after network idle wait in seconds; 0 disables'
    crawler_behavior_timeout_sec:
        type: integer
        minimum: 0
        description: 'Upper bound for running site behavior JS in seconds; 0 disables'
    crawler_devtools_startup_timeout_sec:
        type: integer
        minimum: 1
        description: 'How long to wait for Chromium to expose devtools socket and websocket path in seconds'
    crawler_cdp_handshake_timeout_sec:
        type: integer
        minimum: 1
        description: 'Upper bound for devtools websocket handshake in seconds'
    crawler_cdp_command_timeout_sec:
        type: integer
        minimum: 1
        description: 'Upper bound for a single CDP command round-trip in seconds'
    crawler_size_limit_mib:
        type: integer
        minimum: 1
        description: 'Max WACZ archive size limit in MiB per capture'
    crawler_network_down_bytes_ratio_max:
        type: integer
        minimum: 1
        description: 'Max proxy downstream bytes per capture as a multiple of final WACZ bytes'
    crawler_local_fixture_rewrite:
        type: boolean
        description: 'Rewrite local test fixture hosts (test-target) to 127.0.0.1:18080/18443 for the crawler proxy'
    crawler_devtools_poll_interval_ms:
        type: integer
        minimum: 1
        description: 'Polling interval for devtools socket/path discovery in milliseconds'
    crawler_browser_stop_timeout_ms:
        type: integer
        minimum: 1
        description: 'Grace timeout after SIGTERM before SIGKILL for the Chromium process in milliseconds'
    crawler_proxy_stop_timeout_ms:
        type: integer
        minimum: 1
        description: 'Grace timeout after SIGTERM before SIGKILL for the proxy bridge process in milliseconds'
    s3_credentials_endpoint:
        type: string
        description: 'STS url used to obtain temporary S3 credentials (required only when s3_use_sts=true); S3 data url s3_endpoint (in config) must be http(s)://host[:port] with optional trailing slash and no additional path or query'
    s3_use_sts:
        type: boolean
        description: 'Whether to fetch temporary S3 credentials from STS (true) or use static credentials from secdist (false)'
    s3_credentials_duration_sec:
        type: integer
        minimum: 1
        description: 'Requested lifetime of temporary S3 credentials in seconds'
    s3_credentials_refresh_margin_sec:
        type: integer
        minimum: 1
        description: 'How many seconds before expiration to refresh S3 credentials'
    s3_credentials_refresh_retry_sec:
        type: integer
        minimum: 1
        description: 'Delay between failed S3 credential refresh attempts in seconds'
    link_ratelimit_sec:
        type: integer
        minimum: 0
        description: 'Per-link minimum interval between capture jobs in seconds; 0 disables ratelimit'
    crawl_job_retention_sec:
        type: integer
        minimum: 1
        description: 'Retention window for crawl_job rows in seconds'
    crawl_job_cleanup_interval_sec:
        type: integer
        minimum: 1
        description: 'Interval between crawl_job cleanup passes in seconds'
    purge_job_timeout_sec:
        type: integer
        minimum: 1
        description: 'Upper bound for a single purge job in seconds'
    purge_delete_batch_size:
        type: integer
        minimum: 1
        description: 'Number of objects to delete per purge batch'
	)");
}

struct [[nodiscard]] CrawlContext;
struct [[nodiscard]] StoredCapture;

/** @brief Private pimpl that holds dependencies and query helpers. */
class [[nodiscard]] Crud::Impl {
public:
    const i64 page_max;
    const i64 per_link_max;
    const i64 links_per_page_max;
    const chrono::seconds crawler_run_timeout;
    const i64 crawler_cpu_cores;
    const i64 crawler_memory_gib;
    const chrono::seconds crawler_job_overhead_timeout;
    const chrono::seconds crawler_post_load_delay;
    const chrono::seconds crawler_net_idle_wait;
    const chrono::seconds crawler_page_extra_delay;
    const chrono::seconds crawler_behavior_timeout;
    const chrono::seconds crawler_devtools_startup_timeout;
    const chrono::seconds crawler_cdp_handshake_timeout;
    const chrono::seconds crawler_cdp_command_timeout;
    const i64 crawler_size_limit_mi_b;
    const i64 crawler_network_down_bytes_ratio_max;
    const bool crawler_local_fixture_rewrite;
    const chrono::milliseconds crawler_devtools_poll_interval;
    const chrono::milliseconds crawler_browser_stop_timeout;
    const chrono::milliseconds crawler_proxy_stop_timeout;
    const chrono::seconds link_ratelimit;
    const chrono::seconds crawl_job_retention;
    const chrono::seconds crawl_job_cleanup_interval;
    const bool s3_use_sts;
    const std::optional<String> s3_credentials_endpoint;
    const chrono::seconds s3_credentials_duration;
    const chrono::seconds s3_credentials_refresh_margin;
    const chrono::seconds s3_credentials_refresh_retry;
    const chrono::seconds purge_job_timeout;
    const i64 purge_delete_batch_size;
    const Config &svc_cfg;
    Metrics &metrics;
    CaptureMetaRepo &capture_meta_repo;
    SharedStateRepo &shared_state_repo;
    httpc::Client &http_client_;
    us::clients::dns::Resolver &dns_resolver_;
    eng::subprocess::ProcessStarter &process_starter_;
    eng::TaskProcessor &fs_task_processor_;
    AccessPolicyStore &access_policy;
    CrawlerRunner crawler_runner;
    struct [[nodiscard]] S3ClientState {
        s3::Credentials creds;
        system_clock::time_point expires_at;
        std::shared_ptr<s3::Client> client;
    };
    rcu::Variable<S3ClientState> s3_state;
    s3::AccessKeyId static_access_key_id;
    s3::SecretAccessKey static_secret_access_key;
    eng::TaskProcessor &main_task_processor;
    eng::CancellableSemaphore crawl_slots;
    eng::TaskProcessor &purge_task_processor;
    eng::TaskProcessor &creds_refresh_task_processor;
    // must die first
    us::utils::PeriodicTask s3_refresh_task;
    us::utils::PeriodicTask crawl_job_cleanup_task;
    concurrent::BackgroundTaskStorage purge_background;
    concurrent::BackgroundTaskStorage crawl_background;

    [[nodiscard]] Expected<dto::UuidWithTimeLink, errors::CaptureError>
    RunCrawlJob(Uuid id, Link link);
    [[nodiscard]] Expected<datetime::TimePointTz, PgError> InsertJob(Uuid id, String link);
    [[nodiscard]] Expected<std::optional<dto::CaptureJob>, PgError>
    FindLatestJobForLink(const String &link);
    [[nodiscard]] Expected<SharedStateRepo::MakeOrReuseCrawlJobResult, PgError>
    MakeOrReuseCaptureJobLocked(const String &normalized_link);
    [[nodiscard]] Expected<void, PgError> MarkJobRunning(Uuid id);
    [[nodiscard]] Expected<chrono::milliseconds, PgError>
    MarkJobSucceeded(Uuid id, Uuid result_capture_id, const datetime::TimePointTz &created_at);
    [[nodiscard]] Expected<chrono::milliseconds, PgError>
    MarkJobFailed(Uuid id, const String &error_category, const String &error_message);
    [[nodiscard]] Expected<std::optional<dto::CaptureJob>, PgError> LoadJob(Uuid id);
    [[nodiscard]] Expected<void, errors::CaptureError> RunCrawlerForContext(CrawlContext &ctx);
    [[nodiscard]] CrawlerRunArtifacts RunCrawlerAttempt(const String &seed_url);
    [[nodiscard]] std::optional<StoredCapture> PersistMetadataForContext(CrawlContext &ctx);
    [[nodiscard]] Expected<void, errors::CrudError> PurgePrefix(const String &prefix_key);
    [[nodiscard]] Expected<S3ClientState, std::string> FetchS3ClientStateFromSts();
    [[nodiscard]] Expected<void, std::string>
    PutCaptureObject(String key, const std::string &bytes);
    [[nodiscard]] Expected<void, std::string> DeleteCaptureObject(String key);
    void StartS3RefreshTask();
    void RefreshS3CredentialsTask();
    void StartCrawlJobCleanupTask();
    void CleanupOldJobs();
    explicit Impl(
        const us::components::ComponentConfig &cfg, const us::components::ComponentContext &ctx
    )
        : page_max(cfg["captures_page_max"].As<int64_t>()),
          per_link_max(cfg["captures_per_link_max"].As<int64_t>()),
          links_per_page_max(cfg["captures_links_per_page_max"].As<int64_t>()),
          crawler_run_timeout(cfg["crawler_run_timeout_sec"].As<int64_t>() * 1s),
          crawler_cpu_cores(cfg["crawler_cpu_cores"].As<int64_t>()),
          crawler_memory_gib(cfg["crawler_memory_gib"].As<int64_t>()),
          crawler_job_overhead_timeout(cfg["crawler_job_overhead_timeout_sec"].As<int64_t>() * 1s),
          crawler_post_load_delay(cfg["crawler_post_load_delay_sec"].As<int64_t>() * 1s),
          crawler_net_idle_wait(cfg["crawler_net_idle_wait_sec"].As<int64_t>() * 1s),
          crawler_page_extra_delay(cfg["crawler_page_extra_delay_sec"].As<int64_t>() * 1s),
          crawler_behavior_timeout(cfg["crawler_behavior_timeout_sec"].As<int64_t>() * 1s),
          crawler_devtools_startup_timeout(
              cfg["crawler_devtools_startup_timeout_sec"].As<int64_t>() * 1s
          ),
          crawler_cdp_handshake_timeout(
              cfg["crawler_cdp_handshake_timeout_sec"].As<int64_t>() * 1s
          ),
          crawler_cdp_command_timeout(cfg["crawler_cdp_command_timeout_sec"].As<int64_t>() * 1s),
          crawler_size_limit_mi_b(cfg["crawler_size_limit_mib"].As<int64_t>()),
          crawler_network_down_bytes_ratio_max(
              cfg["crawler_network_down_bytes_ratio_max"].As<int64_t>()
          ),
          crawler_local_fixture_rewrite(cfg["crawler_local_fixture_rewrite"].As<bool>()),
          crawler_devtools_poll_interval(
              cfg["crawler_devtools_poll_interval_ms"].As<int64_t>() * 1ms
          ),
          crawler_browser_stop_timeout(cfg["crawler_browser_stop_timeout_ms"].As<int64_t>() * 1ms),
          crawler_proxy_stop_timeout(cfg["crawler_proxy_stop_timeout_ms"].As<int64_t>() * 1ms),
          link_ratelimit(cfg["link_ratelimit_sec"].As<int64_t>() * 1s),
          crawl_job_retention(cfg["crawl_job_retention_sec"].As<int64_t>() * 1s),
          crawl_job_cleanup_interval(cfg["crawl_job_cleanup_interval_sec"].As<int64_t>() * 1s),
          s3_use_sts(cfg["s3_use_sts"].As<bool>()),
          s3_credentials_endpoint([&cfg] -> std::optional<String> {
              if (!cfg["s3_use_sts"].As<bool>())
                  return {};
              return *String::FromBytes(cfg["s3_credentials_endpoint"].As<std::string>());
          }()),
          s3_credentials_duration(cfg["s3_credentials_duration_sec"].As<int64_t>() * 1s),
          s3_credentials_refresh_margin(
              cfg["s3_credentials_refresh_margin_sec"].As<int64_t>() * 1s
          ),
          s3_credentials_refresh_retry(cfg["s3_credentials_refresh_retry_sec"].As<int64_t>() * 1s),
          purge_job_timeout(cfg["purge_job_timeout_sec"].As<int64_t>() * 1s),
          purge_delete_batch_size(cfg["purge_delete_batch_size"].As<int64_t>()),
          svc_cfg(ctx.FindComponent<Config>()), metrics(ctx.FindComponent<Metrics>()),
          capture_meta_repo(ctx.FindComponent<CaptureMetaRepo>()),
          shared_state_repo(ctx.FindComponent<SharedStateRepo>()),
          http_client_(ctx.FindComponent<us::components::HttpClient>().GetHttpClient()),
          dns_resolver_(ctx.FindComponent<us::clients::dns::Component>().GetResolver()),
          process_starter_(ctx.FindComponent<us::components::ProcessStarter>().Get()),
          fs_task_processor_(ctx.GetTaskProcessor("fs-task-processor")),
          access_policy(ctx.FindComponent<AccessPolicyStore>()),
          crawler_runner(
              access_policy, svc_cfg, dns_resolver_, process_starter_, crawler_run_timeout,
              fs_task_processor_, std::string(svc_cfg.StateDir()),
              ComputeCrawlerLimits(crawler_cpu_cores, crawler_memory_gib),
              crawler_size_limit_mi_b * 1024_i64 * 1024_i64,
              crawler::CaptureTimings{
                  crawler_post_load_delay,
                  crawler_net_idle_wait,
                  crawler_page_extra_delay,
                  crawler_behavior_timeout,
              },
              crawler::CrawlerTunables{
                  crawler_devtools_startup_timeout,
                  crawler_cdp_handshake_timeout,
                  crawler_cdp_command_timeout,
                  crawler_devtools_poll_interval,
                  crawler_browser_stop_timeout,
                  crawler_proxy_stop_timeout,
                  crawler_local_fixture_rewrite,
              },
              crawler_network_down_bytes_ratio_max, metrics
          ),
          main_task_processor(ctx.GetTaskProcessor("main-task-processor")),
          crawl_slots(eng::GetWorkerCount(main_task_processor)),
          purge_task_processor(ctx.GetTaskProcessor("purge_task_processor")),
          creds_refresh_task_processor(ctx.GetTaskProcessor("creds_refresh_task_processor")),
          s3_refresh_task(), crawl_job_cleanup_task(), purge_background(purge_task_processor),
          crawl_background(main_task_processor)
    {
        const auto fixed_timing_budget = crawler_post_load_delay + crawler_net_idle_wait +
                                         crawler_page_extra_delay + crawler_behavior_timeout;
        Invariant(
            fixed_timing_budget <= crawler_run_timeout,
            "crawler fixed timing budget must be <= crawler_run_timeout_sec"_t
        );
        Invariant(
            crawl_job_retention >= link_ratelimit,
            "crawl_job_retention_sec must be >= link_ratelimit_sec"_t
        );
        Invariant(
            s3_credentials_duration > s3_credentials_refresh_margin,
            "s3_credentials_duration_sec must be greater than s3_credentials_refresh_margin_sec"_t
        );
        const auto &secdist = ctx.FindComponent<us::components::Secdist>().Get();
        const auto &creds = secdist.Get<CredentialsSecdist>();
        Invariant(
            creds.access_key_id && creds.secret_access_key,
            "missing required S3 secdist credentials"_t
        );
        static_access_key_id = *creds.access_key_id;
        static_secret_access_key = *creds.secret_access_key;
        S3ClientState initial_state;
        if (s3_use_sts) {
            auto fetched_state = FetchS3ClientStateFromSts();
            if (!fetched_state) {
                metrics.AccountError(Metrics::Error::kStsRefresh);
                us::utils::AbortWithStacktrace(fetched_state.Error());
            }
            initial_state = GrabValueOf(std::move(fetched_state));
            StartS3RefreshTask();
        } else {
            auto static_creds = MakeStaticS3Credentials(
                static_access_key_id, static_secret_access_key
            );
            initial_state = S3ClientState{
                .creds = static_creds,
                .expires_at = system_clock::time_point::max(),
                .client = std::make_shared<s3::Client>(
                    http_client_,
                    s3::Config(
                        svc_cfg.S3Endpoint(), svc_cfg.S3Region(), svc_cfg.S3Timeout(), false
                    ),
                    static_creds, String{}
                ),
            };
        }
        s3_state.Assign(initial_state);
        StartCrawlJobCleanupTask();
    }
};

Crud::Crud(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : us::components::ComponentBase(config, context),
      impl_(std::make_unique<Crud::Impl>(config, context))
{
}

Crud::~Crud() = default;

/** Lightweight context shared across steps of a single crawl job. */
struct [[nodiscard]] CrawlContext {
    Link link;
    Uuid id;
    String s3_key;
    std::optional<std::string> wacz_bytes;
    std::optional<std::string> content_sha256;
    std::optional<Url> replay_url;
    std::optional<String> error_message;

    CrawlContext(Uuid id, Link link, const Config &cfg)
        : link(std::move(link)), id(id), s3_key(MakeCaptureObjectKey(cfg.S3Bucket(), id))
    {
    }
};

struct [[nodiscard]] StoredCapture {
    Uuid id;
    datetime::TimePointTz created_at;
};

[[nodiscard]] Expected<dto::UuidWithTimeLink, errors::CaptureError>
Crud::Impl::RunCrawlJob(Uuid id, Link link)
{
    using enum errors::CaptureErrorKind;

    const auto total_crawl_timeout_budget = crawler_job_overhead_timeout +
                                            crawler_run_timeout * kCrawlerSeedAttemptsMax;
    eng::current_task::SetDeadline(eng::Deadline::FromDuration(total_crawl_timeout_budget));

    std::shared_lock<eng::CancellableSemaphore> slot_lock(crawl_slots);

    CrawlContext ctx(id, std::move(link), svc_cfg);

    auto id_str = us::utils::ToString(id);
    LOG_INFO() << std::format(
        "RunCrawlJob starting crawler for job {} ({})", id_str, ctx.link.ToKey()
    );
    {
        auto crawler_result = RunCrawlerForContext(ctx);
        if (!crawler_result)
            metrics.AccountError(Metrics::Error::kCrawlerRun);
        TRY(std::move(crawler_result));
    }
    LOG_INFO() << std::format(
        "RunCrawlJob finished crawler for job {} ({})", id_str, ctx.link.ToKey()
    );

    LOG_INFO() << std::format("Persisting metadata for job {} ({})", id_str, ctx.link.ToKey());
    auto stored = PersistMetadataForContext(ctx);
    if (!stored)
        return Unex(errors::CaptureError{.kind = kPersistMetadataFailed, .detail = {}});
    LOG_INFO() << std::format("Persisted metadata for job {} ({})", id_str, ctx.link.ToKey());
    return dto::UuidWithTimeLink{stored->id, stored->created_at, ctx.link.ToKey().ToBytes()};
}

Expected<datetime::TimePointTz, PgError> Crud::Impl::InsertJob(Uuid id, String link)
{
    return shared_state_repo.InsertCrawlJob(id, link);
}

Expected<void, PgError> Crud::Impl::MarkJobRunning(Uuid id)
{
    return shared_state_repo.MarkCrawlJobRunning(id);
}

Expected<chrono::milliseconds, PgError> Crud::Impl::MarkJobSucceeded(
    Uuid id, Uuid result_capture_id, const datetime::TimePointTz &created_at
)
{
    return shared_state_repo.MarkCrawlJobSucceeded(id, result_capture_id, created_at);
}

Expected<chrono::milliseconds, PgError>
Crud::Impl::MarkJobFailed(Uuid id, const String &error_category, const String &error_message)
{
    return shared_state_repo.MarkCrawlJobFailed(id, error_category, error_message);
}

Expected<std::optional<dto::CaptureJob>, PgError> Crud::Impl::LoadJob(Uuid id)
{
    auto row_opt = TRY(shared_state_repo.LoadCrawlJob(id));
    auto row = TRY(row_opt);
    return ::MakeCaptureJob(std::move(row));
}

Expected<Crud::Impl::S3ClientState, std::string> Crud::Impl::FetchS3ClientStateFromSts()
{
    ENSURE(
        s3_credentials_endpoint,
        std::string{"missing s3_credentials_endpoint while s3_use_sts is enabled"}
    );
    const auto session_uuid = text::Format("{}", us::utils::generators::GenerateBoostUuid());
    auto session_name = text::Format("{}", session_uuid);
    auto role_arn_description = "ephemeral_s3_credentials"_t;

    const auto policy_json = text::Format(
        "{{\"Version\":\"2012-10-17\",\"Statement\":{{\"Sid\":\"access\",\"Effect\":"
        "\"Allow\",\"Principal\":\"*\",\"Action\":[\"s3:PutObject\",\"s3:DeleteObject\","
        "\"s3:GetObject\"],\"Resource\":\"arn:aws:s3:::{}/*\"}}}}",
        svc_cfg.S3Bucket()
    );

    const auto sts = FetchStsCredentials(
        http_client_, *s3_credentials_endpoint, static_access_key_id, static_secret_access_key,
        svc_cfg.S3Region(), role_arn_description, session_name, policy_json,
        s3_credentials_duration, svc_cfg.S3Timeout()
    );
    if (!sts)
        return Unex(std::string{"failed to fetch STS credentials"});

    const auto creds = s3::Credentials(
        sts->access_key_id, sts->secret_access_key, sts->session_token
    );
    return S3ClientState{
        .creds = creds,
        .expires_at = sts->expires_at,
        .client = std::make_shared<s3::Client>(
            http_client_,
            s3::Config(svc_cfg.S3Endpoint(), svc_cfg.S3Region(), svc_cfg.S3Timeout(), false), creds,
            String()
        ),
    };
}

Expected<void, std::string> Crud::Impl::PutCaptureObject(String key, const std::string &bytes)
{
    return RunS3Operation([&] {
        auto snapshot = s3_state.Read();
        snapshot->client->PutObject(key.View(), bytes, {}, "application/wacz", {}, {});
    });
}

Expected<void, std::string> Crud::Impl::DeleteCaptureObject(String key)
{
    return RunS3Operation([&] {
        auto snapshot = s3_state.Read();
        snapshot->client->DeleteObject(key.View());
    });
}

Expected<std::optional<dto::CaptureJob>, PgError>
Crud::Impl::FindLatestJobForLink(const String &link)
{
    auto row_opt = TRY(shared_state_repo.FindLatestCrawlJobForLink(link));
    auto row = TRY(row_opt);
    return ::MakeCaptureJob(std::move(row));
}

Expected<SharedStateRepo::MakeOrReuseCrawlJobResult, PgError>
Crud::Impl::MakeOrReuseCaptureJobLocked(const String &normalized_link)
{
    auto result = TRY(shared_state_repo.MakeOrReuseCrawlJobLocked(normalized_link, link_ratelimit));
    if (result.created)
        metrics.AccountCaptureJobCreated();
    return result;
}

void Crud::Impl::StartS3RefreshTask()
{
    auto snapshot = s3_state.Read();
    auto now = datetime::Now();
    auto delay = s3refresh::ComputeRefreshDelay(
        now, snapshot->expires_at, s3_credentials_refresh_margin
    );

    us::utils::PeriodicTask::Settings settings(
        chrono::duration_cast<chrono::milliseconds>(delay), 0ms
    );
    settings.task_processor = &creds_refresh_task_processor;

    s3_refresh_task.Start("s3_credentials_refresh", settings, [this]() {
        RefreshS3CredentialsTask();
    });
}

void Crud::Impl::RefreshS3CredentialsTask()
{
    for (;;) {
        if (eng::current_task::ShouldCancel())
            return;
        auto new_state = FetchS3ClientStateFromSts();
        if (new_state) {
            s3_state.Assign(*new_state);

            auto now = datetime::Now();
            auto next_delay = s3refresh::ComputeRefreshDelay(
                now, new_state->expires_at, s3_credentials_refresh_margin
            );

            us::utils::PeriodicTask::Settings settings(
                chrono::duration_cast<chrono::milliseconds>(next_delay), 0ms
            );
            settings.task_processor = &creds_refresh_task_processor;
            s3_refresh_task.SetSettings(settings);
            break;
        }
        metrics.AccountError(Metrics::Error::kStsRefresh);
        LOG_ERROR() << std::format(
            "Failed to refresh S3 credentials from STS: {}", new_state.Error()
        );
        eng::SleepFor(s3_credentials_refresh_retry);
    }
}

void Crud::Impl::StartCrawlJobCleanupTask()
{
    auto interval = crawl_job_cleanup_interval;
    us::utils::PeriodicTask::Settings settings(interval, 0ms);
    settings.task_processor = &purge_task_processor;

    crawl_job_cleanup_task.Start("crawl_job_cleanup", settings, [this]() { CleanupOldJobs(); });
}

void Crud::Impl::CleanupOldJobs()
{
    auto now = datetime::Now();
    auto cutoff = now - crawl_job_retention;
    auto deleted = shared_state_repo.DeleteCrawlJobsExpired(datetime::TimePointTz{cutoff});
    if (!deleted) {
        LOG_ERROR() << std::format("Failed to delete old crawl jobs: {}", deleted.Error().what);
    }
}

CrawlerRunArtifacts Crud::Impl::RunCrawlerAttempt(const String &seed_url)
{
    LOG_INFO() << std::format(
        "Submitting crawl for {} to crawler with timeout={}s", seed_url, crawler_run_timeout.count()
    );

    auto run = crawler_runner.Run(seed_url);
    LOG_INFO() << std::format(
        "Crawler returned for {} (error={}, wacz_exists={})", seed_url,
        run.error ? "true" : "false", run.wacz ? "true" : "false"
    );
    if (run.error) {
        LOG_INFO() << std::format(
            "Crawler error for {} ({})", seed_url, crawler::FormatCrawlerError(*run.error)
        );
    }
    return run;
}

[[nodiscard]] errors::CaptureError MakeCaptureError(const crawler::CrawlerError &error)
{
    using enum crawler::CrawlerErrorKind;
    using enum errors::CaptureErrorKind;

    return {
        .kind = error.kind == kArchiveSizeLimit ? kSizeLimit : kCrawler,
        .detail = crawler::FormatCrawlerError(error),
    };
}

Expected<void, errors::CaptureError> Crud::Impl::RunCrawlerForContext(CrawlContext &ctx)
{
    const auto https_seed_url = ctx.link.HttpsUrl();
    auto http_seed_url = ctx.link.HttpUrl();

    const auto try_store_success = [&ctx](const CrawlerRunArtifacts &run) -> bool {
        if (run.error)
            return false;
        Invariant(run.wacz, "crawler succeeded but did not provide WACZ bytes"_t);
        Invariant(
            run.content_sha256, "crawler did not provide content hash for a successful capture"_t
        );
        Invariant(ssize(*run.content_sha256) == 32_i64, "content hash must be 32 bytes"_t);
        Invariant(run.replay_url, "crawler did not provide replay_url for a successful capture"_t);
        auto replay_url = Url::FromText(*run.replay_url);
        ctx.wacz_bytes = *run.wacz;
        ctx.content_sha256 = *run.content_sha256;
        ctx.replay_url = *replay_url;
        return true;
    };

    const auto https_run = RunCrawlerAttempt(https_seed_url);
    if (try_store_success(https_run))
        return {};
    Invariant(https_run.error, "unsuccessful crawler run must include error"_t);
    if (svc_cfg.HttpsOnly()) {
        ctx.error_message = text::Format("Crawler error for {}", ctx.link.ToKey());
        LOG_INFO() << ctx.error_message;
        return Unex(MakeCaptureError(*https_run.error));
    }
    if (!crawler::ShouldRetryWithHttp(*https_run.error)) {
        ctx.error_message = text::Format("Crawler error for {}", ctx.link.ToKey());
        LOG_INFO() << ctx.error_message;
        return Unex(MakeCaptureError(*https_run.error));
    }

    LOG_INFO() << std::format(
        "Trying HTTP fallback for {} after HTTPS error ({})", http_seed_url,
        crawler::FormatCrawlerError(*https_run.error)
    );

    const auto http_run = RunCrawlerAttempt(http_seed_url);
    if (try_store_success(http_run)) {
        LOG_INFO() << std::format(
            "HTTP fallback succeeded for {} after HTTPS error ({})", http_seed_url,
            crawler::FormatCrawlerError(*https_run.error)
        );
        return {};
    }
    Invariant(http_run.error, "unsuccessful HTTP fallback must include error"_t);
    ctx.error_message = text::Format("Crawler error for {}", ctx.link.ToKey());
    LOG_INFO() << ctx.error_message;
    return Unex(MakeCaptureError(*http_run.error));
}

std::optional<StoredCapture> Crud::Impl::PersistMetadataForContext(CrawlContext &ctx)
{
    const auto prefix_key = prefix::MakePrefixKey(ctx.link);
    auto prefix_tree = prefix::MakePrefixTree(prefix_key);
    Invariant(ctx.replay_url, "replayUrl must be set for a successful capture"_t);

    const auto allowed = access_policy.IsAllowedPrefix(prefix_key);
    if (!allowed || !*allowed) {
        if (!allowed) {
            metrics.AccountError(Metrics::Error::kAccessPolicyCheck);
            LOG_ERROR() << std::format(
                "Failed to check access policy state during crawl: {}", prefix_key
            );
        } else {
            LOG_INFO() << std::format("Prefix became denylisted during crawl: {}", prefix_key);
        }
        return {};
    }

    Invariant(ctx.wacz_bytes, "persistMetadataForContext called without WACZ bytes"_t);
    Invariant(ctx.content_sha256, "persistMetadataForContext called without content hash"_t);
    Invariant(ssize(*ctx.content_sha256) == 32_i64, "content hash must be 32 bytes"_t);

    auto existing = capture_meta_repo.FindCaptureByLinkHash(
        ctx.link.ToKey(), std::string_view{*ctx.content_sha256}
    );
    if (!existing) {
        LOG_ERROR() << std::format(
            "DB select capture-by-hash failed for {}: {}", ctx.link.ToKey(), existing.Error().what
        );
        return {};
    }
    if (*existing) {
        auto row = **existing;
        return StoredCapture{
            .id = row.uuid,
            .created_at = datetime::TimePointTz{row.created_at.GetUnderlying()},
        };
    }

    const auto uploaded = PutCaptureObject(ctx.s3_key, *ctx.wacz_bytes);
    if (!uploaded) {
        metrics.AccountError(Metrics::Error::kS3PutObject);
        LOG_ERROR() << std::format("S3 upload failed for {}: {}", ctx.s3_key, uploaded.Error());
        return {};
    }

    auto row = capture_meta_repo.InsertCapture(
        ctx.id, ctx.link.ToKey(), prefix_key, prefix_tree, std::string_view{*ctx.content_sha256},
        ctx.replay_url->Href()
    );
    if (!row) {
        auto deleted = DeleteCaptureObject(ctx.s3_key);
        if (!deleted) {
            metrics.AccountError(Metrics::Error::kS3DeleteObject);
            LOG_ERROR() << std::format("error deleting {}", ctx.s3_key);
        }
        LOG_ERROR() << std::format("DB insert failed for {}: {}", ctx.id, row.Error().what);
        return {};
    }
    return StoredCapture{
        .id = ctx.id,
        .created_at = datetime::TimePointTz{row->created_at.GetUnderlying()},
    };
}

Expected<void, errors::CrudError> Crud::Impl::PurgePrefix(const String &prefix_key)
{
    using enum errors::CrudError;

    const auto tree = prefix::MakePrefixTree(prefix_key);
    while (true) {
        auto ids = capture_meta_repo.GetCaptureIdsByPrefixTree(tree, purge_delete_batch_size);
        if (!ids) {
            LOG_ERROR() << std::format(
                "access policy purge failed for {}: {}", prefix_key, ids.Error().what
            );
            return Unex(kDbError);
        }
        if (ids->empty())
            break;

        std::vector<Uuid> single;
        single.reserve(1);
        for (auto &&id : *ids) {
            auto key = MakeCaptureObjectKey(svc_cfg.S3Bucket(), id);
            auto s3_deleted = DeleteCaptureObject(key);
            if (!s3_deleted) {
                metrics.AccountError(Metrics::Error::kS3DeleteObject);
                LOG_ERROR() << std::format(
                    "S3 delete failed for key {} (prefix={}): {}", key, prefix_key,
                    s3_deleted.Error()
                );
                return Unex(kDbError);
            }

            single.clear();
            single.emplace_back(id);
            auto db_deleted = capture_meta_repo.DeleteCapturesByIds(single);
            if (!db_deleted) {
                LOG_ERROR() << std::format(
                    "access policy purge failed for {}: {}", prefix_key, db_deleted.Error().what
                );
                return Unex(kDbError);
            }
        }
    }
    return {};
}

Expected<dto::UuidWithTimeLink, errors::CaptureError> Crud::MakeCapture(Link link)
{
    auto *impl_ptr = impl_.get();
    auto id = us::utils::generators::GenerateBoostUuid();
    return us::utils::Async(
               "create_capture",
               [impl_ptr, id, link = std::move(link)]() { return impl_ptr->RunCrawlJob(id, link); }
    ).Get();
}

Expected<dto::CaptureJob, errors::CreateJobError> Crud::MakeCaptureJob(Link link)
{
    using enum errors::CreateJobError;

    auto *impl_ptr = impl_.get();
    const auto normalized_link = link.ToKey();
    dto::CaptureJob job;
    Uuid id;

    if (impl_ptr->link_ratelimit > 0s) {
        auto capture_job_result = impl_ptr->MakeOrReuseCaptureJobLocked(normalized_link);
        if (!capture_job_result)
            LOG_ERROR() << std::format(
                "Failed to create or reuse crawl job for {}: {}", normalized_link,
                capture_job_result.Error().what
            );
        auto result = TRY_ERR_AS(std::move(capture_job_result), kDbError);
        job = ::MakeCaptureJob(std::move(result.job));
        if (!result.created)
            return job;
        id = job.uuid;
    } else {
        id = us::utils::generators::GenerateBoostUuid();
        auto insert_result = impl_ptr->InsertJob(id, normalized_link);
        if (!insert_result)
            LOG_ERROR() << std::format(
                "Failed to create crawl job for {}: {}", normalized_link, insert_result.Error().what
            );
        auto created_at = TRY_ERR_AS(std::move(insert_result), kDbError);
        impl_ptr->metrics.AccountCaptureJobCreated();
        job = MakePendingCaptureJob(id, normalized_link, created_at);
    }
    impl_ptr->crawl_background.AsyncDetach("crawl_job", [impl_ptr, id, link = std::move(link)]() {
        const auto mark_failed = [&](const String &error_category, const String &error_message) {
            // Persist terminal job state even if the crawl deadline has already cancelled this
            // task.
            const eng::TaskCancellationBlocker blocker;
            return impl_ptr->MarkJobFailed(id, error_category, error_message);
        };
        const auto mark_succeeded = [&](Uuid result_capture_id,
                                        const datetime::TimePointTz &created_at_value) {
            const eng::TaskCancellationBlocker blocker;
            return impl_ptr->MarkJobSucceeded(id, result_capture_id, created_at_value);
        };
        const auto account_marked_error =
            [&](const Expected<chrono::milliseconds, PgError> &marked) {
                if (!marked) {
                    LOG_ERROR() << std::format(
                        "DB update crawl job failed for {}: {}", id, marked.Error().what
                    );
                } else {
                    impl_ptr->metrics.AccountCaptureCompleted(false, *marked);
                }
            };
        auto mark_internal_error = [&](std::string_view what) {
            LOG_ERROR() << std::format("Unexpected crawl job error for {}: {}", id, what);
            account_marked_error(mark_failed("internal_server_error"_t, "internal server error"_t));
        };
        auto mark_crawler_error = [&](const String &detail) {
            LOG_WARNING() << std::format("Crawl job error for {}: {}", id, detail);
            account_marked_error(mark_failed("crawler_failed"_t, detail));
        };

        try {
            const auto running = impl_ptr->MarkJobRunning(id);
            if (!running) {
                LOG_ERROR() << std::format(
                    "DB update crawl job failed for {}: {}", id, running.Error().what
                );
                return;
            }

            auto result = impl_ptr->RunCrawlJob(id, link);
            if (!result) {
                using enum errors::CaptureErrorKind;
                Expected<chrono::milliseconds, PgError> marked;
                if (result.Error().kind == kSizeLimit) {
                    marked = mark_failed("size_limit"_t, "capture exceeded archive size limit"_t);
                } else if (result.Error().kind == kPersistMetadataFailed) {
                    marked = mark_failed("internal_server_error"_t, "internal server error"_t);
                } else if (result.Error().detail) {
                    marked = mark_failed("crawler_failed"_t, *result.Error().detail);
                } else {
                    marked = mark_failed("crawler_failed"_t, "crawler error"_t);
                }
                account_marked_error(marked);
                return;
            }

            const auto succeeded = mark_succeeded(result->uuid, result->created_at);
            if (!succeeded) {
                LOG_ERROR() << std::format(
                    "DB update crawl job failed for {}: {}", id, succeeded.Error().what
                );
            } else {
                impl_ptr->metrics.AccountCaptureCompleted(true, *succeeded);
            }
        } catch (const std::exception &e) {
            if (eng::current_task::IsCancelRequested()) {
                mark_crawler_error(
                    text::Format(
                        "crawl job cancelled: {}",
                        eng::ToString(eng::current_task::CancellationReason())
                    )
                );
                throw;
            }
            mark_internal_error(e.what());
        }
    });

    return job;
}

Expected<std::optional<CaptureRecord>, errors::CrudError> Crud::FindCapture(Uuid uuid)
{
    using enum errors::CrudError;

    auto capture = impl_->capture_meta_repo.FindCapture(uuid);
    if (!capture) {
        LOG_ERROR() << std::format(
            "DB select capture failed for {}: {}", uuid, capture.Error().what
        );
        return Unex(kDbError);
    }
    auto capture_opt = GrabValueOf(capture);
    if (!capture_opt) {
        LOG_INFO() << std::format("UUID not found: {}", uuid);
        return {};
    }
    auto row = GrabValueOf(capture_opt);
    auto link_text = *String::FromBytes(row.link);
    auto replay_url_text = *String::FromBytes(row.replay_url);
    auto replay_url = *Url::FromText(replay_url_text);
    return {CaptureRecord{
        .uuid = uuid,
        .created_at = datetime::TimePointTz{row.created_at.GetUnderlying()},
        .link = link_text,
        .replay_url = replay_url,
    }};
}

Expected<std::optional<dto::CaptureJob>, errors::CrudError> Crud::FindCaptureJob(Uuid uuid)
{
    using enum errors::CrudError;

    auto job = impl_->LoadJob(uuid);
    if (!job) {
        LOG_ERROR() << std::format("DB select job failed for {}: {}", uuid, job.Error().what);
        return Unex(kDbError);
    }
    return *job;
}

Expected<dto::PagedFindCapturesByLinkResponse, errors::CapturePageError>
Crud::FindCapturesByLinkPage(const Link &link, String page_token)
{
    namespace crud = ws::crud;
    using enum errors::CapturePageError;

    const auto limit = impl_->page_max + 1_i64;
    std::optional<crud::Cursor> cur;
    if (!page_token.Empty()) {
        cur = crud::DecodeCursor(page_token);
        if (!cur)
            return Unex(kInvalidPageToken);
    }

    Expected<std::vector<CaptureMetaIdRow>, PgError> rows = [&]() {
        if (!cur) {
            return impl_->capture_meta_repo.GetCapturesByLinkFirst(link.ToKey(), limit);
        }
        if (cur->direction == crud::PageDirection::kPrevious) {
            return impl_->capture_meta_repo.GetCapturesByLinkPrev(
                link.ToKey(), limit, pg::TimePointTz{cur->created_at}, cur->id
            );
        }
        return impl_->capture_meta_repo.GetCapturesByLinkNext(
            link.ToKey(), limit, pg::TimePointTz{cur->created_at}, cur->id
        );
    }();

    if (!rows) {
        LOG_ERROR() << std::format("DB select captures page failed: {}", rows.Error().what);
        return Unex(kDbError);
    }
    auto db_rows = GrabValueOf(rows);
    bool has_previous = cur && cur->direction == crud::PageDirection::kNext;
    bool has_next = cur && cur->direction == crud::PageDirection::kPrevious;
    if (ssize(db_rows) > impl_->page_max) {
        db_rows.pop_back();
        if (cur && cur->direction == crud::PageDirection::kPrevious)
            has_previous = true;
        else
            has_next = true;
    }
    if (cur && cur->direction == crud::PageDirection::kPrevious)
        std::ranges::reverse(db_rows);

    std::vector<dto::UuidWithTime> items;
    items.reserve(db_rows.size());
    for (const auto &row : db_rows) {
        items.emplace_back(row.uuid, datetime::TimePointTz{row.created_at.GetUnderlying()});
    }

    std::optional<std::string> next;
    std::optional<std::string> previous;
    if (!items.empty()) {
        if (has_previous) {
            const auto &first = items.front();
            previous = crud::EncodeCursor(
                           first.created_at.GetTimePoint(), first.uuid,
                           crud::PageDirection::kPrevious
            )
                           .ToBytes();
        }
        if (has_next) {
            const auto &last = items.back();
            next = crud::EncodeCursor(
                       last.created_at.GetTimePoint(), last.uuid, crud::PageDirection::kNext
            )
                       .ToBytes();
        }
    }
    return dto::PagedFindCapturesByLinkResponse{
        .items = std::move(items),
        .next_page_token = std::move(next),
        .previous_page_token = std::move(previous),
    };
}

Expected<dto::PagedFindCapturesByPrefixResponse, errors::CapturePageError>
Crud::FindCapturesByPrefixPage(String normalized_prefix, String page_token)
{
    namespace crud = ws::crud;
    using enum errors::CapturePageError;

    std::optional<crud::PrefixCursor> cur;
    if (!page_token.Empty()) {
        cur = crud::DecodePrefixCursor(page_token);
        if (!cur)
            return Unex(kInvalidPageToken);
        if (cur->prefix != normalized_prefix)
            return Unex(kMismatchedPageToken);
    }
    const std::string upper = crud::UpperExclusiveBound(normalized_prefix);
    const auto links_per_page = impl_->links_per_page_max;

    auto select_links_first = [&](i64 limit) {
        return impl_->capture_meta_repo.GetDistinctLinksByPrefixFirst(
            normalized_prefix, upper, limit
        );
    };
    auto select_links_next = [&](const String &from_link, i64 limit) {
        return impl_->capture_meta_repo.GetDistinctLinksByPrefixNext(
            normalized_prefix, upper, from_link, limit
        );
    };
    auto select_links_previous = [&](const String &from_link, i64 limit) {
        return impl_->capture_meta_repo.GetDistinctLinksByPrefixPrev(
            normalized_prefix, upper, from_link, limit
        );
    };
    auto has_rows_before_in_link =
        [&](const String &link, const crud::PrefixCursor &cursor) -> Expected<bool, PgError> {
        return impl_->capture_meta_repo.HasRowsBeforeInLink(
            link, pg::TimePointTz{*cursor.created_at}, *cursor.id
        );
    };

    std::vector<String> links;
    links.reserve(NumericCast<size_t>(links_per_page + 1_i64));
    bool has_more_previous_links = false;
    bool has_more_next_links = false;
    if (cur) {
        const auto &cursor_link = cur->link;
        if (cur->direction == crud::PageDirection::kPrevious) {
            bool include_cursor_link = false;
            if (cur->created_at && cur->id) {
                auto has_rows_before = has_rows_before_in_link(cursor_link, *cur);
                if (!has_rows_before) {
                    LOG_ERROR() << std::format(
                        "DB select prefix captures failed: {}", has_rows_before.Error().what
                    );
                    return Unex(kDbError);
                }
                include_cursor_link = *has_rows_before;
            }
            const auto limit = links_per_page + (include_cursor_link ? 0_i64 : 1_i64);
            auto previous = select_links_previous(cursor_link, limit);
            if (!previous) {
                LOG_ERROR() << std::format(
                    "DB select prefix links failed: {}", previous.Error().what
                );
                return Unex(kDbError);
            }
            auto previous_links = GrabValueOf(previous);
            const auto visible_previous_links_max = include_cursor_link ? links_per_page - 1_i64
                                                                        : links_per_page;
            if (ssize(previous_links) > visible_previous_links_max) {
                has_more_previous_links = true;
                while (ssize(previous_links) > visible_previous_links_max)
                    previous_links.pop_back();
            }
            std::ranges::reverse(previous_links);
            links.insert(std::end(links), std::begin(previous_links), std::end(previous_links));
            if (include_cursor_link)
                links.push_back(cursor_link);
        } else {
            if (cur->created_at) {
                links.push_back(cursor_link);
                auto more = select_links_next(cursor_link, links_per_page);
                if (!more) {
                    LOG_ERROR() << std::format(
                        "DB select prefix links failed: {}", more.Error().what
                    );
                    return Unex(kDbError);
                }
                auto next_links = GrabValueOf(more);
                links.insert(std::end(links), std::begin(next_links), std::end(next_links));
            } else {
                auto more = select_links_next(cursor_link, links_per_page + 1_i64);
                if (!more) {
                    LOG_ERROR() << std::format(
                        "DB select prefix links failed: {}", more.Error().what
                    );
                    return Unex(kDbError);
                }
                links.insert(std::end(links), std::begin(*more), std::end(*more));
            }
            if (ssize(links) > links_per_page) {
                has_more_next_links = true;
                links.pop_back();
            }
        }
    } else {
        auto first = select_links_first(links_per_page + 1_i64);
        if (!first) {
            LOG_ERROR() << std::format("DB select prefix links failed: {}", first.Error().what);
            return Unex(kDbError);
        }
        links.insert(std::end(links), std::begin(*first), std::end(*first));
        if (ssize(links) > links_per_page) {
            has_more_next_links = true;
            links.pop_back();
        }
    }

    std::vector<dto::UuidWithTimeLink> items;
    items.reserve(NumericCast<size_t>(ssize(links) * impl_->per_link_max));
    bool has_previous_within_link = false;
    bool has_next_within_link = false;

    auto select_rows_for_link = [&](const String &link, i64 idx) {
        auto link_limit = impl_->per_link_max + 1_i64;
        if (cur && cur->created_at && cur->id && cur->direction == crud::PageDirection::kPrevious &&
            link == cur->link) {
            return impl_->capture_meta_repo.GetCapturesByLinkPrev(
                link, link_limit, pg::TimePointTz{*cur->created_at}, *cur->id
            );
        }
        if (idx == 0_i64 && cur && cur->created_at && cur->id &&
            cur->direction == crud::PageDirection::kNext) {
            return impl_->capture_meta_repo.GetCapturesByLinkNext(
                link, link_limit, pg::TimePointTz{*cur->created_at}, *cur->id
            );
        }
        return impl_->capture_meta_repo.GetCapturesByLinkFirst(link, link_limit);
    };

    const auto link_count = ssize(links);
    for (i64 idx = 0; idx < link_count; idx++) {
        const auto &link = links[NumericCast<size_t>(idx)];
        auto rows = select_rows_for_link(link, idx);
        if (!rows) {
            LOG_ERROR() << std::format("DB select prefix captures failed: {}", rows.Error().what);
            return Unex(kDbError);
        }
        auto db_rows = GrabValueOf(rows);
        auto is_previous_cursor_link = cur && cur->direction == crud::PageDirection::kPrevious &&
                                       cur->created_at && link == cur->link;
        auto is_last_visible_link = idx + 1_i64 == link_count;
        if (ssize(db_rows) > impl_->per_link_max) {
            db_rows.pop_back();
            if (is_previous_cursor_link)
                has_previous_within_link = true;
            else if (is_last_visible_link)
                has_next_within_link = true;
        }
        if (is_previous_cursor_link && !db_rows.empty())
            has_next_within_link = true;
        if (is_previous_cursor_link)
            std::ranges::reverse(db_rows);
        for (auto &&r : db_rows) {
            items.emplace_back(
                r.uuid, datetime::TimePointTz{r.created_at.GetUnderlying()}, link.ToBytes()
            );
        }
    }

    std::optional<std::string> next, previous;
    if (!items.empty()) {
        if ((cur && cur->direction == crud::PageDirection::kNext) || has_more_previous_links ||
            has_previous_within_link) {
            const auto &first = items.front();
            previous = crud::EncodePrefixCursor(
                           normalized_prefix, *String::FromBytes(first.link),
                           first.created_at.GetTimePoint(), first.uuid,
                           crud::PageDirection::kPrevious
            )
                           .ToBytes();
        }
        if ((cur && cur->direction == crud::PageDirection::kPrevious) || has_next_within_link ||
            has_more_next_links) {
            const auto &last = items.back();
            auto last_link = *String::FromBytes(last.link);
            if (has_next_within_link) {
                next = crud::EncodePrefixCursor(
                           normalized_prefix, last_link, last.created_at.GetTimePoint(), last.uuid,
                           crud::PageDirection::kNext
                )
                           .ToBytes();
            } else {
                next = crud::EncodePrefixCursor(
                           normalized_prefix, last_link, crud::PageDirection::kNext
                )
                           .ToBytes();
            }
        }
    }
    return dto::PagedFindCapturesByPrefixResponse{
        .items = std::move(items),
        .next_page_token = std::move(next),
        .previous_page_token = std::move(previous),
    };
}

Expected<void, AccessPolicyError> Crud::DenyPrefixAndPurge(String prefix_key) noexcept
{
    auto inserted = impl_->access_policy.InsertPrefix(prefix_key, "deny_and_purge"_t);
    if (!inserted)
        impl_->metrics.AccountError(Metrics::Error::kAccessPolicyCheck);
    TRY(std::move(inserted));

    LOG_INFO() << std::format("enqueued for prefix {}", prefix_key);

    impl_->purge_background.AsyncDetach(
        "purge_prefix_lambda", [impl_ptr = impl_.get(), prefix_key]() {
            try {
                eng::current_task::SetDeadline(
                    eng::Deadline::FromDuration(impl_ptr->purge_job_timeout)
                );
                LOG_INFO() << std::format("Starting purge for denylisted prefix: {}", prefix_key);
                auto purged = impl_ptr->PurgePrefix(prefix_key);
                if (!purged) {
                    LOG_CRITICAL() << std::format("Purge task failed for {}", prefix_key);
                    us::utils::AbortWithStacktrace("Purge task failed");
                }
            } catch (const std::exception &e) {
                LOG_CRITICAL() << std::format("Purge task failed for {}: {}", prefix_key, e.what());
                us::utils::AbortWithStacktrace("Purge task failed");
            }
        }
    );
    return {};
}
