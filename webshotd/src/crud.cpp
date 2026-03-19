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

#include <webshot/sql_queries.hpp>

#include <chrono>
#include <cstdlib>
#include <exception>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <boost/uuid/uuid.hpp>

#include <fmt/format.h>

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
    crawler_job_overhead_timeout_sec:
        type: integer
        minimum: 1
        description: 'Extra timeout budget added around crawler runs for upload and metadata persistence'
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

Crud::Crud(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : us::components::ComponentBase(config, context),
      impl(std::make_unique<Crud::Impl>(config, context))
{
}

Crud::~Crud() = default;

struct [[nodiscard]] CrawlContext;

/** @brief Private pimpl that holds dependencies and query helpers. */
class [[nodiscard]] Crud::Impl {
public:
    const i64 pageMax;
    const i64 perLinkMax;
    const i64 linksPerPageMax;
    const i64 crawlerRunTimeoutSec;
    const i64 crawlerJobOverheadTimeoutSec;
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

    [[nodiscard]] dto::UuidWithTimeLink runCrawlJob(Uuid id, Link link);
    [[nodiscard]] us::utils::datetime::TimePointTz insertJob(Uuid id, String link);
    [[nodiscard]] std::optional<dto::CaptureJob> findLatestJobForLink(const String &link);
    void markJobRunning(Uuid id);
    void markJobSucceeded(Uuid id, const us::utils::datetime::TimePointTz &createdAt);
    void markJobFailed(Uuid id, const String &errorCategory, const String &errorMessage);
    [[nodiscard]] std::optional<dto::CaptureJob> loadJob(Uuid id);
    void runCrawlerForContext(CrawlContext &ctx);
    [[nodiscard]] crawler::AttemptSummary
    runCrawlerAttempt(CrawlContext &ctx, const String &seedUrl);
    [[nodiscard]] std::optional<us::utils::datetime::TimePointTz>
    persistMetadataForContext(const CrawlContext &ctx);
    void purgePrefix(const String &prefixKey);
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
          crawlerJobOverheadTimeoutSec(cfg["crawler_job_overhead_timeout_sec"].As<int64_t>()),
          linkCooldownSec(cfg["link_cooldown_sec"].As<int64_t>()),
          crawlJobRetentionSec(cfg["crawl_job_retention_sec"].As<int64_t>()),
          crawlJobCleanupIntervalSec(cfg["crawl_job_cleanup_interval_sec"].As<int64_t>()),
          s3UseSts(cfg["s3_use_sts"].As<bool>()),
          s3CredentialsEndpoint(
              String::fromBytesThrow(cfg["s3_credentials_endpoint"].As<std::string>())
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
              httpClient, processStarter, crawlerRunTimeoutSec, std::string(svcCfg.stateDir())
          ),
          denylist(ctx.FindComponent<Denylist>()),
          mainTaskProcessor(ctx.GetTaskProcessor("main-task-processor")),
          crawlSlots(engine::GetWorkerCount(mainTaskProcessor)),
          purgeTaskProcessor(ctx.GetTaskProcessor("purge_task_processor")),
          credsRefreshTaskProcessor(ctx.GetTaskProcessor("creds_refresh_task_processor")),
          s3RefreshTask(), crawlJobCleanupTask(), purgeBackground(purgeTaskProcessor),
          crawlBackground(mainTaskProcessor)
    {
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
    template <typename... Ts> [[nodiscard]] auto readonly(Ts &&...args)
    {
        return cluster->Execute(pg::ClusterHostType::kSlaveOrMaster, std::forward<Ts>(args)...);
    }

    template <typename... Ts> [[nodiscard]] auto readwrite(Ts &&...args)
    {
        return cluster->Execute(pg::ClusterHostType::kMaster, std::forward<Ts>(args)...);
    }

    template <typename... Ts> [[nodiscard]] auto sharedReadonly(Ts &&...args)
    {
        return sharedCluster->Execute(
            pg::ClusterHostType::kSlaveOrMaster, std::forward<Ts>(args)...
        );
    }

    template <typename... Ts> [[nodiscard]] auto sharedReadwrite(Ts &&...args)
    {
        return sharedCluster->Execute(pg::ClusterHostType::kMaster, std::forward<Ts>(args)...);
    }
};

/** Lightweight context shared across steps of a single crawl job. */
struct [[nodiscard]] CrawlContext {
    Link link;
    Uuid id;
    String keyOnly;
    String s3Key;
    String location;

    CrawlContext(Uuid idIn, Link linkIn, const Config &cfg)
        : link(std::move(linkIn)), id(idIn),
          keyOnly(String::fromBytesThrow(us::utils::ToString(id))),
          s3Key(text::format("{}/{}", cfg.s3Bucket(), keyOnly)),
          location(text::format("{}/{}", cfg.publicBaseUrl(), keyOnly))
    {
    }
};

[[nodiscard]] dto::UuidWithTimeLink Crud::Impl::runCrawlJob(Uuid id, Link link)
{
    const auto totalCrawlTimeLimitSec = crawlerJobOverheadTimeoutSec +
                                        crawlerRunTimeoutSec * kCrawlerSeedAttemptsMax;
    engine::current_task::SetDeadline(
        engine::Deadline::FromDuration(chrono::seconds{totalCrawlTimeLimitSec})
    );

    std::shared_lock<engine::CancellableSemaphore> slotLock(crawlSlots);

    CrawlContext ctx(id, std::move(link), svcCfg);

    LOG_INFO() << fmt::format(
        "runCrawlJob starting crawler for job {} ({})", us::utils::ToString(id),
        ctx.link.normalized()
    );
    runCrawlerForContext(ctx);
    LOG_INFO() << fmt::format(
        "runCrawlJob finished crawler for job {} ({})", us::utils::ToString(id),
        ctx.link.normalized()
    );

    LOG_INFO() << fmt::format(
        "Persisting metadata for job {} ({})", us::utils::ToString(id), ctx.link.normalized()
    );
    auto createdAt = persistMetadataForContext(ctx);
    if (!createdAt)
        throw errors::CrawlerFailedException("failed to persist metadata");
    LOG_INFO() << fmt::format(
        "Persisted metadata for job {} ({})", us::utils::ToString(id), ctx.link.normalized()
    );
    return {ctx.id, createdAt.value(), std::string(ctx.link.normalized().view())};
}

us::utils::datetime::TimePointTz Crud::Impl::insertJob(Uuid id, String link)
{
    struct Row {
        pg::TimePointTz createdAt;
    };
    auto row = sharedReadwrite(sql::kInsertCrawlJob, id, link).AsSingleRow<Row>(pg::kRowTag);
    return us::utils::datetime::TimePointTz(static_cast<system_clock::time_point>(row.createdAt));
}

void Crud::Impl::markJobRunning(Uuid id)
{
    static_cast<void>(sharedReadwrite(sql::kUpdateCrawlJobRunning, id));
}

void Crud::Impl::markJobSucceeded(Uuid id, const us::utils::datetime::TimePointTz &createdAt)
{
    static_cast<void>(sharedReadwrite(
        sql::kUpdateCrawlJobSucceeded, id, pg::TimePointTz(createdAt.GetTimePoint())
    ));
}

void Crud::Impl::markJobFailed(Uuid id, const String &errorCategory, const String &errorMessage)
{
    static_cast<void>(sharedReadwrite(sql::kUpdateCrawlJobFailed, id, errorCategory, errorMessage));
}

std::optional<dto::CaptureJob> Crud::Impl::loadJob(Uuid id)
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
    auto rowOpt = sharedReadonly(sql::kSelectCrawlJob, id).AsOptionalSingleRow<Row>(pg::kRowTag);
    if (!rowOpt)
        return {};

    dto::CaptureJob job;
    job.uuid = rowOpt->uuid;
    job.link = std::string(rowOpt->link.view());
    if (rowOpt->status == "pending")
        job.status = dto::CaptureJob::Status::kPending;
    else if (rowOpt->status == "running")
        job.status = dto::CaptureJob::Status::kRunning;
    else if (rowOpt->status == "succeeded")
        job.status = dto::CaptureJob::Status::kSucceeded;
    else
        job.status = dto::CaptureJob::Status::kFailed;
    job.created_at = us::utils::datetime::TimePointTz(
        static_cast<system_clock::time_point>(rowOpt->createdAt)
    );
    if (rowOpt->startedAt)
        job.started_at = us::utils::datetime::TimePointTz(
            static_cast<system_clock::time_point>(rowOpt->startedAt.value())
        );
    if (rowOpt->finishedAt)
        job.finished_at = us::utils::datetime::TimePointTz(
            static_cast<system_clock::time_point>(rowOpt->finishedAt.value())
        );
    if (rowOpt->resultCreatedAt)
        job.result_created_at = us::utils::datetime::TimePointTz(
            static_cast<system_clock::time_point>(rowOpt->resultCreatedAt.value())
        );
    if (job.status == dto::CaptureJob::Status::kFailed && rowOpt->errorMessage) {
        dto::ErrorEnvelope::Error err{rowOpt->errorMessage.value()};
        job.error = dto::ErrorEnvelope{err};
    }
    if (job.status == dto::CaptureJob::Status::kSucceeded && job.result_created_at)
        job.result = dto::UuidWithTimeLink(job.uuid, job.result_created_at.value(), job.link);
    return job;
}

Crud::Impl::S3ClientState Crud::Impl::fetchS3ClientStateFromSts() const
{
    const auto sessionUuid = String::fromBytesThrow(
        us::utils::ToString(us::utils::generators::GenerateBoostUuid())
    );
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

    S3ClientState state;
    state.creds = s3v4::S3Credentials(sts.accessKeyId, sts.secretAccessKey, sts.sessionToken);
    state.expiresAt = sts.expiresAt;
    state.client = std::make_shared<s3v4::S3V4Client>(
        httpClient,
        s3v4::S3V4Config(svcCfg.s3Endpoint(), svcCfg.s3Region(), svcCfg.s3Timeout(), false),
        state.creds, String()
    );
    return state;
}

std::optional<dto::CaptureJob> Crud::Impl::findLatestJobForLink(const String &link)
{
    auto idOpt = sharedReadonly(sql::kSelectLatestCrawlJobByLink, link).AsOptionalSingleRow<Uuid>();
    if (!idOpt)
        return {};
    return loadJob(idOpt.value());
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
        try {
            refreshS3CredentialsTask();
        } catch (const std::exception &e) {
            LOG_ERROR() << fmt::format("S3 credentials refresh task terminated: {}", e.what());
        }
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
        } catch (const std::exception &e) {
            LOG_ERROR() << fmt::format("Failed to refresh S3 credentials from STS: {}", e.what());
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

    crawlJobCleanupTask.Start("crawl_job_cleanup", settings, [this]() {
        try {
            cleanupOldJobs();
        } catch (const std::exception &e) {
            LOG_ERROR() << fmt::format("Crawl job cleanup task failed: {}", e.what());
        }
    });
}

void Crud::Impl::cleanupOldJobs()
{
    const auto now = us::utils::datetime::Now();
    const auto cutoff = now - chrono::seconds{crawlJobRetentionSec};
    try {
        static_cast<void>(sharedReadwrite(sql::kDeleteCrawlJobsExpired, pg::TimePointTz(cutoff)));
    } catch (const std::exception &e) {
        LOG_ERROR() << fmt::format("Failed to delete old crawl jobs: {}", e.what());
        throw;
    }
}

crawler::AttemptSummary Crud::Impl::runCrawlerAttempt(CrawlContext &ctx, const String &seedUrl)
{
    LOG_INFO() << fmt::format(
        "Submitting crawl for {} to embedded crawler with timeout={}s", seedUrl,
        crawlerRunTimeoutSec
    );

    const auto run = crawlerRunner.run(seedUrl);
    LOG_INFO() << fmt::format(
        "Embedded crawler returned for {} (exit_code={}, wacz_exists={})", seedUrl,
        run.attempt.exitCode, run.attempt.waczExists ? "true" : "false"
    );
    if (!run.attempt.waczExists) {
        const auto attemptContext = crawler::formatAttemptContext(run.attempt);
        LOG_INFO() << fmt::format(
            "Crawler attempt failed for {}{}", seedUrl,
            attemptContext.empty() ? std::string() : fmt::format(" ({})", attemptContext)
        );
        return run.attempt;
    }

    try {
        LOG_INFO() << fmt::format("Uploading WACZ for {} to {}", seedUrl, ctx.s3Key);
        UINVARIANT(run.wacz, "embedded crawler reported missing WACZ payload");
        auto snapshot = s3State.Read();
        snapshot->client->PutObject(
            ctx.s3Key.view(), run.wacz.value(), {}, "application/zip", {}, {}
        );
        LOG_INFO() << fmt::format("Uploaded WACZ for {} to {}", seedUrl, ctx.s3Key);
    } catch (const std::exception &e) {
        const auto msg = fmt::format("S3 upload failed for {}: {}", ctx.s3Key, e.what());
        LOG_ERROR() << msg;
        throw;
    }

    return run.attempt;
}

void Crud::Impl::runCrawlerForContext(CrawlContext &ctx)
{
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
        return;
    }

    if (result.httpsAttempt.seedProbe && result.httpAttempt) {
        LOG_INFO() << fmt::format(
            "HTTPS seed probe before HTTP fallback: status={}, loadState={}",
            result.httpsAttempt.seedProbe->status.value_or(0),
            result.httpsAttempt.seedProbe->loadState.value_or(-1)
        );
    }

    if (result.outcome == crawler::RunOutcome::kFailedSizeLimit) {
        const auto attempt = result.httpAttempt ? result.httpAttempt.value() : result.httpsAttempt;
        const auto msg = fmt::format(
            "Failed to crawl {} ({})", result.httpAttempt ? httpSeedUrl : httpsSeedUrl,
            crawler::formatAttemptStatus(result.httpAttempt ? "http" : "https", attempt)
        );
        LOG_INFO() << msg;
        throw errors::CrawlerSizeLimitException(msg);
    }

    if (result.outcome == crawler::RunOutcome::kFailedChildNoExit) {
        const auto attempt = result.httpAttempt ? result.httpAttempt.value() : result.httpsAttempt;
        const auto attemptContext = crawler::formatAttemptContext(attempt);
        const auto msg = fmt::format(
            "Failed to crawl {}, child process did not exit cleanly{}",
            result.httpAttempt ? httpSeedUrl : httpsSeedUrl,
            attemptContext.empty() ? std::string() : fmt::format(" ({})", attemptContext)
        );
        LOG_INFO() << msg;
        throw errors::CrawlerFailedException(msg);
    }

    if (result.outcome == crawler::RunOutcome::kFailedNoWacz) {
        const auto attempt = result.httpAttempt ? result.httpAttempt.value() : result.httpsAttempt;
        const auto attemptContext = crawler::formatAttemptContext(attempt);
        const auto msg = fmt::format(
            "Failed to crawl {}, no WACZ{}", result.httpAttempt ? httpSeedUrl : httpsSeedUrl,
            attemptContext.empty() ? std::string() : fmt::format(" ({})", attemptContext)
        );
        LOG_INFO() << msg;
        throw errors::CrawlerFailedException(msg);
    }

    const auto msg = fmt::format(
        "Failed to crawl {} ({})", ctx.link.normalized(),
        result.httpAttempt
            ? fmt::format(
                  "{}, {}", crawler::formatAttemptStatus("https", result.httpsAttempt),
                  crawler::formatAttemptStatus("http", result.httpAttempt.value())
              )
            : std::string(crawler::formatAttemptStatus("https", result.httpsAttempt).view())
    );
    LOG_INFO() << msg;
    throw errors::CrawlerFailedException(msg);
}

