#include "crud.hpp"
/**
 * @file
 * @brief Implementation of storage and crawl orchestration.
 *
 * Implements the `Crud` component, including background crawl startup,
 * metadata writes, and various paged queries.
 */
#include "config.hpp"
#include "crawler/failure.hpp"
#include "crawler/runner.hpp"
#include "denylist.hpp"
#include "grab_value.hpp"
#include "integers.hpp"
#include "link.hpp"
#include "metrics.hpp"
#include "pagination.hpp"
#include "prefix_pagination.hpp"
#include "prefix_utils.hpp"
#include "s3/s3_sts_client.hpp"
#include "s3/s3_v4_client.hpp"
#include "s3_refresh_utils.hpp"
#include "s3_secdist.hpp"
#include "schema/webshot.hpp"
#include "server_errors.hpp"
#include "text.hpp"
#include "text_postgres_formatter.hpp"
#include "uuid_format.hpp"

#include <webshot/sql_queries.hpp>

#include <chrono>
#include <cstdlib>
#include <exception>
#include <format>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <boost/uuid/uuid.hpp>

#include <userver/clients/dns/component.hpp>
#include <userver/clients/http/component.hpp>
#include <userver/components/component.hpp>
#include <userver/components/component_base.hpp>
#include <userver/components/process_starter.hpp>
#include <userver/concurrent/background_task_storage.hpp>
#include <userver/crypto/base64.hpp>
#include <userver/engine/semaphore.hpp>
#include <userver/engine/sleep.hpp>
#include <userver/engine/task/cancel.hpp>
#include <userver/engine/task/current_task.hpp>
#include <userver/engine/task/task_processor_fwd.hpp>
#include <userver/formats/json.hpp>
#include <userver/logging/log.hpp>
#include <userver/rcu/rcu.hpp>
#include <userver/storages/postgres/cluster.hpp>
#include <userver/storages/postgres/io/bytea.hpp>
#include <userver/storages/postgres/io/chrono.hpp>
#include <userver/storages/postgres/io/row_types.hpp>
#include <userver/storages/postgres/io/uuid.hpp>
#include <userver/storages/postgres/postgres.hpp>
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
#include <userver/utils/traceful_exception.hpp>
#include <userver/yaml_config/merge_schemas.hpp>
#include <userver/yaml_config/yaml_config.hpp>

namespace pg = us::storages::postgres;
namespace engine = us::engine;
namespace concurrent = us::concurrent;
namespace rcu = us::rcu;
namespace chrono = std::chrono;
namespace sql = webshot::sql;
using namespace v1;
using namespace text::literals;
using namespace std::chrono_literals;
using Uuid = boost::uuids::uuid;
using chrono::system_clock;

Url v1::buildCaptureDownloadUrl(Uuid uuid, const Config &config)
{
    const auto downloadUrlText =
        String::fromBytes(std::format("{}/{}.wacz", config.publicBaseUrl(), uuid)).expect();
    const auto downloadUrl = Url::fromText(downloadUrlText);
    UINVARIANT(downloadUrl, "downloadUrl must parse");
    return *downloadUrl;
}

namespace {
constexpr i64 kCrawlerSeedAttemptsMax = 2_i64;
constexpr i64 kGiB = 1024_i64 * 1024_i64 * 1024_i64;
constexpr i64 kCpuMaxPeriodUs = 100000_i64;

struct [[nodiscard]] PgError final {
    std::string what;
};

struct [[nodiscard]] CaptureJobRow final {
    Uuid uuid;
    String link;
    std::string status;
    std::optional<std::string> errorCategory;
    std::optional<std::string> errorMessage;
    pg::TimePointTz createdAt;
    std::optional<pg::TimePointTz> startedAt;
    std::optional<pg::TimePointTz> finishedAt;
    std::optional<pg::TimePointTz> resultCreatedAt;
    std::optional<Uuid> resultCaptureId;
};

struct [[nodiscard]] CreateCaptureJobResult final {
    dto::CaptureJob job;
    bool created;
};

struct [[nodiscard]] ClientIpCooldownRow final {
    pg::TimePointTz expiresAt;
};

[[nodiscard]] std::optional<crawler::CgroupLimits> computeCrawlerLimits(i64 cpuCores, i64 memoryGib)
{
    if (cpuCores == 0_i64 && memoryGib == 0_i64)
        return {};

    UINVARIANT(cpuCores > 0_i64 && memoryGib > 0_i64, "crawler limits must be both > 0 or both 0");
    const auto maxI64 = std::numeric_limits<i64>::max();
    const auto maxMemoryGib = maxI64 / kGiB;
    UINVARIANT(memoryGib <= maxMemoryGib, "memory GiB limit is too large");
    const auto maxCpuCores = maxI64 / kCpuMaxPeriodUs;
    UINVARIANT(cpuCores <= maxCpuCores, "cpu core limit is too large");
    return crawler::CgroupLimits{.cpuCores = cpuCores, .memoryBytes = memoryGib * kGiB};
}

[[nodiscard]] dto::CaptureJob makePendingCaptureJob(
    Uuid uuid, const String &link, const us::utils::datetime::TimePointTz &createdAt
)
{
    return dto::CaptureJob{
        .uuid = uuid,
        .link = std::string(link.view()),
        .status = dto::CaptureJob::Status::kPending,
        .created_at = createdAt,
    };
}

[[nodiscard]] dto::CaptureJob makeCaptureJob(CaptureJobRow row)
{
    dto::CaptureJob job;
    job.uuid = row.uuid;
    job.link = std::string(row.link.view());
    if (row.status == "pending")
        job.status = dto::CaptureJob::Status::kPending;
    else if (row.status == "running")
        job.status = dto::CaptureJob::Status::kRunning;
    else if (row.status == "succeeded")
        job.status = dto::CaptureJob::Status::kSucceeded;
    else
        job.status = dto::CaptureJob::Status::kFailed;
    job.created_at = us::utils::datetime::TimePointTz(row.createdAt.GetUnderlying());
    if (row.startedAt)
        job.started_at = us::utils::datetime::TimePointTz(row.startedAt->GetUnderlying());
    if (row.finishedAt)
        job.finished_at = us::utils::datetime::TimePointTz(row.finishedAt->GetUnderlying());
    if (row.resultCreatedAt) {
        job.result_created_at = us::utils::datetime::TimePointTz(
            row.resultCreatedAt->GetUnderlying()
        );
    }
    if (job.status == dto::CaptureJob::Status::kFailed) {
        // Never expose internal diagnostics (crawler details, exception text, etc) to API clients.
        std::string message = "internal server error";
        if (row.errorCategory) {
            if (*row.errorCategory == "size_limit") {
                message = "capture exceeded archive size limit";
            } else if (*row.errorCategory == "crawler_failed") {
                message = "capture failed";
            } else if (*row.errorCategory == "internal_server_error") {
                message = "internal server error";
            }
        }
        dto::ErrorEnvelope::Error err{std::move(message)};
        job.error = dto::ErrorEnvelope{err};
    }
    if (job.status == dto::CaptureJob::Status::kSucceeded && job.result_created_at) {
        UINVARIANT(row.resultCaptureId, "succeeded job must have result_capture_id");
        job.result = dto::UuidWithTimeLink(*row.resultCaptureId, *job.result_created_at, job.link);
    }
    return job;
}

[[nodiscard]] s3v4::S3Credentials makeStaticS3Credentials(
    const s3v4::AccessKeyId &accessKeyId, const s3v4::SecretAccessKey &secretAccessKey
)
{
    return {accessKeyId, secretAccessKey, {}};
}

} // namespace

