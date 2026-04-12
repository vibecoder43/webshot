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

#include <userver/clients/http/component.hpp>
#include <userver/components/component.hpp>
#include <userver/components/component_base.hpp>
#include <userver/components/process_starter.hpp>
#include <userver/concurrent/background_task_storage.hpp>
#include <userver/crypto/base64.hpp>
#include <userver/engine/semaphore.hpp>
#include <userver/engine/sleep.hpp>
#include <userver/engine/task/current_task.hpp>
#include <userver/engine/task/task_processor_fwd.hpp>
#include <userver/formats/json.hpp>
#include <userver/logging/log.hpp>
#include <userver/rcu/rcu.hpp>
#include <userver/storages/postgres/cluster.hpp>
#include <userver/storages/postgres/io/chrono.hpp>
#include <userver/storages/postgres/io/row_types.hpp>
#include <userver/storages/postgres/io/uuid.hpp>
#include <userver/storages/postgres/postgres.hpp>
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
using Uuid = boost::uuids::uuid;
using chrono::system_clock;
namespace {
constexpr i64 kCrawlerSeedAttemptsMax = 2_i64;
constexpr i64 kGiB = 1024_i64 * 1024_i64 * 1024_i64;
constexpr i64 kCpuMaxPeriodUs = 100000_i64;

struct [[nodiscard]] PgError final {
    std::string what;
};

[[nodiscard]] std::optional<crawler::CgroupLimits> computeCrawlerLimits(i64 cpuCores, i64 memoryGib)
{
    if (cpuCores == 0_i64 && memoryGib == 0_i64)
        return {};

    UINVARIANT(cpuCores > 0_i64 && memoryGib > 0_i64, "crawler limits must be both > 0 or both 0");
    const auto maxI64 = i64(std::numeric_limits<int64_t>::max());
    const auto maxMemoryGib = maxI64 / kGiB;
    UINVARIANT(memoryGib <= maxMemoryGib, "memory GiB limit is too large");
    const auto maxCpuCores = maxI64 / kCpuMaxPeriodUs;
    UINVARIANT(cpuCores <= maxCpuCores, "cpu core limit is too large");
    return crawler::CgroupLimits{.cpuCores = cpuCores, .memoryBytes = memoryGib * kGiB};
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
    crawler_devtools_poll_interval_ms:
        type: integer
        minimum: 1
        description: 'Polling interval for devtools socket/path discovery in milliseconds'
    crawler_cdp_wait_poll_interval_ms:
        type: integer
        minimum: 1
        description: 'Polling interval for CDP response wait loop in milliseconds'
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

/** @brief Private pimpl that holds dependencies and query helpers. */
class [[nodiscard]] Crud::Impl {
public:
    const i64 pageMax;
    const i64 perLinkMax;
    const i64 linksPerPageMax;
    const i64 crawlerRunTimeoutSec;
    const i64 crawlerCpuCores;
    const i64 crawlerMemoryGib;
    const i64 crawlerJobOverheadTimeoutSec;
    const i64 crawlerPostLoadDelaySec;
    const i64 crawlerNetIdleWaitSec;
    const i64 crawlerPageExtraDelaySec;
    const i64 crawlerBehaviorTimeoutSec;
    const i64 crawlerDevtoolsStartupTimeoutSec;
    const i64 crawlerCdpHandshakeTimeoutSec;
    const i64 crawlerCdpCommandTimeoutSec;
    const i64 crawlerDevtoolsPollIntervalMs;
    const i64 crawlerCdpWaitPollIntervalMs;
    const i64 crawlerBrowserStopTimeoutMs;
    const i64 crawlerProxyStopTimeoutMs;
    const i64 linkCooldownSec;
    const i64 crawlJobRetentionSec;
    const i64 crawlJobCleanupIntervalSec;
    const bool s3UseSts;
    const String s3CredentialsEndpoint;
    const i64 s3CredentialsDurationSec;
    const i64 s3CredentialsRefreshMarginSec;
    const i64 s3CredentialsRefreshRetrySec;
    const i64 purgeJobTimeoutSec;
    const i64 purgeDeleteBatchSize;
    const Config &svcCfg;
    pg::ClusterPtr cluster;
    pg::ClusterPtr sharedCluster;
    us::clients::http::Client &httpClient;
    us::engine::subprocess::ProcessStarter &processStarter;
    CrawlerRunner crawlerRunner;
    struct [[nodiscard]] S3ClientState {
        s3v4::S3Credentials creds;
        system_clock::time_point expiresAt;
        std::shared_ptr<s3v4::S3V4Client> client;
    };
    rcu::Variable<S3ClientState> s3State;
    s3v4::AccessKeyId staticAccessKeyId;
    s3v4::SecretAccessKey staticSecretAccessKey;
    Denylist &denylist;
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
    [[nodiscard]] Expected<std::optional<dto::CaptureJob>, PgError>
    findLatestJobForLink(const String &link);
    [[nodiscard]] Expected<void, PgError> markJobRunning(Uuid id);
    [[nodiscard]] Expected<void, PgError>
    markJobSucceeded(Uuid id, const us::utils::datetime::TimePointTz &createdAt);
    [[nodiscard]] Expected<void, PgError>
    markJobFailed(Uuid id, const String &errorCategory, const String &errorMessage);
    [[nodiscard]] Expected<std::optional<dto::CaptureJob>, PgError> loadJob(Uuid id);
    [[nodiscard]] Expected<void, errors::CrawlFailure> runCrawlerForContext(CrawlContext &ctx);
    [[nodiscard]] crawler::AttemptSummary
    runCrawlerAttempt(CrawlContext &ctx, const String &seedUrl);
    [[nodiscard]] std::optional<us::utils::datetime::TimePointTz>
    persistMetadataForContext(const CrawlContext &ctx);
    [[nodiscard]] Expected<void, errors::CrudError> purgePrefix(const String &prefixKey);
    [[nodiscard]] S3ClientState fetchS3ClientStateFromSts() const;
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
          crawlerRunTimeoutSec(cfg["crawler_run_timeout_sec"].As<int64_t>()),
          crawlerCpuCores(cfg["crawler_cpu_cores"].As<int64_t>()),
          crawlerMemoryGib(cfg["crawler_memory_gib"].As<int64_t>()),
          crawlerJobOverheadTimeoutSec(cfg["crawler_job_overhead_timeout_sec"].As<int64_t>()),
          crawlerPostLoadDelaySec(cfg["crawler_post_load_delay_sec"].As<int64_t>()),
          crawlerNetIdleWaitSec(cfg["crawler_net_idle_wait_sec"].As<int64_t>()),
          crawlerPageExtraDelaySec(cfg["crawler_page_extra_delay_sec"].As<int64_t>()),
          crawlerBehaviorTimeoutSec(cfg["crawler_behavior_timeout_sec"].As<int64_t>()),
          crawlerDevtoolsStartupTimeoutSec(
              cfg["crawler_devtools_startup_timeout_sec"].As<int64_t>()
          ),
          crawlerCdpHandshakeTimeoutSec(cfg["crawler_cdp_handshake_timeout_sec"].As<int64_t>()),
          crawlerCdpCommandTimeoutSec(cfg["crawler_cdp_command_timeout_sec"].As<int64_t>()),
          crawlerDevtoolsPollIntervalMs(cfg["crawler_devtools_poll_interval_ms"].As<int64_t>()),
          crawlerCdpWaitPollIntervalMs(cfg["crawler_cdp_wait_poll_interval_ms"].As<int64_t>()),
          crawlerBrowserStopTimeoutMs(cfg["crawler_browser_stop_timeout_ms"].As<int64_t>()),
          crawlerProxyStopTimeoutMs(cfg["crawler_proxy_stop_timeout_ms"].As<int64_t>()),
          linkCooldownSec(cfg["link_cooldown_sec"].As<int64_t>()),
          crawlJobRetentionSec(cfg["crawl_job_retention_sec"].As<int64_t>()),
          crawlJobCleanupIntervalSec(cfg["crawl_job_cleanup_interval_sec"].As<int64_t>()),
          s3UseSts(cfg["s3_use_sts"].As<bool>()),
          s3CredentialsEndpoint(
              String::fromBytes(cfg["s3_credentials_endpoint"].As<std::string>()).expect()
          ),
          s3CredentialsDurationSec(cfg["s3_credentials_duration_sec"].As<int64_t>()),
          s3CredentialsRefreshMarginSec(cfg["s3_credentials_refresh_margin_sec"].As<int64_t>()),
          s3CredentialsRefreshRetrySec(cfg["s3_credentials_refresh_retry_sec"].As<int64_t>()),
          purgeJobTimeoutSec(cfg["purge_job_timeout_sec"].As<int64_t>()),
          purgeDeleteBatchSize(cfg["purge_delete_batch_size"].As<int64_t>()),
          svcCfg(ctx.FindComponent<Config>()),
          cluster(ctx.FindComponent<us::components::Postgres>("capture_meta_db").GetCluster()),
          sharedCluster(
              ctx.FindComponent<us::components::Postgres>("shared_state_db").GetCluster()
          ),
          httpClient(ctx.FindComponent<us::components::HttpClient>().GetHttpClient()),
          processStarter(ctx.FindComponent<us::components::ProcessStarter>().Get()),
          crawlerRunner(
              httpClient, processStarter, chrono::seconds{crawlerRunTimeoutSec},
              std::string(svcCfg.stateDir()),
              computeCrawlerLimits(crawlerCpuCores, crawlerMemoryGib),
              crawler::CaptureTimings{
                  chrono::seconds{crawlerPostLoadDelaySec},
                  chrono::seconds{crawlerNetIdleWaitSec},
                  chrono::seconds{crawlerPageExtraDelaySec},
                  chrono::seconds{crawlerBehaviorTimeoutSec},
              },
              crawler::CrawlerTunables{
                  chrono::seconds{crawlerDevtoolsStartupTimeoutSec},
                  chrono::seconds{crawlerCdpHandshakeTimeoutSec},
                  chrono::seconds{crawlerCdpCommandTimeoutSec},
                  chrono::milliseconds{crawlerDevtoolsPollIntervalMs},
                  chrono::milliseconds{crawlerCdpWaitPollIntervalMs},
                  chrono::milliseconds{crawlerBrowserStopTimeoutMs},
                  chrono::milliseconds{crawlerProxyStopTimeoutMs},
              }
          ),
          denylist(ctx.FindComponent<Denylist>()),
          mainTaskProcessor(ctx.GetTaskProcessor("main-task-processor")),
          crawlSlots(engine::GetWorkerCount(mainTaskProcessor)),
          purgeTaskProcessor(ctx.GetTaskProcessor("purge_task_processor")),
          credsRefreshTaskProcessor(ctx.GetTaskProcessor("creds_refresh_task_processor")),
          s3RefreshTask(), crawlJobCleanupTask(), purgeBackground(purgeTaskProcessor),
          crawlBackground(mainTaskProcessor)
    {
        const auto fixedTimingBudgetSec = crawlerPostLoadDelaySec + crawlerNetIdleWaitSec +
                                          crawlerPageExtraDelaySec + crawlerBehaviorTimeoutSec;
        UINVARIANT(
            fixedTimingBudgetSec <= crawlerRunTimeoutSec,
            "crawler fixed timing budget must be <= crawler_run_timeout_sec"
        );
        UINVARIANT(
            s3CredentialsDurationSec > s3CredentialsRefreshMarginSec,
            "s3_credentials_duration_sec must be greater than s3_credentials_refresh_margin_sec"
        );
        UINVARIANT(crawlJobRetentionSec > 0_i64, "crawl_job_retention_sec must be positive");
        UINVARIANT(
            crawlJobCleanupIntervalSec > 0_i64, "crawl_job_cleanup_interval_sec must be positive"
        );
        const auto &secdist = ctx.FindComponent<us::components::Secdist>().Get();
        const auto &creds = secdist.Get<S3CredentialsSecdist>();
        UINVARIANT(
            creds.accessKeyId && creds.secretAccessKey, "missing required S3 secdist credentials"
        );
        staticAccessKeyId = creds.accessKeyId.value();
        staticSecretAccessKey = creds.secretAccessKey.value();
        S3ClientState initialState;
        if (s3UseSts) {
            initialState = fetchS3ClientStateFromSts();
            startS3RefreshTask();
        } else {
            S3ClientState state;
            state.creds = s3v4::S3Credentials(
                staticAccessKeyId, staticSecretAccessKey, creds.sessionToken
            );
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

    template <pg::ClusterHostType Host, typename F, typename... Ts>
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
            return Out{std::unexpected(PgError{.what = std::string(e.what())})};
        }
    }

    template <typename F, typename... Ts> [[nodiscard]] auto readonly(F &&f, Ts &&...args)
    {
        return execDb<pg::ClusterHostType::kSlaveOrMaster>(
            cluster, std::forward<F>(f), std::forward<Ts>(args)...
        );
    }

    template <typename F, typename... Ts> [[nodiscard]] auto readwrite(F &&f, Ts &&...args)
    {
        return execDb<pg::ClusterHostType::kMaster>(
            cluster, std::forward<F>(f), std::forward<Ts>(args)...
        );
    }

    template <typename F, typename... Ts> [[nodiscard]] auto sharedReadonly(F &&f, Ts &&...args)
    {
        return execDb<pg::ClusterHostType::kSlaveOrMaster>(
            sharedCluster, std::forward<F>(f), std::forward<Ts>(args)...
        );
    }

    template <typename F, typename... Ts> [[nodiscard]] auto sharedReadwrite(F &&f, Ts &&...args)
    {
        return execDb<pg::ClusterHostType::kMaster>(
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
    String location;
    std::optional<String> failureMessage;

    CrawlContext(Uuid id, Link link, const Config &cfg)
        : link(std::move(link)), id(id), keyOnly(text::format("{}", id)),
          s3Key(text::format("{}/{}", cfg.s3Bucket(), keyOnly)),
          location(text::format("{}/{}", cfg.publicBaseUrl(), keyOnly))
    {
    }
};

[[nodiscard]] Expected<dto::UuidWithTimeLink, errors::CrawlFailure>
Crud::Impl::runCrawlJob(Uuid id, Link link)
{
    using enum errors::CrawlError;

    const auto totalCrawlTimeLimitSec = crawlerJobOverheadTimeoutSec +
                                        crawlerRunTimeoutSec * kCrawlerSeedAttemptsMax;
    engine::current_task::SetDeadline(
        engine::Deadline::FromDuration(chrono::seconds{totalCrawlTimeLimitSec})
    );

    std::shared_lock<engine::CancellableSemaphore> slotLock(crawlSlots);

    CrawlContext ctx(id, std::move(link), svcCfg);

    LOG_INFO() << std::format(
        "runCrawlJob starting crawler for job {} ({})", id, ctx.link.normalized()
    );
    {
        const auto ran = runCrawlerForContext(ctx);
        if (!ran)
            return std::unexpected(ran.error());
    }
    LOG_INFO() << std::format(
        "runCrawlJob finished crawler for job {} ({})", id, ctx.link.normalized()
    );

    LOG_INFO() << std::format("Persisting metadata for job {} ({})", id, ctx.link.normalized());
    auto createdAt = persistMetadataForContext(ctx);
    if (!createdAt)
        return std::unexpected(errors::CrawlFailure{.code = kPersistMetadataFailed, .detail = {}});
    LOG_INFO() << std::format("Persisted metadata for job {} ({})", id, ctx.link.normalized());
    return dto::UuidWithTimeLink{
        ctx.id, createdAt.value(), std::string(ctx.link.normalized().view())
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
    return us::utils::datetime::TimePointTz(static_cast<system_clock::time_point>(row->createdAt));
}

Expected<void, PgError> Crud::Impl::markJobRunning(Uuid id)
{
    return sharedReadwrite(
        [&](auto &res) { static_cast<void>(res); }, sql::kUpdateCrawlJobRunning, id
    );
}

Expected<void, PgError>
Crud::Impl::markJobSucceeded(Uuid id, const us::utils::datetime::TimePointTz &createdAt)
{
    return sharedReadwrite(
        [&](auto &res) { static_cast<void>(res); }, sql::kUpdateCrawlJobSucceeded, id,
        pg::TimePointTz(createdAt.GetTimePoint())
    );
}

Expected<void, PgError>
Crud::Impl::markJobFailed(Uuid id, const String &errorCategory, const String &errorMessage)
{
    return sharedReadwrite(
        [&](auto &res) { static_cast<void>(res); }, sql::kUpdateCrawlJobFailed, id, errorCategory,
        errorMessage
    );
}

Expected<std::optional<dto::CaptureJob>, PgError> Crud::Impl::loadJob(Uuid id)
{
    struct Row {
        Uuid uuid;
        String link;
        std::string status;
        std::optional<std::string> errorCategory;
        std::optional<std::string> errorMessage;
        pg::TimePointTz createdAt;
        std::optional<pg::TimePointTz> startedAt;
        std::optional<pg::TimePointTz> finishedAt;
        std::optional<pg::TimePointTz> resultCreatedAt;
    };
    auto rowOpt = sharedReadonly(
        [&](auto &res) { return res.template AsOptionalSingleRow<Row>(pg::kRowTag); },
        sql::kSelectCrawlJob, id
    );
    if (!rowOpt)
        return std::unexpected(std::move(rowOpt).error());
    if (!rowOpt.value())
        return {};
    auto row = grabValueOf(grabValueOf(rowOpt));

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
    job.created_at = us::utils::datetime::TimePointTz(
        static_cast<system_clock::time_point>(row.createdAt)
    );
    if (row.startedAt)
        job.started_at = us::utils::datetime::TimePointTz(
            static_cast<system_clock::time_point>(row.startedAt.value())
        );
    if (row.finishedAt)
        job.finished_at = us::utils::datetime::TimePointTz(
            static_cast<system_clock::time_point>(row.finishedAt.value())
        );
    if (row.resultCreatedAt)
        job.result_created_at = us::utils::datetime::TimePointTz(
            static_cast<system_clock::time_point>(row.resultCreatedAt.value())
        );
    if (job.status == dto::CaptureJob::Status::kFailed) {
        // Never expose internal diagnostics (crawler details, exception text, etc) to API clients.
        std::string message = "internal server error";
        if (row.errorCategory) {
            if (row.errorCategory.value() == "size_limit") {
                message = "capture exceeded archive size limit";
            } else if (row.errorCategory.value() == "crawler_failed") {
                message = "capture failed";
            } else if (row.errorCategory.value() == "internal_server_error") {
                message = "internal server error";
            }
        }
        dto::ErrorEnvelope::Error err{std::move(message)};
        job.error = dto::ErrorEnvelope{err};
    }
    if (job.status == dto::CaptureJob::Status::kSucceeded && job.result_created_at)
        job.result = dto::UuidWithTimeLink(job.uuid, job.result_created_at.value(), job.link);
    return {std::move(job)};
}

Crud::Impl::S3ClientState Crud::Impl::fetchS3ClientStateFromSts() const
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
        svcCfg.s3Region(), kRoleArnDescription, sessionName, policyJson,
        chrono::seconds{s3CredentialsDurationSec}, svcCfg.s3Timeout()
    );
    if (!sts)
        us::utils::AbortWithStacktrace("failed to fetch STS credentials");

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
    auto idOpt = sharedReadonly(
        [&](auto &res) { return res.template AsOptionalSingleRow<Uuid>(); },
        sql::kSelectLatestCrawlJobByLink, link
    );
    if (!idOpt)
        return std::unexpected(std::move(idOpt).error());
    if (!idOpt.value())
        return {};
    return loadJob(grabValueOf(grabValueOf(idOpt)));
}

void Crud::Impl::startS3RefreshTask()
{
    auto snapshot = s3State.Read();
    const auto now = us::utils::datetime::Now();
    auto delay = s3refresh::computeRefreshDelay(
        now, snapshot->expiresAt, s3CredentialsRefreshMarginSec
    );

    us::utils::PeriodicTask::Settings settings(
        chrono::duration_cast<chrono::milliseconds>(delay), chrono::milliseconds(0)
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
                now, newState.expiresAt, s3CredentialsRefreshMarginSec
            );

            us::utils::PeriodicTask::Settings settings(
                chrono::duration_cast<chrono::milliseconds>(nextDelay), chrono::milliseconds(0)
            );
            settings.task_processor = &credsRefreshTaskProcessor;
            s3RefreshTask.SetSettings(settings);
            break;
        } catch (const us::utils::TracefulException &e) {
            LOG_ERROR() << std::format("Failed to refresh S3 credentials from STS: {}", e.what());
            engine::SleepFor(chrono::seconds{s3CredentialsRefreshRetrySec});
        }
    }
}

void Crud::Impl::startCrawlJobCleanupTask()
{
    auto interval = chrono::duration_cast<chrono::milliseconds>(
        chrono::seconds{crawlJobCleanupIntervalSec}
    );
    us::utils::PeriodicTask::Settings settings(interval, chrono::milliseconds(0));
    settings.task_processor = &purgeTaskProcessor;

    crawlJobCleanupTask.Start("crawl_job_cleanup", settings, [this]() { cleanupOldJobs(); });
}

void Crud::Impl::cleanupOldJobs()
{
    const auto now = us::utils::datetime::Now();
    const auto cutoff = now - chrono::seconds{crawlJobRetentionSec};
    const auto deleted = sharedReadwrite(
        [&](auto &res) { static_cast<void>(res); }, sql::kDeleteCrawlJobsExpired,
        pg::TimePointTz(cutoff)
    );
    if (!deleted) {
        LOG_ERROR() << std::format("Failed to delete old crawl jobs: {}", deleted.error().what);
    }
}

crawler::AttemptSummary Crud::Impl::runCrawlerAttempt(CrawlContext &ctx, const String &seedUrl)
{
    LOG_INFO() << std::format(
        "Submitting crawl for {} to embedded crawler with timeout={}s", seedUrl,
        crawlerRunTimeoutSec
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
        return run.attempt;
    }

    LOG_INFO() << std::format("Uploading WACZ for {} to {}", seedUrl, ctx.s3Key);
    UINVARIANT(run.wacz, "embedded crawler reported missing WACZ payload");
    auto snapshot = s3State.Read();
    snapshot->client->PutObject(ctx.s3Key.view(), run.wacz.value(), {}, "application/zip", {}, {});
    LOG_INFO() << std::format("Uploaded WACZ for {} to {}", seedUrl, ctx.s3Key);

    return run.attempt;
}

Expected<void, errors::CrawlFailure> Crud::Impl::runCrawlerForContext(CrawlContext &ctx)
{
    using enum errors::CrawlError;

    const auto httpsSeedUrl = ctx.link.httpsUrl();
    const auto httpSeedUrl = ctx.link.httpUrl();

    auto result = crawler::runHttpsFirstWithHttpFallback(
        httpsSeedUrl, httpSeedUrl,
        [&ctx, this](const String &seedUrlIn) { return runCrawlerAttempt(ctx, seedUrlIn); }
    );

    if (result.outcome == crawler::RunOutcome::kSucceeded) {
        if (result.httpAttempt) {
            LOG_INFO() << "HTTP fallback succeeded after HTTPS failed with no response";
        }
        return {};
    }

    if (result.httpsAttempt.seedProbe && result.httpAttempt) {
        LOG_INFO() << std::format(
            "HTTPS seed probe before HTTP fallback: status={}, loadState={}",
            result.httpsAttempt.seedProbe->status.value_or(0),
            result.httpsAttempt.seedProbe->loadState.value_or(-1)
        );
    }

    if (result.outcome == crawler::RunOutcome::kFailedSizeLimit) {
        const auto attempt = result.httpAttempt ? result.httpAttempt.value() : result.httpsAttempt;
        ctx.failureMessage =
            String::fromBytes(
                std::format(
                    "Failed to crawl {} ({})", result.httpAttempt ? httpSeedUrl : httpsSeedUrl,
                    crawler::formatAttemptStatus(result.httpAttempt ? "http" : "https", attempt)
                )
            )
                .expect();
        LOG_INFO() << std::string(ctx.failureMessage->view());
        return std::unexpected(errors::CrawlFailure{.code = kSizeLimit, .detail = {}});
    }

    if (result.outcome == crawler::RunOutcome::kFailedChildNoExit) {
        const auto attempt = result.httpAttempt ? result.httpAttempt.value() : result.httpsAttempt;
        const auto attemptContext = crawler::formatAttemptContext(attempt);
        ctx.failureMessage = String::fromBytes(
                                 std::format(
                                     "Failed to crawl {}, child process did not exit cleanly{}",
                                     result.httpAttempt ? httpSeedUrl : httpsSeedUrl,
                                     attemptContext.empty() ? std::string()
                                                            : std::format(" ({})", attemptContext)
                                 )
        )
                                 .expect();
        LOG_INFO() << std::string(ctx.failureMessage->view());
        return std::unexpected(errors::CrawlFailure{.code = kFailed, .detail = ctx.failureMessage});
    }

    if (result.outcome == crawler::RunOutcome::kFailedNoWacz) {
        const auto attempt = result.httpAttempt ? result.httpAttempt.value() : result.httpsAttempt;
        const auto attemptContext = crawler::formatAttemptContext(attempt);
        ctx.failureMessage = String::fromBytes(
                                 std::format(
                                     "Failed to crawl {}, no WACZ{}",
                                     result.httpAttempt ? httpSeedUrl : httpsSeedUrl,
                                     attemptContext.empty() ? std::string()
                                                            : std::format(" ({})", attemptContext)
                                 )
        )
                                 .expect();
        LOG_INFO() << std::string(ctx.failureMessage->view());
        return std::unexpected(errors::CrawlFailure{.code = kFailed, .detail = ctx.failureMessage});
    }

    ctx.failureMessage =
        String::fromBytes(
            std::format(
                "Failed to crawl {} ({})", ctx.link.normalized(),
                result.httpAttempt
                    ? std::format(
                          "{}, {}", crawler::formatAttemptStatus("https", result.httpsAttempt),
                          crawler::formatAttemptStatus("http", result.httpAttempt.value())
                      )
                    : std::string(crawler::formatAttemptStatus("https", result.httpsAttempt).view())
            )
        )
            .expect();
    LOG_INFO() << std::string(ctx.failureMessage->view());
    return std::unexpected(errors::CrawlFailure{.code = kFailed, .detail = ctx.failureMessage});
}

[[nodiscard]] std::optional<us::utils::datetime::TimePointTz>
Crud::Impl::persistMetadataForContext(const CrawlContext &ctx)
{
    const auto prefixKey = prefix::makePrefixKey(ctx.link);
    const auto prefixTree = prefix::makePrefixTree(prefixKey);
    const auto host = ctx.link.url.hostname();

    const auto allowed = denylist.isAllowedPrefix(prefixKey);
    if (!allowed || !allowed.value()) {
        try {
            auto snapshot = s3State.Read();
            snapshot->client->DeleteObject(ctx.s3Key.view());
        } catch (const us::utils::TracefulException &) {
            LOG_ERROR() << std::format("error deleting {}", ctx.s3Key);
        }
        if (!allowed) {
            LOG_ERROR() << std::format("Failed to check denylist state during crawl: {}", host);
        } else {
            LOG_INFO() << std::format("Host became denylisted during crawl: {}", host);
        }
        return {};
    }

    struct Row {
        Uuid id;
        pg::TimePointTz createdAt;
    };
    auto row = readwrite(
        [&](auto &res) { return res.template AsSingleRow<Row>(pg::kRowTag); }, sql::kInsertCapture,
        ctx.id, ctx.link.normalized(), prefixKey, prefixTree, ctx.location
    );
    if (!row) {
        try {
            auto snapshot = s3State.Read();
            snapshot->client->DeleteObject(ctx.s3Key.view());
        } catch (const us::utils::TracefulException &) {
            LOG_ERROR() << std::format("error deleting {}", ctx.s3Key);
        }
        LOG_ERROR() << std::format("DB insert failed for {}: {}", ctx.id, row.error().what);
        return {};
    }
    return us::utils::datetime::TimePointTz(static_cast<system_clock::time_point>(row->createdAt));
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
        for (auto &&id : ids.value()) {
            const auto key = std::format("{}/{}", svcCfg.s3Bucket(), id);
            try {
                auto snapshot = s3State.Read();
                snapshot->client->DeleteObject(key);
            } catch (const us::utils::TracefulException &e) {
                LOG_ERROR() << std::format(
                    "S3 delete failed for key {} (prefix={}): {}", key, prefixKey, e.what()
                );
                return std::unexpected(kDbFailure);
            }

            single.clear();
            single.emplace_back(id);
            auto deleted = readwrite(
                [&](auto &res) { static_cast<void>(res); }, sql::kDeleteCapturesByIds, single
            );
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

    if (implPtr->linkCooldownSec > 0_i64) {
        auto latestJob = implPtr->findLatestJobForLink(normalizedLink);
        if (!latestJob) {
            LOG_ERROR() << std::format(
                "DB select latest crawl job failed for {}: {}", normalizedLink,
                latestJob.error().what
            );
            return std::unexpected(kDbFailure);
        }
        auto latestJobOpt = grabValueOf(latestJob);
        if (latestJobOpt) {
            auto job = grabValueOf(latestJobOpt);
            const auto now = us::utils::datetime::Now();
            const auto lastCreated = job.created_at.GetTimePoint();
            const auto deadline = lastCreated + chrono::seconds{implPtr->linkCooldownSec};
            if (now < deadline)
                return std::move(job);
        }
    }

    auto id = us::utils::generators::GenerateBoostUuid();
    auto createdAt = implPtr->insertJob(id, normalizedLink);
    if (!createdAt) {
        LOG_ERROR() << std::format(
            "Failed to create crawl job for {}: {}", normalizedLink, createdAt.error().what
        );
        return std::unexpected(kDbFailure);
    }
    const auto createdAtTp = grabValueOf(createdAt);
    implPtr->crawlBackground.AsyncDetach("crawl_job", [implPtr, id, link = std::move(link)]() {
        auto markInternalError = [&](std::string_view what) {
            LOG_ERROR() << std::format("Unexpected crawl job failure for {}: {}", id, what);
            const auto marked = implPtr->markJobFailed(
                id, "internal_server_error"_t, "internal server error"_t
            );
            if (!marked) {
                LOG_ERROR() << std::format(
                    "DB update crawl job failed for {}: {}", id, marked.error().what
                );
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
                Expected<void, PgError> marked;
                if (result.error().code == kSizeLimit) {
                    marked = implPtr->markJobFailed(
                        id, "size_limit"_t, "capture exceeded archive size limit"_t
                    );
                } else if (result.error().code == kPersistMetadataFailed) {
                    marked = implPtr->markJobFailed(
                        id, "internal_server_error"_t, "internal server error"_t
                    );
                } else if (result.error().detail) {
                    marked = implPtr->markJobFailed(
                        id, "crawler_failed"_t, result.error().detail.value()
                    );
                } else {
                    marked = implPtr->markJobFailed(id, "crawler_failed"_t, "crawler failed"_t);
                }
                if (!marked) {
                    LOG_ERROR() << std::format(
                        "DB update crawl job failed for {}: {}", id, marked.error().what
                    );
                }
                return;
            }

            const auto succeeded = implPtr->markJobSucceeded(id, result->created_at);
            if (!succeeded) {
                LOG_ERROR() << std::format(
                    "DB update crawl job failed for {}: {}", id, succeeded.error().what
                );
            }
        } catch (const us::utils::TracefulException &e) {
            markInternalError(e.what());
        } catch (const std::exception &e) {
            markInternalError(e.what());
        }
    });

    return dto::CaptureJob{
        .uuid = id,
        .link = std::string(normalizedLink.view()),
        .status = dto::CaptureJob::Status::kPending,
        .created_at = createdAtTp,
        .started_at = {},
        .finished_at = {},
        .result_created_at = {},
        .result = {},
        .error = {},
    };
}

Expected<std::optional<Link>, errors::CrudError> Crud::findCapture(Uuid uuid)
{
    using enum errors::CrudError;

    auto location = impl->readonly(
        [&](auto &res) { return res.template AsOptionalSingleRow<std::string>(); },
        sql::kSelectCapture, uuid
    );
    if (!location) {
        LOG_ERROR() << std::format(
            "DB select capture failed for {}: {}", uuid, location.error().what
        );
        return std::unexpected(kDbFailure);
    }
    auto locationOpt = grabValueOf(location);
    if (!locationOpt) {
        LOG_INFO() << std::format("UUID not found: {}", uuid);
        return {};
    }
    auto locationText = String::fromBytes(grabValueOf(locationOpt));
    if (!locationText)
        return std::unexpected(kCorruptData);
    auto link = Link::fromText(
        locationText.value(), impl->svcCfg.queryPartLengthMax(), Link::FromTextOptions::kNone
    );
    if (!link)
        return std::unexpected(kCorruptData);
    return {grabValueOf(link)};
}

Expected<std::optional<dto::CaptureJob>, errors::CrudError> Crud::findCaptureJob(Uuid uuid)
{
    using enum errors::CrudError;

    auto job = impl->loadJob(uuid);
    if (!job) {
        LOG_ERROR() << std::format("DB select job failed for {}: {}", uuid, job.error().what);
        return std::unexpected(kDbFailure);
    }
    return job.value();
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
                row.uuid, us::utils::datetime::TimePointTz(
                              static_cast<system_clock::time_point>(row.timepoint)
                          )
            );
        }
        if (safeSize(items) == impl->pageMax && !items.empty()) {
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
                row.uuid, us::utils::datetime::TimePointTz(
                              static_cast<system_clock::time_point>(row.timepoint)
                          )
            );
        }
        if (safeSize(items) == impl->pageMax && !items.empty()) {
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
                links.insert(std::end(links), std::begin(more.value()), std::end(more.value()));
            }
        } else {
            auto more = selectLinksNext(cursorLink, linksPerPage);
            if (!more) {
                LOG_ERROR() << std::format("DB select prefix links failed: {}", more.error().what);
                return std::unexpected(kDbFailure);
            }
            links.insert(std::end(links), std::begin(more.value()), std::end(more.value()));
        }
    } else {
        auto first = selectLinksFirst(linksPerPage);
        if (!first) {
            LOG_ERROR() << std::format("DB select prefix links failed: {}", first.error().what);
            return std::unexpected(kDbFailure);
        }
        links.insert(std::end(links), std::begin(first.value()), std::end(first.value()));
    }

    struct Row {
        Uuid uuid;
        pg::TimePointTz tp;
    };
    std::vector<dto::UuidWithTimeLink> items;
    items.reserve(numericCast<size_t>(safeSize(links) * impl->perLinkMax));
    bool endedMidLink = false;
    String lastLink;
    std::optional<Row> lastRow;

    auto selectRowsForLink = [&](const String &link, i64 idx) {
        if (idx == 0_i64 && cur && cur->createdAt && cur->id) {
            return impl->readonly(
                [&](auto &res) { return res.template AsContainer<std::vector<Row>>(pg::kRowTag); },
                sql::kSelectCaptureByLinkNext, link, raw(impl->perLinkMax),
                pg::TimePointTz(cur->createdAt.value()), cur->id.value()
            );
        }
        return impl->readonly(
            [&](auto &res) { return res.template AsContainer<std::vector<Row>>(pg::kRowTag); },
            sql::kSelectCaptureByLinkFirst, link, raw(impl->perLinkMax)
        );
    };

    const auto linkCount = safeSize(links);
    for (i64 idx = 0; idx < linkCount; idx++) {
        const auto &link = links[numericCast<size_t>(idx)];
        auto rows = selectRowsForLink(link, idx);
        if (!rows) {
            LOG_ERROR() << std::format("DB select prefix captures failed: {}", rows.error().what);
            return std::unexpected(kDbFailure);
        }
        for (auto &&r : rows.value()) {
            items.emplace_back(
                r.uuid,
                us::utils::datetime::TimePointTz(static_cast<system_clock::time_point>(r.tp)),
                std::string(link.view())
            );
        }
        if (!rows->empty()) {
            lastRow = rows->back();
            lastLink = link;
            if (safeSize(rows.value()) == impl->perLinkMax && idx + 1_i64 == linkCount) {
                endedMidLink = true;
            }
        } else {
            lastLink = link;
        }
    }

    std::optional<std::string> next;
    if (!items.empty()) {
        if (endedMidLink && lastRow) {
            const auto tp = static_cast<system_clock::time_point>(lastRow->tp);
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
    if (!inserted)
        return std::unexpected(inserted.error());

    LOG_INFO() << std::format("enqueued for prefix {}", prefixKey);

    impl->purgeBackground.AsyncDetach("purge_prefix_lambda", [implPtr = impl.get(), prefixKey]() {
        try {
            engine::current_task::SetDeadline(
                engine::Deadline::FromDuration(chrono::seconds{implPtr->purgeJobTimeoutSec})
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