[[nodiscard]] std::optional<us::utils::datetime::TimePointTz>
Crud::Impl::persistMetadataForContext(const CrawlContext &ctx)
{
    const auto prefixKey = prefix::makePrefixKey(ctx.link);
    const auto prefixTree = prefix::makePrefixTree(prefixKey);
    const auto host = ctx.link.url.hostname();

    if (!denylist.isAllowedPrefix(prefixKey)) {
        try {
            auto snapshot = s3State.Read();
            snapshot->client->DeleteObject(ctx.s3Key.view());
        } catch (const std::exception &) {
            LOG_ERROR() << fmt::format("error deleting {}", ctx.s3Key);
        }
        LOG_INFO() << fmt::format("Host became denylisted during crawl: {}", host);
        return {};
    }

    try {
        struct Row {
            Uuid id;
            pg::TimePointTz createdAt;
        };
        auto row = readwrite(
                       sql::kInsertCapture, ctx.id, ctx.link.normalized(), prefixKey, prefixTree,
                       ctx.location
        )
                       .AsSingleRow<Row>(pg::kRowTag);
        return us::utils::datetime::TimePointTz(
            static_cast<system_clock::time_point>(row.createdAt)
        );
    } catch (const std::exception &e) {
        try {
            auto snapshot = s3State.Read();
            snapshot->client->DeleteObject(ctx.s3Key.view());
        } catch (const std::exception &) {
            LOG_ERROR() << fmt::format("error deleting {}", ctx.s3Key);
        }
        LOG_ERROR() << fmt::format(
            "DB insert failed for {}: {}", us::utils::ToString(ctx.id), e.what()
        );
        return {};
    }
}