us::yaml_config::Schema Crud::GetStaticConfigSchema()
{
    return us::yaml_config::MergeSchemas<us::components::ComponentBase>(R"(
type: object
description: '.'
additionalProperties: false
properties:
    snapshots_page_max:
        type: integer
        minimum: 1
        description: '.'
    snapshots_per_link_max:
        type: integer
        minimum: 1
        description: 'Max captures per link in a prefix page'
    snapshots_links_per_page_max:
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
        description: 'STS url used to obtain temporary S3 credentials; S3 data url s3_endpoint (in config) must be http(s)://host[:port] with optional trailing slash and no additional path or query'
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
    link_cooldown_sec:
        type: integer
        minimum: 0
        description: 'Per-link minimum interval between capture jobs in seconds; 0 disables cooldown'
    ip_cooldown_ms:
        type: integer
        minimum: 0
        description: 'Per-client-IP minimum interval between HTTP CRUD operations in milliseconds; 0 disables cooldown'
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
    const i64 pageMax;
    const i64 perLinkMax;
    const i64 linksPerPageMax;
    const chrono::seconds crawlerRunTimeout;
    const i64 crawlerCpuCores;
    const i64 crawlerMemoryGib;
    const chrono::seconds crawlerJobOverheadTimeout;
    const chrono::seconds crawlerPostLoadDelay;
    const chrono::seconds crawlerNetIdleWait;
    const chrono::seconds crawlerPageExtraDelay;
    const chrono::seconds crawlerBehaviorTimeout;
    const chrono::seconds crawlerDevtoolsStartupTimeout;
    const chrono::seconds crawlerCdpHandshakeTimeout;
    const chrono::seconds crawlerCdpCommandTimeout;
    const i64 crawlerSizeLimitMiB;
    const i64 crawlerNetworkDownBytesRatioMax;
    const bool crawlerLocalFixtureRewrite;
    const chrono::milliseconds crawlerDevtoolsPollInterval;
    const chrono::milliseconds crawlerBrowserStopTimeout;
    const chrono::milliseconds crawlerProxyStopTimeout;
    const chrono::seconds linkCooldown;
    const chrono::milliseconds ipCooldown;
    const chrono::seconds crawlJobRetention;
    const chrono::seconds crawlJobCleanupInterval;
    const bool s3UseSts;
    const String s3CredentialsEndpoint;
    const chrono::seconds s3CredentialsDuration;
    const chrono::seconds s3CredentialsRefreshMargin;
    const chrono::seconds s3CredentialsRefreshRetry;
    const chrono::seconds purgeJobTimeout;
    const i64 purgeDeleteBatchSize;
    const Config &svcCfg;
    Metrics &metrics;
    pg::ClusterPtr cluster;
    pg::ClusterPtr sharedCluster;
    us::clients::http::Client &httpClient;
    us::clients::dns::Resolver &dnsResolver;
    us::engine::subprocess::ProcessStarter &processStarter;
    Denylist &denylist;
    CrawlerRunner crawlerRunner;
    struct [[nodiscard]] S3ClientState {
        s3v4::S3Credentials creds;
        system_clock::time_point expiresAt;
        std::shared_ptr<s3v4::S3V4Client> client;
    };
    rcu::Variable<S3ClientState> s3State;
    s3v4::AccessKeyId staticAccessKeyId;
    s3v4::SecretAccessKey staticSecretAccessKey;
    engine::TaskProcessor &mainTaskProcessor;
    engine::CancellableSemaphore crawlSlots;
    engine::TaskProcessor &purgeTaskProcessor;
    engine::TaskProcessor &credsRefreshTaskProcessor;
    // must die first
    us::utils::PeriodicTask s3RefreshTask;
    us::utils::PeriodicTask crawlJobCleanupTask;
    concurrent::BackgroundTaskStorage purgeBackground;
    concurrent::BackgroundTaskStorage crawlBackground;

    [[nodiscard]] Expected<dto::UuidWithTimeLink, errors::CrawlFailure>
    runCrawlJob(Uuid id, Link link);
    [[nodiscard]] Expected<us::utils::datetime::TimePointTz, PgError>
    insertJob(Uuid id, String link);
    [[nodiscard]] Expected<std::optional<ClientIpCooldown>, PgError>
    acquireClientIpCooldownLocked(const String &clientIp);
    [[nodiscard]] Expected<std::optional<dto::CaptureJob>, PgError>
    findLatestJobForLink(const String &link);
    [[nodiscard]] Expected<CreateCaptureJobResult, PgError>
    getOrCreateCaptureJobLocked(const String &normalizedLink);
    [[nodiscard]] Expected<void, PgError> markJobRunning(Uuid id);
    [[nodiscard]] Expected<chrono::milliseconds, PgError> markJobSucceeded(
        Uuid id, Uuid resultCaptureId, const us::utils::datetime::TimePointTz &createdAt
    );
    [[nodiscard]] Expected<chrono::milliseconds, PgError>
    markJobFailed(Uuid id, const String &errorCategory, const String &errorMessage);
    [[nodiscard]] Expected<std::optional<dto::CaptureJob>, PgError> loadJob(Uuid id);
    [[nodiscard]] Expected<void, errors::CrawlFailure> runCrawlerForContext(CrawlContext &ctx);
    [[nodiscard]] CrawlerRunArtifacts runCrawlerAttempt(const String &seedUrl);
    [[nodiscard]] std::optional<StoredCapture> persistMetadataForContext(CrawlContext &ctx);
    [[nodiscard]] Expected<void, errors::CrudError> purgePrefix(const String &prefixKey);
    [[nodiscard]] S3ClientState fetchS3ClientStateFromSts();
    void startS3RefreshTask();
    void refreshS3CredentialsTask();
    void startCrawlJobCleanupTask();
    void cleanupOldJobs();
    explicit Impl(
        const us::components::ComponentConfig &cfg, const us::components::ComponentContext &ctx
    )
        : pageMax(cfg["snapshots_page_max"].As<int64_t>()),
          perLinkMax(cfg["snapshots_per_link_max"].As<int64_t>()),
          linksPerPageMax(cfg["snapshots_links_per_page_max"].As<int64_t>()),
          crawlerRunTimeout(cfg["crawler_run_timeout_sec"].As<int64_t>() * 1s),
          crawlerCpuCores(cfg["crawler_cpu_cores"].As<int64_t>()),
          crawlerMemoryGib(cfg["crawler_memory_gib"].As<int64_t>()),
          crawlerJobOverheadTimeout(cfg["crawler_job_overhead_timeout_sec"].As<int64_t>() * 1s),
          crawlerPostLoadDelay(cfg["crawler_post_load_delay_sec"].As<int64_t>() * 1s),
          crawlerNetIdleWait(cfg["crawler_net_idle_wait_sec"].As<int64_t>() * 1s),
          crawlerPageExtraDelay(cfg["crawler_page_extra_delay_sec"].As<int64_t>() * 1s),
          crawlerBehaviorTimeout(cfg["crawler_behavior_timeout_sec"].As<int64_t>() * 1s),
          crawlerDevtoolsStartupTimeout(
              cfg["crawler_devtools_startup_timeout_sec"].As<int64_t>() * 1s
          ),
          crawlerCdpHandshakeTimeout(cfg["crawler_cdp_handshake_timeout_sec"].As<int64_t>() * 1s),
          crawlerCdpCommandTimeout(cfg["crawler_cdp_command_timeout_sec"].As<int64_t>() * 1s),
          crawlerSizeLimitMiB(cfg["crawler_size_limit_mib"].As<int64_t>()),
          crawlerNetworkDownBytesRatioMax(
              cfg["crawler_network_down_bytes_ratio_max"].As<int64_t>()
          ),
          crawlerLocalFixtureRewrite(cfg["crawler_local_fixture_rewrite"].As<bool>()),
          crawlerDevtoolsPollInterval(cfg["crawler_devtools_poll_interval_ms"].As<int64_t>() * 1ms),
          crawlerBrowserStopTimeout(cfg["crawler_browser_stop_timeout_ms"].As<int64_t>() * 1ms),
          crawlerProxyStopTimeout(cfg["crawler_proxy_stop_timeout_ms"].As<int64_t>() * 1ms),
          linkCooldown(cfg["link_cooldown_sec"].As<int64_t>() * 1s),
          ipCooldown(cfg["ip_cooldown_ms"].As<int64_t>() * 1ms),
          crawlJobRetention(cfg["crawl_job_retention_sec"].As<int64_t>() * 1s),
          crawlJobCleanupInterval(cfg["crawl_job_cleanup_interval_sec"].As<int64_t>() * 1s),
          s3UseSts(cfg["s3_use_sts"].As<bool>()),
          s3CredentialsEndpoint(
              String::fromBytes(cfg["s3_credentials_endpoint"].As<std::string>()).expect()
          ),
          s3CredentialsDuration(cfg["s3_credentials_duration_sec"].As<int64_t>() * 1s),
          s3CredentialsRefreshMargin(cfg["s3_credentials_refresh_margin_sec"].As<int64_t>() * 1s),
          s3CredentialsRefreshRetry(cfg["s3_credentials_refresh_retry_sec"].As<int64_t>() * 1s),
          purgeJobTimeout(cfg["purge_job_timeout_sec"].As<int64_t>() * 1s),
          purgeDeleteBatchSize(cfg["purge_delete_batch_size"].As<int64_t>()),
          svcCfg(ctx.FindComponent<Config>()), metrics(ctx.FindComponent<Metrics>()),
          cluster(ctx.FindComponent<us::components::Postgres>("capture_meta_db").GetCluster()),
          sharedCluster(
              ctx.FindComponent<us::components::Postgres>("shared_state_db").GetCluster()
          ),
          httpClient(ctx.FindComponent<us::components::HttpClient>().GetHttpClient()),
          dnsResolver(ctx.FindComponent<us::clients::dns::Component>().GetResolver()),
          processStarter(ctx.FindComponent<us::components::ProcessStarter>().Get()),
          denylist(ctx.FindComponent<Denylist>()),
          crawlerRunner(
              denylist, svcCfg, dnsResolver, processStarter, crawlerRunTimeout,
              std::string(svcCfg.stateDir()),
              computeCrawlerLimits(crawlerCpuCores, crawlerMemoryGib),
              crawlerSizeLimitMiB * 1024_i64 * 1024_i64,
              crawler::CaptureTimings{
                  crawlerPostLoadDelay,
                  crawlerNetIdleWait,
                  crawlerPageExtraDelay,
                  crawlerBehaviorTimeout,
              },
              crawler::CrawlerTunables{
                  crawlerDevtoolsStartupTimeout,
                  crawlerCdpHandshakeTimeout,
                  crawlerCdpCommandTimeout,
                  crawlerDevtoolsPollInterval,
                  crawlerBrowserStopTimeout,
                  crawlerProxyStopTimeout,
                  crawlerLocalFixtureRewrite,
              },
              crawlerNetworkDownBytesRatioMax
          ),
          mainTaskProcessor(ctx.GetTaskProcessor("main-task-processor")),
          crawlSlots(engine::GetWorkerCount(mainTaskProcessor)),
          purgeTaskProcessor(ctx.GetTaskProcessor("purge_task_processor")),
          credsRefreshTaskProcessor(ctx.GetTaskProcessor("creds_refresh_task_processor")),
          s3RefreshTask(), crawlJobCleanupTask(), purgeBackground(purgeTaskProcessor),
          crawlBackground(mainTaskProcessor)
    {
        const auto fixedTimingBudget = crawlerPostLoadDelay + crawlerNetIdleWait +
                                       crawlerPageExtraDelay + crawlerBehaviorTimeout;
        UINVARIANT(
            fixedTimingBudget <= crawlerRunTimeout,
            "crawler fixed timing budget must be <= crawler_run_timeout_sec"
        );
        UINVARIANT(
            crawlJobRetention >= linkCooldown,
            "crawl_job_retention_sec must be >= link_cooldown_sec"
        );
        UINVARIANT(
            s3CredentialsDuration > s3CredentialsRefreshMargin,
            "s3_credentials_duration_sec must be greater than s3_credentials_refresh_margin_sec"
        );
        const auto &secdist = ctx.FindComponent<us::components::Secdist>().Get();
        const auto &creds = secdist.Get<S3CredentialsSecdist>();
        UINVARIANT(
            creds.accessKeyId && creds.secretAccessKey, "missing required S3 secdist credentials"
        );
        staticAccessKeyId = *creds.accessKeyId;
        staticSecretAccessKey = *creds.secretAccessKey;
        S3ClientState initialState;
        if (s3UseSts) {
            initialState = fetchS3ClientStateFromSts();
            startS3RefreshTask();
        } else {
            S3ClientState state;
            state.creds = makeStaticS3Credentials(staticAccessKeyId, staticSecretAccessKey);
            state.expiresAt = system_clock::time_point::max();
            state.client = std::make_shared<s3v4::S3V4Client>(
                httpClient,
                s3v4::S3V4Config(svcCfg.s3Endpoint(), svcCfg.s3Region(), svcCfg.s3Timeout(), false),
                state.creds, String()
            );
            initialState = std::move(state);
        }
        s3State.Assign(initialState);
        startCrawlJobCleanupTask();
    }

    template <pg::ClusterHostType Host, Metrics::Error ErrorMetric, typename F, typename... Ts>
    [[nodiscard]] auto execDb(pg::ClusterPtr &clusterIn, F &&f, Ts &&...args)
    {
        using Res = decltype(clusterIn->Execute(Host, std::forward<Ts>(args)...));
        using R = std::remove_cvref_t<std::invoke_result_t<F, Res &>>;
        using Out =
            std::conditional_t<std::is_void_v<R>, Expected<void, PgError>, Expected<R, PgError>>;

        try {
            auto res = clusterIn->Execute(Host, std::forward<Ts>(args)...);
            if constexpr (std::is_void_v<R>) {
                std::forward<F>(f)(res);
                return Out{};
            } else {
                return Out{std::forward<F>(f)(res)};
            }
        } catch (const pg::Error &e) {
            metrics.accountError(ErrorMetric);
            return Out{std::unexpected(PgError{.what = std::string(e.what())})};
        }
    }

    template <typename F, typename... Ts> [[nodiscard]] auto readonly(F &&f, Ts &&...args)
    {
        return execDb<pg::ClusterHostType::kSlaveOrMaster, Metrics::Error::kDbCaptureMetaRead>(
            cluster, std::forward<F>(f), std::forward<Ts>(args)...
        );
    }

    template <typename F, typename... Ts> [[nodiscard]] auto readwrite(F &&f, Ts &&...args)
    {
        return execDb<pg::ClusterHostType::kMaster, Metrics::Error::kDbCaptureMetaWrite>(
            cluster, std::forward<F>(f), std::forward<Ts>(args)...
        );
    }

    template <typename F, typename... Ts> [[nodiscard]] auto sharedReadonly(F &&f, Ts &&...args)
    {
        return execDb<pg::ClusterHostType::kSlaveOrMaster, Metrics::Error::kDbSharedStateRead>(
            sharedCluster, std::forward<F>(f), std::forward<Ts>(args)...
        );
    }

    template <typename F, typename... Ts> [[nodiscard]] auto sharedReadwrite(F &&f, Ts &&...args)
    {
        return execDb<pg::ClusterHostType::kMaster, Metrics::Error::kDbSharedStateWrite>(
            sharedCluster, std::forward<F>(f), std::forward<Ts>(args)...
        );
    }
};

Crud::Crud(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : us::components::ComponentBase(config, context),
      impl(std::make_unique<Crud::Impl>(config, context))
{
}

Crud::~Crud() = default;

/** Lightweight context shared across steps of a single crawl job. */
struct [[nodiscard]] CrawlContext {
    Link link;
    Uuid id;
    String keyOnly;
    String s3Key;
    std::optional<std::string> waczBytes;
    std::optional<std::string> contentSha256;
    std::optional<Url> replayUrl;
    std::optional<String> failureMessage;

    CrawlContext(Uuid id, Link link, const Config &cfg)
        : link(std::move(link)), id(id), keyOnly(text::format("{}.wacz", id)),
          s3Key(text::format("{}/{}", cfg.s3Bucket(), keyOnly))
    {
    }
};

struct [[nodiscard]] StoredCapture {
    Uuid id;
    us::utils::datetime::TimePointTz createdAt;
};

[[nodiscard]] Expected<dto::UuidWithTimeLink, errors::CrawlFailure>
Crud::Impl::runCrawlJob(Uuid id, Link link)
{
    using enum errors::CrawlError;

    const auto totalCrawlTimeLimit = crawlerJobOverheadTimeout +
                                     crawlerRunTimeout * kCrawlerSeedAttemptsMax;
    engine::current_task::SetDeadline(engine::Deadline::FromDuration(totalCrawlTimeLimit));

    std::shared_lock<engine::CancellableSemaphore> slotLock(crawlSlots);

    CrawlContext ctx(id, std::move(link), svcCfg);

    LOG_INFO() << std::format(
        "runCrawlJob starting crawler for job {} ({})", id, ctx.link.normalized()
    );
    {
        const auto ran = runCrawlerForContext(ctx);
        if (!ran) {
            metrics.accountError(Metrics::Error::kCrawlerRun);
            return std::unexpected(ran.error());
        }
    }
    LOG_INFO() << std::format(
        "runCrawlJob finished crawler for job {} ({})", id, ctx.link.normalized()
    );

    LOG_INFO() << std::format("Persisting metadata for job {} ({})", id, ctx.link.normalized());
    auto stored = persistMetadataForContext(ctx);
    if (!stored)
        return std::unexpected(errors::CrawlFailure{.code = kPersistMetadataFailed, .detail = {}});
    LOG_INFO() << std::format("Persisted metadata for job {} ({})", id, ctx.link.normalized());
    return dto::UuidWithTimeLink{
        stored->id, stored->createdAt, std::string(ctx.link.normalized().view())
    };
}

Expected<us::utils::datetime::TimePointTz, PgError> Crud::Impl::insertJob(Uuid id, String link)
{
    struct Row {
        pg::TimePointTz createdAt;
    };
    auto row = sharedReadwrite(
        [&](auto &res) { return res.template AsSingleRow<Row>(pg::kRowTag); }, sql::kInsertCrawlJob,
        id, link
    );
    if (!row)
        return std::unexpected(std::move(row).error());
    return us::utils::datetime::TimePointTz(row->createdAt.GetUnderlying());
}

Expected<std::optional<ClientIpCooldown>, PgError>
Crud::Impl::acquireClientIpCooldownLocked(const String &clientIp)
{
    if (ipCooldown == 0ms)
        return {};

    try {
        auto trx = sharedCluster->Begin(pg::ClusterHostType::kMaster, pg::Transaction::RW);
        trx.Execute(sql::kLockClientIpCooldown, text::format("client_ip_cooldown:{}", clientIp));

        const auto now = us::utils::datetime::Now();
        auto rowOpt = trx.Execute(sql::kSelectClientIpCooldown, clientIp)
                          .template AsOptionalSingleRow<ClientIpCooldownRow>(pg::kRowTag);
        if (rowOpt) {
            const auto expiresAt = rowOpt->expiresAt.GetUnderlying();
            if (now < expiresAt) {
                trx.Commit();
                return ClientIpCooldown{
                    .retryAfter = chrono::ceil<chrono::milliseconds>(expiresAt - now),
                };
            }
        }

        trx.Execute(sql::kUpsertClientIpCooldown, clientIp, pg::TimePointTz(now + ipCooldown));
        trx.Commit();
        return {};
    } catch (const pg::Error &e) {
        metrics.accountError(Metrics::Error::kDbSharedStateWrite);
        us::utils::AbortWithStacktrace(
            std::format("Failed to acquire client IP cooldown for {}: {}", clientIp, e.what())
        );
    }
}

Expected<void, PgError> Crud::Impl::markJobRunning(Uuid id)
{
    return sharedReadwrite([](auto &) {}, sql::kUpdateCrawlJobRunning, id);
}

Expected<chrono::milliseconds, PgError> Crud::Impl::markJobSucceeded(
    Uuid id, Uuid resultCaptureId, const us::utils::datetime::TimePointTz &createdAt
)
{
    struct Row {
        Uuid id;
        int64_t durationMs;
    };
    auto row = sharedReadwrite(
        [&](auto &res) { return res.template AsSingleRow<Row>(pg::kRowTag); },
        sql::kUpdateCrawlJobSucceeded, id, pg::TimePointTz(createdAt.GetTimePoint()),
        resultCaptureId
    );
    if (!row)
        return std::unexpected(std::move(row).error());
    return row->durationMs * 1ms;
}

Expected<chrono::milliseconds, PgError>
Crud::Impl::markJobFailed(Uuid id, const String &errorCategory, const String &errorMessage)
{
    struct Row {
        Uuid id;
        int64_t durationMs;
    };
    auto row = sharedReadwrite(
        [&](auto &res) { return res.template AsSingleRow<Row>(pg::kRowTag); },
        sql::kUpdateCrawlJobFailed, id, errorCategory, errorMessage
    );
    if (!row)
        return std::unexpected(std::move(row).error());
    return row->durationMs * 1ms;
}

Expected<std::optional<dto::CaptureJob>, PgError> Crud::Impl::loadJob(Uuid id)
{
    auto rowOpt = sharedReadonly(
        [&](auto &res) { return res.template AsOptionalSingleRow<CaptureJobRow>(pg::kRowTag); },
        sql::kSelectCrawlJob, id
    );
    if (!rowOpt)
        return std::unexpected(std::move(rowOpt).error());
    if (!*rowOpt)
        return {};
    return {makeCaptureJob(grabValueOf(grabValueOf(rowOpt)))};
}

Crud::Impl::S3ClientState Crud::Impl::fetchS3ClientStateFromSts()
{
    const auto sessionUuid = text::format("{}", us::utils::generators::GenerateBoostUuid());
    const auto sessionName = text::format("{}", sessionUuid);
    const auto kRoleArnDescription = "ephemeral_s3_credentials"_t;

    const auto policyJson = text::format(
        "{{\"Version\":\"2012-10-17\",\"Statement\":{{\"Sid\":\"access\",\"Effect\":"
        "\"Allow\",\"Principal\":\"*\",\"Action\":[\"s3:PutObject\",\"s3:DeleteObject\","
        "\"s3:GetObject\"],\"Resource\":\"arn:aws:s3:::{}/*\"}}}}",
        svcCfg.s3Bucket()
    );

    const auto sts = fetchStsCredentials(
        httpClient, s3CredentialsEndpoint, staticAccessKeyId, staticSecretAccessKey,
        svcCfg.s3Region(), kRoleArnDescription, sessionName, policyJson, s3CredentialsDuration,
        svcCfg.s3Timeout()
    );
    if (!sts) {
        metrics.accountError(Metrics::Error::kStsRefresh);
        us::utils::AbortWithStacktrace("failed to fetch STS credentials");
    }

    S3ClientState state;
    state.creds = s3v4::S3Credentials(sts->accessKeyId, sts->secretAccessKey, sts->sessionToken);
    state.expiresAt = sts->expiresAt;
    state.client = std::make_shared<s3v4::S3V4Client>(
        httpClient,
        s3v4::S3V4Config(svcCfg.s3Endpoint(), svcCfg.s3Region(), svcCfg.s3Timeout(), false),
        state.creds, String()
    );
    return state;
}

Expected<std::optional<dto::CaptureJob>, PgError>
Crud::Impl::findLatestJobForLink(const String &link)
{
    auto rowOpt = sharedReadonly(
        [&](auto &res) { return res.template AsOptionalSingleRow<CaptureJobRow>(pg::kRowTag); },
        sql::kSelectLatestCrawlJobByLink, link
    );
    if (!rowOpt)
        return std::unexpected(std::move(rowOpt).error());
    if (!*rowOpt)
        return {};
    return {makeCaptureJob(grabValueOf(grabValueOf(rowOpt)))};
}

Expected<CreateCaptureJobResult, PgError>
Crud::Impl::getOrCreateCaptureJobLocked(const String &normalizedLink)
{
    struct Row {
        pg::TimePointTz createdAt;
    };

    try {
        auto trx = sharedCluster->Begin(pg::ClusterHostType::kMaster, pg::Transaction::RW);
        if (linkCooldown > 0s) {
            trx.Execute(sql::kLockCrawlJobLink, text::format("link:{}", normalizedLink));
        }

        if (linkCooldown > 0s) {
            auto latestJobRowOpt = trx.Execute(sql::kSelectLatestCrawlJobByLink, normalizedLink)
                                       .template AsOptionalSingleRow<CaptureJobRow>(pg::kRowTag);
            if (latestJobRowOpt) {
                auto job = makeCaptureJob(grabValueOf(std::move(latestJobRowOpt)));
                const auto now = us::utils::datetime::Now();
                const auto lastCreated = job.created_at.GetTimePoint();
                const auto deadline = lastCreated + linkCooldown;
                if (now < deadline) {
                    trx.Commit();
                    return CreateCaptureJobResult{.job = std::move(job), .created = false};
                }
            }
        }

        auto id = us::utils::generators::GenerateBoostUuid();
        auto row = trx.Execute(sql::kInsertCrawlJob, id, normalizedLink)
                       .template AsSingleRow<Row>(pg::kRowTag);
        trx.Commit();

        metrics.accountCaptureJobCreated();
        return CreateCaptureJobResult{
            .job = makePendingCaptureJob(
                id, normalizedLink, us::utils::datetime::TimePointTz(row.createdAt.GetUnderlying())
            ),
            .created = true,
        };
    } catch (const pg::Error &e) {
        metrics.accountError(Metrics::Error::kDbSharedStateWrite);
        return std::unexpected(PgError{.what = std::string(e.what())});
    }
}

void Crud::Impl::startS3RefreshTask()
{
    auto snapshot = s3State.Read();
    const auto now = us::utils::datetime::Now();
    auto delay = s3refresh::computeRefreshDelay(
        now, snapshot->expiresAt, s3CredentialsRefreshMargin
    );

    us::utils::PeriodicTask::Settings settings(
        chrono::duration_cast<chrono::milliseconds>(delay), 0ms
    );
    settings.task_processor = &credsRefreshTaskProcessor;

    s3RefreshTask.Start("s3_credentials_refresh", settings, [this]() {
        refreshS3CredentialsTask();
    });
}

void Crud::Impl::refreshS3CredentialsTask()
{
    for (;;) {
        if (engine::current_task::ShouldCancel())
            return;
        try {
            const auto newState = fetchS3ClientStateFromSts();
            s3State.Assign(newState);

            const auto now = us::utils::datetime::Now();
            auto nextDelay = s3refresh::computeRefreshDelay(
                now, newState.expiresAt, s3CredentialsRefreshMargin
            );

            us::utils::PeriodicTask::Settings settings(
                chrono::duration_cast<chrono::milliseconds>(nextDelay), 0ms
            );
            settings.task_processor = &credsRefreshTaskProcessor;
            s3RefreshTask.SetSettings(settings);
            break;
        } catch (const us::utils::TracefulException &e) {
            metrics.accountError(Metrics::Error::kStsRefresh);
            LOG_ERROR() << std::format("Failed to refresh S3 credentials from STS: {}", e.what());
            engine::SleepFor(s3CredentialsRefreshRetry);
        }
    }
}

void Crud::Impl::startCrawlJobCleanupTask()
{
    const auto interval = crawlJobCleanupInterval;
    us::utils::PeriodicTask::Settings settings(interval, 0ms);
    settings.task_processor = &purgeTaskProcessor;

    crawlJobCleanupTask.Start("crawl_job_cleanup", settings, [this]() { cleanupOldJobs(); });
}

void Crud::Impl::cleanupOldJobs()
{
    const auto now = us::utils::datetime::Now();
    const auto cutoff = now - crawlJobRetention;
    const auto deleted = sharedReadwrite(
        [](auto &) {}, sql::kDeleteCrawlJobsExpired, pg::TimePointTz(cutoff)
    );
    if (!deleted) {
        LOG_ERROR() << std::format("Failed to delete old crawl jobs: {}", deleted.error().what);
    }

    const auto cooldownDeleted = sharedReadwrite(
        [](auto &) {}, sql::kDeleteClientIpCooldownsExpired, pg::TimePointTz(now)
    );
    if (!cooldownDeleted) {
        us::utils::AbortWithStacktrace(
            std::format(
                "Failed to delete expired client IP cooldowns: {}", cooldownDeleted.error().what
            )
        );
    }
}

CrawlerRunArtifacts Crud::Impl::runCrawlerAttempt(const String &seedUrl)
{
    LOG_INFO() << std::format(
        "Submitting crawl for {} to embedded crawler with timeout={}s", seedUrl,
        crawlerRunTimeout.count()
    );

    const auto run = crawlerRunner.run(seedUrl);
    LOG_INFO() << std::format(
        "Embedded crawler returned for {} (exit_code={}, wacz_exists={})", seedUrl,
        run.attempt.exitCode, run.attempt.waczExists ? "true" : "false"
    );
    if (!run.attempt.waczExists) {
        const auto attemptContext = crawler::formatAttemptContext(run.attempt);
        LOG_INFO() << std::format(
            "Crawler attempt failed for {}{}", seedUrl,
            attemptContext.empty() ? std::string() : std::format(" ({})", attemptContext)
        );
        return run;
    }
    return run;
}

Expected<void, errors::CrawlFailure> Crud::Impl::runCrawlerForContext(CrawlContext &ctx)
{
    using enum errors::CrawlError;
    using userver::utils::UnderlyingValue;

    const auto httpsSeedUrl = ctx.link.httpsUrl();
    const auto httpSeedUrl = ctx.link.httpUrl();

    const auto tryStoreSuccess = [&ctx](const CrawlerRunArtifacts &run) -> bool {
        if (!crawler::isAttemptSuccess(run.attempt))
            return false;
        UINVARIANT(run.wacz, "crawler reported wacz_exists=true but did not provide WACZ bytes");
        UINVARIANT(
            run.contentSha256, "crawler did not provide content hash for a successful capture"
        );
        UINVARIANT(ssize(*run.contentSha256) == 32_i64, "content hash must be 32 bytes");
        UINVARIANT(run.replayUrl, "crawler did not provide replayUrl for a successful capture");
        const auto replayUrl = Url::fromText(*run.replayUrl);
        UINVARIANT(replayUrl, "replayUrl must parse for a successful capture");
        ctx.waczBytes = *run.wacz;
        ctx.contentSha256 = *run.contentSha256;
        ctx.replayUrl = *replayUrl;
        return true;
    };

    const auto httpsRun = runCrawlerAttempt(httpsSeedUrl);
    if (tryStoreSuccess(httpsRun))
        return {};
    if (!httpsRun.attempt.exited) {
        const auto attemptContext = crawler::formatAttemptContext(httpsRun.attempt);
        ctx.failureMessage =
            String::fromBytes(
                std::format(
                    "Failed to crawl {}, child process did not exit cleanly{}", httpsSeedUrl,
                    attemptContext.empty() ? std::string() : std::format(" ({})", attemptContext)
                )
            )
                .expect();
        LOG_INFO() << ctx.failureMessage->view();
        return std::unexpected(errors::CrawlFailure{.code = kFailed, .detail = ctx.failureMessage});
    }
    if (httpsRun.attempt.exitCode == UnderlyingValue(crawler::CrawlerExitCode::kSizeLimit)) {
        ctx.failureMessage = String::fromBytes(
                                 std::format(
                                     "Failed to crawl {} ({})", httpsSeedUrl,
                                     crawler::formatAttemptStatus("https", httpsRun.attempt)
                                 )
        )
                                 .expect();
        LOG_INFO() << ctx.failureMessage->view();
        return std::unexpected(errors::CrawlFailure{.code = kSizeLimit, .detail = {}});
    }
    if (httpsRun.attempt.exitCode == UnderlyingValue(crawler::CrawlerExitCode::kSuccess) &&
        !httpsRun.attempt.waczExists) {
        const auto attemptContext = crawler::formatAttemptContext(httpsRun.attempt);
        ctx.failureMessage = String::fromBytes(
                                 std::format(
                                     "Failed to crawl {}, no WACZ{}", httpsSeedUrl,
                                     attemptContext.empty() ? std::string()
                                                            : std::format(" ({})", attemptContext)
                                 )
        )
                                 .expect();
        LOG_INFO() << ctx.failureMessage->view();
        return std::unexpected(errors::CrawlFailure{.code = kFailed, .detail = ctx.failureMessage});
    }
    if (!crawler::shouldAttemptHttpFallback(httpsRun.attempt)) {
        ctx.failureMessage = String::fromBytes(
                                 std::format(
                                     "Failed to crawl {} ({})", ctx.link.normalized(),
                                     crawler::formatAttemptStatus("https", httpsRun.attempt)
                                 )
        )
                                 .expect();
        LOG_INFO() << ctx.failureMessage->view();
        return std::unexpected(errors::CrawlFailure{.code = kFailed, .detail = ctx.failureMessage});
    }

    if (httpsRun.attempt.seedProbe) {
        LOG_INFO() << std::format(
            "HTTPS seed probe before HTTP fallback: status={}, loadState={}",
            httpsRun.attempt.seedProbe->status.value_or(0),
            httpsRun.attempt.seedProbe->loadState.value_or(-1)
        );
    }

    const auto httpRun = runCrawlerAttempt(httpSeedUrl);
    if (tryStoreSuccess(httpRun)) {
        LOG_INFO() << "HTTP fallback succeeded after HTTPS failed with no response";
        return {};
    }
    if (!httpRun.attempt.exited) {
        const auto attemptContext = crawler::formatAttemptContext(httpRun.attempt);
        ctx.failureMessage =
            String::fromBytes(
                std::format(
                    "Failed to crawl {}, child process did not exit cleanly{}", httpSeedUrl,
                    attemptContext.empty() ? std::string() : std::format(" ({})", attemptContext)
                )
            )
                .expect();
        LOG_INFO() << ctx.failureMessage->view();
        return std::unexpected(errors::CrawlFailure{.code = kFailed, .detail = ctx.failureMessage});
    }
    if (httpRun.attempt.exitCode == UnderlyingValue(crawler::CrawlerExitCode::kSizeLimit)) {
        ctx.failureMessage = String::fromBytes(
                                 std::format(
                                     "Failed to crawl {} ({})", httpSeedUrl,
                                     crawler::formatAttemptStatus("http", httpRun.attempt)
                                 )
        )
                                 .expect();
        LOG_INFO() << ctx.failureMessage->view();
        return std::unexpected(errors::CrawlFailure{.code = kSizeLimit, .detail = {}});
    }
    if (httpRun.attempt.exitCode == UnderlyingValue(crawler::CrawlerExitCode::kSuccess) &&
        !httpRun.attempt.waczExists) {
        const auto attemptContext = crawler::formatAttemptContext(httpRun.attempt);
        ctx.failureMessage = String::fromBytes(
                                 std::format(
                                     "Failed to crawl {}, no WACZ{}", httpSeedUrl,
                                     attemptContext.empty() ? std::string()
                                                            : std::format(" ({})", attemptContext)
                                 )
        )
                                 .expect();
        LOG_INFO() << ctx.failureMessage->view();
        return std::unexpected(errors::CrawlFailure{.code = kFailed, .detail = ctx.failureMessage});
    }

    ctx.failureMessage = String::fromBytes(
                             std::format(
                                 "Failed to crawl {} ({}, {})", ctx.link.normalized(),
                                 crawler::formatAttemptStatus("https", httpsRun.attempt),
                                 crawler::formatAttemptStatus("http", httpRun.attempt)
                             )
    )
                             .expect();
    LOG_INFO() << ctx.failureMessage->view();
    return std::unexpected(errors::CrawlFailure{.code = kFailed, .detail = ctx.failureMessage});
}

std::optional<StoredCapture> Crud::Impl::persistMetadataForContext(CrawlContext &ctx)
{
    const auto prefixKey = prefix::makePrefixKey(ctx.link);
    const auto prefixTree = prefix::makePrefixTree(prefixKey);
    const auto host = ctx.link.url.hostname();
    UINVARIANT(ctx.replayUrl, "replayUrl must be set for a successful capture");

    const auto allowed = denylist.isAllowedPrefix(prefixKey);
    if (!allowed || !*allowed) {
        if (!allowed) {
            metrics.accountError(Metrics::Error::kDenylistCheck);
            LOG_ERROR() << std::format("Failed to check denylist state during crawl: {}", host);
        } else {
            LOG_INFO() << std::format("Host became denylisted during crawl: {}", host);
        }
        return {};
    }

    UINVARIANT(ctx.waczBytes, "persistMetadataForContext called without WACZ bytes");
    UINVARIANT(ctx.contentSha256, "persistMetadataForContext called without content hash");
    UINVARIANT(ssize(*ctx.contentSha256) == 32_i64, "content hash must be 32 bytes");

    struct ExistingRow {
        Uuid id;
        pg::TimePointTz createdAt;
    };
    auto existing = readonly(
        [&](auto &res) { return res.template AsOptionalSingleRow<ExistingRow>(pg::kRowTag); },
        sql::kSelectCaptureByLinkHash, ctx.link.normalized(),
        pg::Bytea(std::string_view{*ctx.contentSha256})
    );
    if (!existing) {
        LOG_ERROR() << std::format(
            "DB select capture-by-hash failed for {}: {}", ctx.link.normalized(),
            existing.error().what
        );
        return {};
    }
    if (*existing) {
        auto row = grabValueOf(grabValueOf(existing));
        return StoredCapture{
            .id = row.id,
            .createdAt = us::utils::datetime::TimePointTz(row.createdAt.GetUnderlying()),
        };
    }

    try {
        auto snapshot = s3State.Read();
        snapshot->client->PutObject(
            ctx.s3Key.view(), *ctx.waczBytes, {}, "application/wacz", {}, {}
        );
    } catch (const us::utils::TracefulException &e) {
        metrics.accountError(Metrics::Error::kS3PutObject);
        LOG_ERROR() << std::format("S3 upload failed for {}: {}", ctx.s3Key, e.what());
        return {};
    }

    struct Row {
        Uuid id;
        pg::TimePointTz createdAt;
    };
    auto row = readwrite(
        [&](auto &res) { return res.template AsSingleRow<Row>(pg::kRowTag); }, sql::kInsertCapture,
        ctx.id, ctx.link.normalized(), prefixKey, prefixTree,
        pg::Bytea(std::string_view{*ctx.contentSha256}), ctx.replayUrl->href()
    );
    if (!row) {
        try {
            auto snapshot = s3State.Read();
            snapshot->client->DeleteObject(ctx.s3Key.view());
        } catch (const us::utils::TracefulException &) {
            metrics.accountError(Metrics::Error::kS3DeleteObject);
            LOG_ERROR() << std::format("error deleting {}", ctx.s3Key);
        }
        LOG_ERROR() << std::format("DB insert failed for {}: {}", ctx.id, row.error().what);
        return {};
    }
    return StoredCapture{
        .id = ctx.id,
        .createdAt = us::utils::datetime::TimePointTz(row->createdAt.GetUnderlying()),
    };
}

Expected<void, errors::CrudError> Crud::Impl::purgePrefix(const String &prefixKey)
{
    using enum errors::CrudError;

    const auto tree = prefix::makePrefixTree(prefixKey);
    while (true) {
        auto ids = readonly(
            [&](auto &res) {
                std::vector<Uuid> idsOut;
                idsOut.reserve(res.Size());
                for (auto row : res)
                    idsOut.emplace_back(row[0].template As<Uuid>());
                return idsOut;
            },
            sql::kSelectIdsByDenyPrefixPaged, tree, raw(purgeDeleteBatchSize)
        );
        if (!ids) {
            LOG_ERROR() << std::format(
                "denylist purge failed for {}: {}", prefixKey, ids.error().what
            );
            return std::unexpected(kDbFailure);
        }
        if (ids->empty())
            break;

        std::vector<Uuid> single;
        single.reserve(1);
        for (auto &&id : *ids) {
            const auto key = std::format("{}/{}", svcCfg.s3Bucket(), id);
            try {
                auto snapshot = s3State.Read();
                snapshot->client->DeleteObject(key);
            } catch (const us::utils::TracefulException &e) {
                metrics.accountError(Metrics::Error::kS3DeleteObject);
                LOG_ERROR() << std::format(
                    "S3 delete failed for key {} (prefix={}): {}", key, prefixKey, e.what()
                );
                return std::unexpected(kDbFailure);
            }

            single.clear();
            single.emplace_back(id);
            auto deleted = readwrite([](auto &) {}, sql::kDeleteCapturesByIds, single);
            if (!deleted) {
                LOG_ERROR() << std::format(
                    "denylist purge failed for {}: {}", prefixKey, deleted.error().what
                );
                return std::unexpected(kDbFailure);
            }
        }
    }
    return {};
}

Expected<dto::UuidWithTimeLink, errors::CrawlFailure> Crud::createCapture(Link link)
{
    auto *implPtr = impl.get();
    auto id = us::utils::generators::GenerateBoostUuid();
    return us::utils::Async(
               "create_capture",
               [implPtr, id, link = std::move(link)]() { return implPtr->runCrawlJob(id, link); }
    ).Get();
}

Expected<dto::CaptureJob, errors::CreateJobError> Crud::createCaptureJob(Link link)
{
    using enum errors::CreateJobError;

    auto *implPtr = impl.get();
    const auto normalizedLink = link.normalized();
    dto::CaptureJob job;
    Uuid id;

    if (implPtr->linkCooldown > 0s) {
        auto decision = implPtr->getOrCreateCaptureJobLocked(normalizedLink);
        if (!decision) {
            LOG_ERROR() << std::format(
                "Failed to create or reuse crawl job for {}: {}", normalizedLink,
                decision.error().what
            );
            return std::unexpected(kDbFailure);
        }
        auto result = grabValueOf(decision);
        job = std::move(result.job);
        if (!result.created)
            return job;
        id = job.uuid;
    } else {
        id = us::utils::generators::GenerateBoostUuid();
        auto createdAt = implPtr->insertJob(id, normalizedLink);
        if (!createdAt) {
            LOG_ERROR() << std::format(
                "Failed to create crawl job for {}: {}", normalizedLink, createdAt.error().what
            );
            return std::unexpected(kDbFailure);
        }
        implPtr->metrics.accountCaptureJobCreated();
        job = makePendingCaptureJob(id, normalizedLink, grabValueOf(createdAt));
    }
    implPtr->crawlBackground.AsyncDetach("crawl_job", [implPtr, id, link = std::move(link)]() {
        const auto markFailed = [&](const String &errorCategory, const String &errorMessage) {
            // Persist terminal job state even if the crawl deadline has already cancelled this
            // task.
            const engine::TaskCancellationBlocker blocker;
            return implPtr->markJobFailed(id, errorCategory, errorMessage);
        };
        const auto markSucceeded = [&](Uuid resultCaptureId,
                                       const us::utils::datetime::TimePointTz &createdAtValue) {
            const engine::TaskCancellationBlocker blocker;
            return implPtr->markJobSucceeded(id, resultCaptureId, createdAtValue);
        };
        auto markInternalError = [&](std::string_view what) {
            LOG_ERROR() << std::format("Unexpected crawl job failure for {}: {}", id, what);
            const auto marked = markFailed("internal_server_error"_t, "internal server error"_t);
            if (!marked) {
                LOG_ERROR() << std::format(
                    "DB update crawl job failed for {}: {}", id, marked.error().what
                );
            } else {
                implPtr->metrics.accountCaptureCompleted(false, *marked);
            }
        };

        try {
            const auto running = implPtr->markJobRunning(id);
            if (!running) {
                LOG_ERROR() << std::format(
                    "DB update crawl job failed for {}: {}", id, running.error().what
                );
                return;
            }

            auto result = implPtr->runCrawlJob(id, link);
            if (!result) {
                using enum errors::CrawlError;
                Expected<chrono::milliseconds, PgError> marked;
                if (result.error().code == kSizeLimit) {
                    marked = markFailed("size_limit"_t, "capture exceeded archive size limit"_t);
                } else if (result.error().code == kPersistMetadataFailed) {
                    marked = markFailed("internal_server_error"_t, "internal server error"_t);
                } else if (result.error().detail) {
                    marked = markFailed("crawler_failed"_t, *result.error().detail);
                } else {
                    marked = markFailed("crawler_failed"_t, "crawler failed"_t);
                }
                if (!marked) {
                    LOG_ERROR() << std::format(
                        "DB update crawl job failed for {}: {}", id, marked.error().what
                    );
                } else {
                    implPtr->metrics.accountCaptureCompleted(false, *marked);
                }
                return;
            }

            const auto succeeded = markSucceeded(result->uuid, result->created_at);
            if (!succeeded) {
                LOG_ERROR() << std::format(
                    "DB update crawl job failed for {}: {}", id, succeeded.error().what
                );
            } else {
                implPtr->metrics.accountCaptureCompleted(true, *succeeded);
            }
        } catch (const us::utils::TracefulException &e) {
            markInternalError(e.what());
        } catch (const std::exception &e) {
            markInternalError(e.what());
        }
    });

    return job;
}

Expected<std::optional<ClientIpCooldown>, errors::CrudError>
Crud::acquireClientIpCooldown(String clientIp)
{
    using enum errors::CrudError;

    UINVARIANT(!clientIp.empty(), "client IP must not be empty");
    auto acquired = impl->acquireClientIpCooldownLocked(clientIp);
    if (!acquired) {
        us::utils::AbortWithStacktrace(
            std::format(
                "Failed to acquire client IP cooldown for {}: {}", clientIp, acquired.error().what
            )
        );
    }
    return grabValueOf(acquired);
}

Expected<std::optional<CaptureRecord>, errors::CrudError> Crud::findCapture(Uuid uuid)
{
    using enum errors::CrudError;

    struct Row {
        pg::TimePointTz createdAt;
        std::string link;
        std::string replayUrl;
    };
    auto capture = impl->readonly(
        [&](auto &res) { return res.template AsOptionalSingleRow<Row>(pg::kRowTag); },
        sql::kSelectCapture, uuid
    );
    if (!capture) {
        LOG_ERROR() << std::format(
            "DB select capture failed for {}: {}", uuid, capture.error().what
        );
        return std::unexpected(kDbFailure);
    }
    auto captureOpt = grabValueOf(capture);
    if (!captureOpt) {
        LOG_INFO() << std::format("UUID not found: {}", uuid);
        return {};
    }
    auto row = grabValueOf(captureOpt);
    auto linkText = String::fromBytes(row.link);
    if (!linkText)
        return std::unexpected(kCorruptData);
    auto replayUrlText = String::fromBytes(row.replayUrl);
    if (!replayUrlText)
        return std::unexpected(kCorruptData);
    auto replayUrl = Url::fromText(*replayUrlText);
    if (!replayUrl)
        return std::unexpected(kCorruptData);
    return {CaptureRecord{
        .uuid = uuid,
        .createdAt = us::utils::datetime::TimePointTz(row.createdAt.GetUnderlying()),
        .link = *linkText,
        .replayUrl = *replayUrl,
    }};
}

Expected<std::optional<dto::CaptureJob>, errors::CrudError> Crud::findCaptureJob(Uuid uuid)
{
    using enum errors::CrudError;

    auto job = impl->loadJob(uuid);
    if (!job) {
        LOG_ERROR() << std::format("DB select job failed for {}: {}", uuid, job.error().what);
        return std::unexpected(kDbFailure);
    }
    return *job;
}

Expected<dto::PagedFindCapturesByUrlResponse, errors::CapturePageError>
Crud::findCapturesByLinkPage(const Link &link, String pageToken)
{
    namespace crud = v1::crud;
    using enum errors::CapturePageError;

    struct Row {
        Uuid uuid;
        pg::TimePointTz timepoint;
    };
    if (pageToken.empty()) {
        auto rows = impl->readonly(
            [&](auto &res) { return res.template AsContainer<std::vector<Row>>(pg::kRowTag); },
            sql::kSelectCaptureByLinkFirst, link.normalized(), raw(impl->pageMax)
        );
        if (!rows) {
            LOG_ERROR() << std::format("DB select captures page failed: {}", rows.error().what);
            return std::unexpected(kDbFailure);
        }
        auto dbRows = grabValueOf(rows);
        std::vector<dto::UuidWithTime> items;
        items.reserve(dbRows.size());
        for (const auto &row : dbRows) {
            items.emplace_back(
                row.uuid, us::utils::datetime::TimePointTz(row.timepoint.GetUnderlying())
            );
        }
        if (ssize(items) == impl->pageMax && !items.empty()) {
            const auto &last = items.back();
            auto tp = last.created_at.GetTimePoint();
            crud::Cursor cursor(tp, last.uuid);
            return dto::PagedFindCapturesByUrlResponse{
                .items = std::move(items),
                .next_page_token = std::string(crud::encodeCursor(cursor).view()),
            };
        }
        return dto::PagedFindCapturesByUrlResponse{
            .items = std::move(items), .next_page_token = {}
        };
    } else {
        auto cur = crud::decodeCursor(pageToken);
        if (!cur)
            return std::unexpected(kInvalidPageToken);
        auto rows = impl->readonly(
            [&](auto &res) { return res.template AsContainer<std::vector<Row>>(pg::kRowTag); },
            sql::kSelectCaptureByLinkNext, link.normalized(), raw(impl->pageMax),
            pg::TimePointTz(cur->createdAt), cur->id
        );
        if (!rows) {
            LOG_ERROR() << std::format("DB select captures page failed: {}", rows.error().what);
            return std::unexpected(kDbFailure);
        }
        auto dbRows = grabValueOf(rows);
        std::vector<dto::UuidWithTime> items;
        items.reserve(dbRows.size());
        for (const auto &row : dbRows) {
            items.emplace_back(
                row.uuid, us::utils::datetime::TimePointTz(row.timepoint.GetUnderlying())
            );
        }
        if (ssize(items) == impl->pageMax && !items.empty()) {
            const auto &last = items.back();
            auto tp = last.created_at.GetTimePoint();
            crud::Cursor cursor(tp, last.uuid);
            return dto::PagedFindCapturesByUrlResponse{
                .items = std::move(items),
                .next_page_token = std::string(crud::encodeCursor(cursor).view()),
            };
        }
        return dto::PagedFindCapturesByUrlResponse{
            .items = std::move(items), .next_page_token = {}
        };
    }
}

Expected<dto::PagedFindCapturesByPrefixResponse, errors::CapturePageError>
Crud::findCapturesByPrefixPage(String normalizedPrefix, String pageToken)
{
    namespace crud = v1::crud;
    using enum errors::CapturePageError;

    std::optional<crud::PrefixCursor> cur;
    if (!pageToken.empty()) {
        cur = crud::decodePrefixCursor(pageToken);
        if (!cur)
            return std::unexpected(kInvalidPageToken);
        if (cur->prefix != normalizedPrefix)
            return std::unexpected(kMismatchedPageToken);
    }
    const std::string upper = crud::upperExclusiveBound(normalizedPrefix);
    const auto linksPerPage = impl->linksPerPageMax;

    auto selectLinksFirst = [&](i64 limit) {
        return impl->readonly(
            [&](auto &res) { return res.template AsContainer<std::vector<String>>(); },
            sql::kSelectDistinctLinksByPrefixFirst, normalizedPrefix, upper, raw(limit)
        );
    };
    auto selectLinksNext = [&](String fromLink, i64 limit) {
        return impl->readonly(
            [&](auto &res) { return res.template AsContainer<std::vector<String>>(); },
            sql::kSelectDistinctLinksByPrefixNext, normalizedPrefix, upper, fromLink, raw(limit)
        );
    };

    std::vector<String> links;
    links.reserve(numericCast<size_t>(linksPerPage));
    if (cur) {
        const auto &cursorLink = cur->link;
        if (cur->createdAt) {
            links.push_back(cursorLink);
            if (linksPerPage > 1_i64) {
                auto more = selectLinksNext(cursorLink, linksPerPage - 1_i64);
                if (!more) {
                    LOG_ERROR() << std::format(
                        "DB select prefix links failed: {}", more.error().what
                    );
                    return std::unexpected(kDbFailure);
                }
                links.insert(std::end(links), std::begin(*more), std::end(*more));
            }
        } else {
            auto more = selectLinksNext(cursorLink, linksPerPage);
            if (!more) {
                LOG_ERROR() << std::format("DB select prefix links failed: {}", more.error().what);
                return std::unexpected(kDbFailure);
            }
            links.insert(std::end(links), std::begin(*more), std::end(*more));
        }
    } else {
        auto first = selectLinksFirst(linksPerPage);
        if (!first) {
            LOG_ERROR() << std::format("DB select prefix links failed: {}", first.error().what);
            return std::unexpected(kDbFailure);
        }
        links.insert(std::end(links), std::begin(*first), std::end(*first));
    }

    struct Row {
        Uuid uuid;
        pg::TimePointTz tp;
    };
    std::vector<dto::UuidWithTimeLink> items;
    items.reserve(numericCast<size_t>(ssize(links) * impl->perLinkMax));
    bool endedMidLink = false;
    String lastLink;
    std::optional<Row> lastRow;

    auto selectRowsForLink = [&](const String &link, i64 idx) {
        if (idx == 0_i64 && cur && cur->createdAt && cur->id) {
            return impl->readonly(
                [&](auto &res) { return res.template AsContainer<std::vector<Row>>(pg::kRowTag); },
                sql::kSelectCaptureByLinkNext, link, raw(impl->perLinkMax),
                pg::TimePointTz(*cur->createdAt), *cur->id
            );
        }
        return impl->readonly(
            [&](auto &res) { return res.template AsContainer<std::vector<Row>>(pg::kRowTag); },
            sql::kSelectCaptureByLinkFirst, link, raw(impl->perLinkMax)
        );
    };

    const auto linkCount = ssize(links);
    for (i64 idx = 0; idx < linkCount; idx++) {
        const auto &link = links[numericCast<size_t>(idx)];
        auto rows = selectRowsForLink(link, idx);
        if (!rows) {
            LOG_ERROR() << std::format("DB select prefix captures failed: {}", rows.error().what);
            return std::unexpected(kDbFailure);
        }
        for (auto &&r : *rows) {
            items.emplace_back(
                r.uuid, us::utils::datetime::TimePointTz(r.tp.GetUnderlying()),
                std::string(link.view())
            );
        }
        if (!rows->empty()) {
            lastRow = rows->back();
            lastLink = link;
            if (ssize(*rows) == impl->perLinkMax && idx + 1_i64 == linkCount) {
                endedMidLink = true;
            }
        } else {
            lastLink = link;
        }
    }

    std::optional<std::string> next;
    if (!items.empty()) {
        if (endedMidLink && lastRow) {
            const auto tp = lastRow->tp.GetUnderlying();
            next = std::string(
                crud::encodePrefixCursor(normalizedPrefix, lastLink, tp, lastRow->uuid).view()
            );
        } else {
            next = std::string(crud::encodePrefixCursor(normalizedPrefix, lastLink).view());
        }
    }
    return dto::PagedFindCapturesByPrefixResponse{
        .items = std::move(items), .next_page_token = std::move(next)
    };
}

Expected<void, DenylistError> Crud::disallowAndPurgePrefix(String prefixKey) noexcept
{
    auto inserted = impl->denylist.insertPrefix(prefixKey, "disallow_and_purge"_t);
    if (!inserted) {
        impl->metrics.accountError(Metrics::Error::kDenylistCheck);
        return std::unexpected(inserted.error());
    }

    LOG_INFO() << std::format("enqueued for prefix {}", prefixKey);

    impl->purgeBackground.AsyncDetach("purge_prefix_lambda", [implPtr = impl.get(), prefixKey]() {
        try {
            engine::current_task::SetDeadline(
                engine::Deadline::FromDuration(implPtr->purgeJobTimeout)
            );
            LOG_INFO() << std::format("Starting purge for denylisted prefix: {}", prefixKey);
            auto purged = implPtr->purgePrefix(prefixKey);
            if (!purged) {
                LOG_CRITICAL() << std::format("Purge task failed for {}", prefixKey);
                us::utils::AbortWithStacktrace("Purge task failed");
            }
        } catch (const us::utils::TracefulException &e) {
            LOG_CRITICAL() << std::format("Purge task failed for {}: {}", prefixKey, e.what());
            us::utils::AbortWithStacktrace("Purge task failed");
        }
    });
    return {};
}