void Crud::Impl::purgePrefix(const String &prefixKey)
{
    const auto tree = prefix::makePrefixTree(prefixKey);
    while (true) {
        try {
            auto res = readonly(sql::kSelectIdsByDenyPrefixPaged, tree, raw(purgeDeleteBatchSize));
            std::vector<Uuid> ids;
            ids.reserve(res.Size());
            for (auto row : res)
                ids.emplace_back(row[0].As<Uuid>());
            if (ids.empty())
                break;
            for (auto &&id : ids) {
                const auto key = fmt::format("{}/{}", svcCfg.s3Bucket(), us::utils::ToString(id));
                try {
                    auto snapshot = s3State.Read();
                    snapshot->client->DeleteObject(key);
                } catch (const std::exception &e) {
                    LOG_ERROR() << fmt::format("S3 delete failed for key {}: {}", key, e.what());
                }
            }
            static_cast<void>(readwrite(sql::kDeleteCapturesByIds, ids));
        } catch (const std::exception &e) {
            LOG_ERROR() << fmt::format("denylist purge failed for {}: {}", prefixKey, e.what());
            throw;
        }
    }
}

dto::UuidWithTimeLink Crud::createCapture(Link link)
{
    auto *implPtr = impl.get();
    auto id = us::utils::generators::GenerateBoostUuid();
    return us::utils::Async(
               "create_capture",
               [implPtr, id, link = std::move(link)]() { return implPtr->runCrawlJob(id, link); }
    ).Get();
}

dto::CaptureJob Crud::createCaptureJob(Link link)
{
    auto *implPtr = impl.get();
    const auto normalizedLink = link.normalized();

    if (implPtr->linkCooldownSec > 0_i64) {
        auto latestJob = implPtr->findLatestJobForLink(normalizedLink);
        if (latestJob) {
            const auto now = us::utils::datetime::Now();
            const auto lastCreated = latestJob->created_at.GetTimePoint();
            const auto deadline = lastCreated + chrono::seconds{implPtr->linkCooldownSec};
            if (now < deadline)
                return latestJob.value();
        }
    }

    auto id = us::utils::generators::GenerateBoostUuid();
    auto createdAt = implPtr->insertJob(id, normalizedLink);
    implPtr->crawlBackground.AsyncDetach("crawl_job", [implPtr, id, link = std::move(link)]() {
        try {
            implPtr->markJobRunning(id);
            auto result = implPtr->runCrawlJob(id, link);
            implPtr->markJobSucceeded(id, result.created_at);
        } catch (const errors::CrawlerSizeLimitException &e) {
            implPtr->markJobFailed(id, "size_limit"_t, "capture exceeded archive size limit"_t);
        } catch (const errors::CrawlerFailedException &e) {
            implPtr->markJobFailed(
                id, "crawler_failed"_t, String::fromBytesThrow(std::string_view(e.what()))
            );
        } catch (const std::exception &e) {
            LOG_ERROR() << fmt::format(
                "Unexpected crawl job failure for {}: {}", us::utils::ToString(id), e.what()
            );
            implPtr->markJobFailed(id, "internal_server_error"_t, "internal server error"_t);
        }
    });

    dto::CaptureJob job;
    job.uuid = id;
    job.link = std::string(normalizedLink.view());
    job.status = dto::CaptureJob::Status::kPending;
    job.created_at = createdAt;
    job.started_at = {};
    job.finished_at = {};
    job.result_created_at = {};
    job.result = {};
    job.error = {};
    return job;
}

std::optional<Link> Crud::findCapture(Uuid uuid)
{
    const auto location =
        impl->readonly(sql::kSelectCapture, uuid).AsOptionalSingleRow<std::string>();
    if (!location) {
        LOG_INFO() << fmt::format("UUID not found: {}", us::utils::ToString(uuid));
        return {};
    }
    return {
        Link::fromText(String::fromBytesThrow(location.value()), impl->svcCfg.queryPartLengthMax())
    };
}

std::optional<dto::CaptureJob> Crud::findCaptureJob(Uuid uuid) { return impl->loadJob(uuid); }

dto::PagedFindCapturesByUrlResponse Crud::findCapturesByLinkPage(const Link &link, String pageToken)
{
    namespace crud = v1::crud;

    struct Row {
        Uuid uuid;
        pg::TimePointTz timepoint;
    };
    std::vector<Row> dbRows;
    if (pageToken.empty()) {
        dbRows = impl->readonly(
                         sql::kSelectCaptureByLinkFirst, link.normalized(), raw(impl->pageMax)
        )
                     .AsContainer<std::vector<Row>>(pg::kRowTag);
    } else {
        auto cur = crud::decodeCursor(pageToken);
        if (!cur)
            throw errors::InvalidPageTokenException("invalid page_token");
        dbRows = impl->readonly(
                         sql::kSelectCaptureByLinkNext, link.normalized(), raw(impl->pageMax),
                         pg::TimePointTz(cur->createdAt), cur->id
        )
                     .AsContainer<std::vector<Row>>(pg::kRowTag);
    }
    std::vector<dto::UuidWithTime> items;
    items.reserve(dbRows.size());
    for (const auto &row : dbRows) {
        items.emplace_back(
            row.uuid,
            us::utils::datetime::TimePointTz(static_cast<system_clock::time_point>(row.timepoint))
        );
    }
    if (safeSize(items) == impl->pageMax && !items.empty()) {
        const auto &last = items.back();
        auto tp = last.created_at.GetTimePoint();
        crud::Cursor cursor(tp, last.uuid);
        return {items, std::string(crud::encodeCursor(cursor).view())};
    }
    return {items, {}};
}

dto::PagedFindCapturesByPrefixResponse
Crud::findCapturesByPrefixPage(String normalizedPrefix, String pageToken)
{
    namespace crud = v1::crud;

    std::optional<crud::PrefixCursor> cur;
    if (!pageToken.empty()) {
        cur = crud::decodePrefixCursor(pageToken);
        if (!cur || cur->prefix != normalizedPrefix)
            throw errors::InvalidPageTokenException("invalid page_token");
    }
    const std::string upper = crud::upperExclusiveBound(normalizedPrefix);
    const auto linksPerPage = impl->linksPerPageMax;

    auto selectLinksFirst = [&](i64 limit) {
        return impl
            ->readonly(sql::kSelectDistinctLinksByPrefixFirst, normalizedPrefix, upper, raw(limit))
            .AsContainer<std::vector<String>>();
    };
    auto selectLinksNext = [&](String fromLink, i64 limit) {
        return impl
            ->readonly(
                sql::kSelectDistinctLinksByPrefixNext, normalizedPrefix, upper, fromLink, raw(limit)
            )
            .AsContainer<std::vector<String>>();
    };

    std::vector<String> links;
    links.reserve(numericCast<size_t>(linksPerPage));
    if (cur) {
        const auto &cursorLink = cur->link;
        if (cur->createdAt) {
            links.push_back(cursorLink);
            if (linksPerPage > 1_i64) {
                auto more = selectLinksNext(cursorLink, linksPerPage - 1_i64);
                links.insert(std::end(links), std::begin(more), std::end(more));
            }
        } else {
            auto more = selectLinksNext(cursorLink, linksPerPage);
            links.insert(std::end(links), std::begin(more), std::end(more));
        }
    } else {
        auto first = selectLinksFirst(linksPerPage);
        links.insert(std::end(links), std::begin(first), std::end(first));
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
            return impl
                ->readonly(
                    sql::kSelectCaptureByLinkNext, link, raw(impl->perLinkMax),
                    pg::TimePointTz(cur->createdAt.value()), cur->id.value()
                )
                .AsContainer<std::vector<Row>>(pg::kRowTag);
        }
        return impl->readonly(sql::kSelectCaptureByLinkFirst, link, raw(impl->perLinkMax))
            .AsContainer<std::vector<Row>>(pg::kRowTag);
    };

    const auto linkCount = safeSize(links);
    for (i64 idx = 0; idx < linkCount; idx++) {
        const auto &link = links[numericCast<size_t>(idx)];
        auto rows = selectRowsForLink(link, idx);
        for (auto &&r : rows) {
            items.emplace_back(
                r.uuid,
                us::utils::datetime::TimePointTz(static_cast<system_clock::time_point>(r.tp)),
                std::string(link.view())
            );
        }
        if (!rows.empty()) {
            lastRow = rows.back();
            lastLink = link;
            if (safeSize(rows) == impl->perLinkMax && idx + 1_i64 == linkCount) {
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
    return {items, next};
}

void Crud::disallowAndPurgePrefix(String prefixKey)
{
    impl->denylist.insertPrefix(prefixKey, "disallow_and_purge"_t);

    LOG_INFO() << fmt::format("enqueued for prefix {}", prefixKey);

    impl->purgeBackground.AsyncDetach("purge_prefix_lambda", [implPtr = impl.get(), prefixKey]() {
        try {
            engine::current_task::SetDeadline(
                engine::Deadline::FromDuration(chrono::seconds{implPtr->purgeJobTimeoutSec})
            );
            LOG_INFO() << fmt::format("Starting purge for denylisted prefix: {}", prefixKey);
            implPtr->purgePrefix(prefixKey);
        } catch (const std::exception &e) {
            LOG_CRITICAL() << fmt::format("Purge task failed for {}: {}", prefixKey, e.what());
            us::utils::AbortWithStacktrace("Purge task failed");
        }
    });
}
