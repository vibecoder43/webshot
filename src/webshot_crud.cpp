#include "webshot_crud.hpp"
/**
 * @file
 * @brief Implementation of storage and crawl orchestration.
 *
 * Implements the `WebshotCrud` component, including background crawl startup,
 * metadata writes, and various paged queries.
 */
#include "container_guard.hpp"
#include "link.hpp"
#include "s3/s3_sts_client.hpp"
#include "s3/s3_v4_client.hpp"
#include "s3_refresh_utils.hpp"
#include "s3_secdist.hpp"
#include "schemas/webshot.hpp"
#include "server_errors.hpp"
#include "text.hpp"
#include "text_postgres_formatter.hpp"
#include "utils.hpp"
#include "webshot_config.hpp"
#include "webshot_denylist.hpp"
#include "webshot_pagination.hpp"
#include "webshot_prefix_pagination.hpp"
#include "webshot_prefix_utils.hpp"

#include <webshot/sql_queries.hpp>

#include <chrono>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <boost/uuid/uuid.hpp>

#include <fmt/format.h>

#include <userver/clients/http/component.hpp>
#include <userver/components/component.hpp>
#include <userver/components/component_base.hpp>
#include <userver/concurrent/background_task_storage.hpp>
#include <userver/crypto/base64.hpp>
#include <userver/engine/semaphore.hpp>
#include <userver/engine/sleep.hpp>
#include <userver/engine/subprocess/process_starter.hpp>
#include <userver/engine/task/current_task.hpp>
#include <userver/engine/task/task_processor_fwd.hpp>
#include <userver/formats/json.hpp>
#include <userver/fs/blocking/read.hpp>
#include <userver/fs/blocking/temp_directory.hpp>
#include <userver/fs/blocking/write.hpp>
#include <userver/fs/read.hpp>
#include <userver/fs/write.hpp>
#include <userver/http/common_headers.hpp>
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
namespace {
constexpr int kCrawlerExitSuccess = 0;
constexpr int kCrawlerExitSizeLimit = 14;
constexpr int kCrawlerExitTimeLimit = 15;
constexpr int kCrawlerExitDiskUtilization = 16;
} // namespace
using namespace v1;
using namespace text::literals;
using Uuid = boost::uuids::uuid;
using chrono::system_clock;

us::yaml_config::Schema WebshotCrud::GetStaticConfigSchema()
{
    return us::yaml_config::MergeSchemas<us::components::ComponentBase>(R"(
type: object
description: '.'
additionalProperties: false
properties:
    webshots-page-max:
        type: integer
        minimum: 1
        description: '.'
    webshots-per-link-max:
        type: integer
        minimum: 1
        description: 'Max captures per link in a prefix page'
    webshots-links-per-page-max:
        type: integer
        minimum: 1
        description: 'Max distinct links in a prefix page'
    crawler-network:
        type: string
        description: 'Name of the network to run crawlers on (scoped egress rules)'
    crawl-concurrency:
        type: integer
        minimum: 1
        description: 'Max concurrent crawls; blocks above this'
    crawler-image:
        type: string
        description: 'image used for the crawl container'
    crawler-workers:
        type: integer
        minimum: 1
        description: 'Number of crawler workers per job'
    crawler-page-load-timeout-sec:
        type: integer
        minimum: 1
        description: 'Page load timeout in seconds'
    crawler-post-load-delay-sec:
        type: integer
        minimum: 0
        description: 'Post-load delay in seconds'
    crawler-net-idle-wait-sec:
        type: integer
        minimum: 0
        description: 'Extra wait for network idleness in seconds'
    crawler-page-extra-delay-sec:
        type: integer
        minimum: 0
        description: 'Extra delay before snapshot in seconds'
    crawler-behavior-timeout-sec:
        type: integer
        minimum: 1
        description: 'Behavior script timeout in seconds'
    crawler-container-timeout-sec:
        type: integer
        minimum: 1
        description: 'Max lifetime of the crawler container (passed as Browsertrix --timeLimit) in seconds'
    crawler-overhead-timeout-sec:
        type: integer
        minimum: 1
        description: 'Overhead timeout added to crawler stage timeouts in seconds'
    crawler-lang:
        type: string
        description: 'Language hint passed to the crawler'
    crawler-scope-type:
        type: string
        description: 'Scope type passed to the crawler (e.g. page-spa)'
    s3-credentials-endpoint:
        type: string
        description: 'STS endpoint used to obtain temporary S3 credentials; S3 data endpoint s3-endpoint (in webshot_config) must be http(s)://host[:port] with optional trailing slash and no additional path or query'
    s3-use-sts:
        type: boolean
        description: 'Whether to fetch temporary S3 credentials from STS (true) or use static credentials from secdist (false)'
    s3-credentials-duration-sec:
        type: integer
        minimum: 1
        description: 'Requested lifetime of temporary S3 credentials in seconds'
    s3-credentials-refresh-margin-sec:
        type: integer
        minimum: 1
        description: 'How many seconds before expiration to refresh S3 credentials'
    s3-credentials-refresh-retry-sec:
        type: integer
        minimum: 1
        description: 'Delay between failed S3 credential refresh attempts in seconds'
    link-cooldown-sec:
        type: integer
        minimum: 0
        description: 'Per-link minimum interval between capture jobs in seconds; 0 disables cooldown'
    crawl-job-retention-sec:
        type: integer
        minimum: 1
        description: 'Retention window for crawl_job rows in seconds'
    crawl-job-cleanup-interval-sec:
        type: integer
        minimum: 1
        description: 'Interval between crawl_job cleanup passes in seconds'
    purge-job-timeout-sec:
        type: integer
        minimum: 1
        description: 'Upper bound for a single purge job in seconds'
    purge-delete-batch-size:
        type: integer
        minimum: 1
        description: 'Number of objects to delete per purge batch'
    crawler-size-limit-mib:
        type: integer
        minimum: 0
        description: 'Per-capture WARC size limit in MiB; 0 disables size limiting'
)");
}

WebshotCrud::WebshotCrud(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : us::components::ComponentBase(config, context),
      impl(std::make_unique<WebshotCrud::Impl>(config, context))
{
}

WebshotCrud::~WebshotCrud() = default;

struct [[nodiscard]] CrawlContext;

/** @brief Private pimpl that holds dependencies and query helpers. */
class [[nodiscard]] WebshotCrud::Impl {
public:
    const int64_t webshotsPageMax;
    const int64_t webshotsPerLinkMax;
    const int64_t webshotsLinksPerPageMax;
    const String crawlerNetwork;
    const int64_t crawlerWorkers;
    const String crawlerImage;
    const int64_t crawlerPageLoadTimeoutSec;
    const int64_t crawlerPostLoadDelaySec;
    const int64_t crawlerNetIdleWaitSec;
    const int64_t crawlerPageExtraDelaySec;
    const int64_t crawlerBehaviorTimeoutSec;
    const int64_t crawlerContainerTimeoutSec;
    const int64_t crawlerOverheadTimeoutSec;
    const String crawlerLang;
    const String crawlerScopeType;
    const int64_t crawlerSizeLimitMiB;
    const int64_t linkCooldownSec;
    const int64_t crawlJobRetentionSec;
    const int64_t crawlJobCleanupIntervalSec;
    const bool s3UseSts;
    const String s3CredentialsEndpoint;
    const int64_t s3CredentialsDurationSec;
    const int64_t s3CredentialsRefreshMarginSec;
    const int64_t s3CredentialsRefreshRetrySec;
    const int64_t purgeJobTimeoutSec;
    const int64_t purgeDeleteBatchSize;
    const WebshotConfig &svcCfg;
    pg::ClusterPtr cluster;
    pg::ClusterPtr sharedCluster;
    us::clients::http::Client &httpClient;
    struct [[nodiscard]] S3ClientState {
        s3v4::S3Credentials creds;
        system_clock::time_point expiresAt;
        std::shared_ptr<s3v4::S3V4Client> client;
    };
    rcu::Variable<S3ClientState> s3State;
    s3v4::AccessKeyId staticAccessKeyId;
    s3v4::SecretAccessKey staticSecretAccessKey;
    WebshotDenylist &denylist;
    engine::CancellableSemaphore crawlSlots;
    engine::TaskProcessor &purgeTaskProcessor;
    engine::TaskProcessor &credsRefreshTaskProcessor;
    // must die first
    us::utils::PeriodicTask s3RefreshTask;
    us::utils::PeriodicTask crawlJobCleanupTask;
    concurrent::BackgroundTaskStorage purgeBackground;
    concurrent::BackgroundTaskStorage crawlBackground;

    [[nodiscard]] dto::UuidWithTimeLink
    runCrawlJob(Uuid id, Link link, std::vector<String> pinnedIps);
    [[nodiscard]] us::utils::datetime::TimePointTz insertJob(Uuid id, String link);
    [[nodiscard]] std::optional<dto::WebshotJob> findLatestJobForLink(const String &link);
    void markJobRunning(Uuid id);
    void markJobSucceeded(Uuid id, const us::utils::datetime::TimePointTz &createdAt);
    void markJobFailed(Uuid id, const String &errorCategory, const String &errorMessage);
    [[nodiscard]] std::optional<dto::WebshotJob> loadJob(Uuid id);
    void runCrawlerForContext(CrawlContext &ctx, engine::subprocess::ProcessStarter &starter);
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
        : webshotsPageMax(cfg["webshots-page-max"].As<int64_t>()),
          webshotsPerLinkMax(cfg["webshots-per-link-max"].As<int64_t>()),
          webshotsLinksPerPageMax(cfg["webshots-links-per-page-max"].As<int64_t>()),
          crawlerNetwork(String::fromBytesThrow(cfg["crawler-network"].As<std::string>())),
          crawlerWorkers(cfg["crawler-workers"].As<int64_t>()),
          crawlerImage(String::fromBytesThrow(cfg["crawler-image"].As<std::string>())),
          crawlerPageLoadTimeoutSec(cfg["crawler-page-load-timeout-sec"].As<int64_t>()),
          crawlerPostLoadDelaySec(cfg["crawler-post-load-delay-sec"].As<int64_t>()),
          crawlerNetIdleWaitSec(cfg["crawler-net-idle-wait-sec"].As<int64_t>()),
          crawlerPageExtraDelaySec(cfg["crawler-page-extra-delay-sec"].As<int64_t>()),
          crawlerBehaviorTimeoutSec(cfg["crawler-behavior-timeout-sec"].As<int64_t>()),
          crawlerContainerTimeoutSec(cfg["crawler-container-timeout-sec"].As<int64_t>()),
          crawlerOverheadTimeoutSec(cfg["crawler-overhead-timeout-sec"].As<int64_t>()),
          crawlerLang(String::fromBytesThrow(cfg["crawler-lang"].As<std::string>())),
          crawlerScopeType(String::fromBytesThrow(cfg["crawler-scope-type"].As<std::string>())),
          crawlerSizeLimitMiB(cfg["crawler-size-limit-mib"].As<int64_t>()),
          linkCooldownSec(cfg["link-cooldown-sec"].As<int64_t>()),
          crawlJobRetentionSec(cfg["crawl-job-retention-sec"].As<int64_t>()),
          crawlJobCleanupIntervalSec(cfg["crawl-job-cleanup-interval-sec"].As<int64_t>()),
          s3UseSts(cfg["s3-use-sts"].As<bool>()),
          s3CredentialsEndpoint(
              String::fromBytesThrow(cfg["s3-credentials-endpoint"].As<std::string>())
          ),
          s3CredentialsDurationSec(cfg["s3-credentials-duration-sec"].As<int64_t>()),
          s3CredentialsRefreshMarginSec(cfg["s3-credentials-refresh-margin-sec"].As<int64_t>()),
          s3CredentialsRefreshRetrySec(cfg["s3-credentials-refresh-retry-sec"].As<int64_t>()),
          purgeJobTimeoutSec(cfg["purge-job-timeout-sec"].As<int64_t>()),
          purgeDeleteBatchSize(cfg["purge-delete-batch-size"].As<int64_t>()),
          svcCfg(ctx.FindComponent<WebshotConfig>()),
          cluster(ctx.FindComponent<us::components::Postgres>("capture-meta-db").GetCluster()),
          sharedCluster(
              ctx.FindComponent<us::components::Postgres>("shared-state-db").GetCluster()
          ),
          httpClient(ctx.FindComponent<us::components::HttpClient>().GetHttpClient()),
          denylist(ctx.FindComponent<WebshotDenylist>()),
          crawlSlots(cfg["crawl-concurrency"].As<size_t>()),
          purgeTaskProcessor(ctx.GetTaskProcessor("purge-task-processor")),
          credsRefreshTaskProcessor(ctx.GetTaskProcessor("creds-refresh-task-processor")),
          s3RefreshTask(), crawlJobCleanupTask(), purgeBackground(purgeTaskProcessor),
          crawlBackground(ctx.GetTaskProcessor("main-task-processor"))
    {
        const auto crawlerStageTotalTimeoutSec = crawlerPageLoadTimeoutSec +
                                                 crawlerPostLoadDelaySec + crawlerNetIdleWaitSec +
                                                 crawlerPageExtraDelaySec +
                                                 crawlerBehaviorTimeoutSec;
        UINVARIANT(
            crawlerStageTotalTimeoutSec <= crawlerContainerTimeoutSec,
            "crawler-container-timeout-sec must be >= sum of crawler stage timeouts"
        );
        UINVARIANT(
            s3CredentialsDurationSec > s3CredentialsRefreshMarginSec,
            "s3-credentials-duration-sec must be greater than s3-credentials-refresh-margin-sec"
        );
        UINVARIANT(crawlJobRetentionSec > 0, "crawl-job-retention-sec must be positive");
        UINVARIANT(
            crawlJobCleanupIntervalSec > 0, "crawl-job-cleanup-interval-sec must be positive"
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
    std::vector<String> pinnedIps;
    us::fs::blocking::TempDirectory archiveRoot;
    Uuid id;
    String keyOnly;
    String s3Key;
    String location;

    CrawlContext(Uuid idIn, Link linkIn, std::vector<String> pinnedIpsIn, const WebshotConfig &cfg)
        : link(std::move(linkIn)), pinnedIps(std::move(pinnedIpsIn)),
          archiveRoot(us::fs::blocking::TempDirectory::Create()), id(idIn),
          keyOnly(String::fromBytesThrow(us::utils::ToString(id))),
          s3Key(String::fromBytesThrow(fmt::format("{}/{}", cfg.s3Bucket(), keyOnly))),
          location(String::fromBytesThrow(fmt::format("{}/{}", cfg.publicBaseUrl(), keyOnly)))
    {
    }
};

[[nodiscard]] dto::UuidWithTimeLink
WebshotCrud::Impl::runCrawlJob(Uuid id, Link link, std::vector<String> pinnedIps)
{
    UINVARIANT(!pinnedIps.empty(), "can't crawl with no IPs");

    const auto totalCrawlTimeLimitSec = crawlerOverheadTimeoutSec + crawlerContainerTimeoutSec;
    engine::current_task::SetDeadline(
        engine::Deadline::FromDuration(chrono::seconds(totalCrawlTimeLimitSec))
    );

    std::shared_lock<engine::CancellableSemaphore> slotLock(crawlSlots);

    engine::subprocess::ProcessStarter starter(engine::current_task::GetBlockingTaskProcessor());

    CrawlContext ctx(id, std::move(link), std::move(pinnedIps), svcCfg);

    runCrawlerForContext(ctx, starter);

    auto createdAt = persistMetadataForContext(ctx);
    if (!createdAt)
        throw errors::CrawlerFailedException("failed to persist metadata");
    return {ctx.id, *createdAt, std::string(ctx.link.normalized().view())};
}

us::utils::datetime::TimePointTz WebshotCrud::Impl::insertJob(Uuid id, String link)
{
    struct Row {
        pg::TimePointTz createdAt;
    };
    auto row = sharedReadwrite(sql::kInsertCrawlJob, id, link).AsSingleRow<Row>(pg::kRowTag);
    return us::utils::datetime::TimePointTz(static_cast<system_clock::time_point>(row.createdAt));
}

void WebshotCrud::Impl::markJobRunning(Uuid id)
{
    static_cast<void>(sharedReadwrite(sql::kUpdateCrawlJobRunning, id));
}

void WebshotCrud::Impl::markJobSucceeded(Uuid id, const us::utils::datetime::TimePointTz &createdAt)
{
    static_cast<void>(sharedReadwrite(
        sql::kUpdateCrawlJobSucceeded, id, pg::TimePointTz(createdAt.GetTimePoint())
    ));
}

void WebshotCrud::Impl::markJobFailed(
    Uuid id, const String &errorCategory, const String &errorMessage
)
{
    static_cast<void>(sharedReadwrite(sql::kUpdateCrawlJobFailed, id, errorCategory, errorMessage));
}

std::optional<dto::WebshotJob> WebshotCrud::Impl::loadJob(Uuid id)
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

    dto::WebshotJob job;
    job.uuid = rowOpt->uuid;
    job.link = std::string(rowOpt->link.view());
    if (rowOpt->status == "pending")
        job.status = dto::WebshotJob::Status::kPending;
    else if (rowOpt->status == "running")
        job.status = dto::WebshotJob::Status::kRunning;
    else if (rowOpt->status == "succeeded")
        job.status = dto::WebshotJob::Status::kSucceeded;
    else
        job.status = dto::WebshotJob::Status::kFailed;
    job.created_at = us::utils::datetime::TimePointTz(
        static_cast<system_clock::time_point>(rowOpt->createdAt)
    );
    if (rowOpt->startedAt)
        job.started_at = us::utils::datetime::TimePointTz(
            static_cast<system_clock::time_point>(*rowOpt->startedAt)
        );
    if (rowOpt->finishedAt)
        job.finished_at = us::utils::datetime::TimePointTz(
            static_cast<system_clock::time_point>(*rowOpt->finishedAt)
        );
    if (rowOpt->resultCreatedAt)
        job.result_created_at = us::utils::datetime::TimePointTz(
            static_cast<system_clock::time_point>(*rowOpt->resultCreatedAt)
        );
    if (job.status == dto::WebshotJob::Status::kFailed && rowOpt->errorMessage) {
        dto::ErrorEnvelope::Error err{*rowOpt->errorMessage};
        job.error = dto::ErrorEnvelope{err};
    }
    if (job.status == dto::WebshotJob::Status::kSucceeded && job.result_created_at)
        job.result = dto::UuidWithTimeLink(job.uuid, *job.result_created_at, job.link);
    return job;
}

WebshotCrud::Impl::S3ClientState WebshotCrud::Impl::fetchS3ClientStateFromSts() const
{
    const auto sessionUuid = *String::fromBytes(
        us::utils::ToString(us::utils::generators::GenerateBoostUuid())
    );
    const auto sessionName = text::format("webshot-{}", sessionUuid);
    const auto kRoleArnDescription = "webshot-ephemeral-s3-credentials"_t;

    const auto policyJson = text::format(
        "{{\"Version\":\"2012-10-17\",\"Statement\":{{\"Sid\":\"webshot-access\",\"Effect\":"
        "\"Allow\",\"Principal\":\"*\",\"Action\":[\"s3:PutObject\",\"s3:DeleteObject\","
        "\"s3:GetObject\"],\"Resource\":\"arn:aws:s3:::{}/*\"}}}}",
        svcCfg.s3Bucket()
    );

    const auto sts = fetchStsCredentials(
        httpClient, s3CredentialsEndpoint, staticAccessKeyId, staticSecretAccessKey,
        svcCfg.s3Region(), kRoleArnDescription, sessionName, policyJson,
        chrono::seconds(s3CredentialsDurationSec), svcCfg.s3Timeout()
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

std::optional<dto::WebshotJob> WebshotCrud::Impl::findLatestJobForLink(const String &link)
{
    auto idOpt = sharedReadonly(sql::kSelectLatestCrawlJobByLink, link).AsOptionalSingleRow<Uuid>();
    if (!idOpt)
        return {};
    return loadJob(*idOpt);
}

void WebshotCrud::Impl::startS3RefreshTask()
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

    s3RefreshTask.Start("s3-credentials-refresh", settings, [this]() {
        try {
            refreshS3CredentialsTask();
        } catch (const std::exception &e) {
            LOG_ERROR() << fmt::format("S3 credentials refresh task terminated: {}", e.what());
        }
    });
}

void WebshotCrud::Impl::refreshS3CredentialsTask()
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
            engine::SleepFor(chrono::seconds(s3CredentialsRefreshRetrySec));
        }
    }
}

void WebshotCrud::Impl::startCrawlJobCleanupTask()
{
    auto interval = chrono::duration_cast<chrono::milliseconds>(
        chrono::seconds(crawlJobCleanupIntervalSec)
    );
    us::utils::PeriodicTask::Settings settings(interval, chrono::milliseconds(0));
    settings.task_processor = &purgeTaskProcessor;

    crawlJobCleanupTask.Start("crawl-job-cleanup", settings, [this]() {
        try {
            cleanupOldJobs();
        } catch (const std::exception &e) {
            LOG_ERROR() << fmt::format("Crawl job cleanup task failed: {}", e.what());
        }
    });
}

void WebshotCrud::Impl::cleanupOldJobs()
{
    const auto now = us::utils::datetime::Now();
    const auto cutoff = now - chrono::seconds(crawlJobRetentionSec);
    try {
        static_cast<void>(sharedReadwrite(sql::kDeleteCrawlJobsExpired, pg::TimePointTz(cutoff)));
    } catch (const std::exception &e) {
        LOG_ERROR() << fmt::format("Failed to delete old crawl jobs: {}", e.what());
        throw;
    }
}

void WebshotCrud::Impl::runCrawlerForContext(
    CrawlContext &ctx, engine::subprocess::ProcessStarter &starter
)
{
    const auto cname = text::format(
        "btcx-{}", us::utils::ToString(us::utils::generators::GenerateBoostUuid())
    );
    std::vector<String> createArgs = {
        "--hooks-dir=./containers"_t,
        "create"_t,
        "-e"_t,
        "CHROME_FLAGS=\"--dns-over-https-mode=off\""_t,
        "--shm-size"_t,
        "1g"_t,
        "-v"_t
    };
    createArgs.push_back(text::format("{}:/crawls", ctx.archiveRoot.GetPath()));
    createArgs.push_back("--network"_t);
    createArgs.push_back(crawlerNetwork);
    for (auto &&ip : ctx.pinnedIps) {
        createArgs.push_back("--add-host"_t);
        createArgs.push_back(text::format("{}:{}", ctx.link.host(), ip));
    }
    const auto collection = "1"_t;
    // Mark crawler containers for per-container firewalling via OCI hooks.
    createArgs.push_back("--annotation"_t);
    createArgs.push_back("webshot.crawler.netpol=true"_t);
    createArgs.push_back("--name"_t);
    createArgs.push_back(cname);
    createArgs.push_back(crawlerImage);
    createArgs.insert(
        createArgs.end(),
        {"crawl"_t,
         "--collection"_t,
         collection,
         "--generateWACZ"_t,
         "--workers"_t,
         text::format("{}", crawlerWorkers),
         "--headless"_t,
         "--scopeType"_t,
         crawlerScopeType,
         "--pageLimit"_t,
         "1"_t,
         "--pageLoadTimeout"_t,
         text::format("{}", crawlerPageLoadTimeoutSec),
         "--postLoadDelay"_t,
         text::format("{}", crawlerPostLoadDelaySec),
         "--netIdleWait"_t,
         text::format("{}", crawlerNetIdleWaitSec),
         "--pageExtraDelay"_t,
         text::format("{}", crawlerPageExtraDelaySec),
         "--behaviorTimeout"_t,
         text::format("{}", crawlerBehaviorTimeoutSec),
         "--timeLimit"_t,
         text::format("{}", crawlerContainerTimeoutSec),
         "--waitUntil"_t,
         "load"_t,
         "--blockAds"_t,
         "--behaviors"_t,
         "siteSpecific"_t,
         "--lang"_t,
         crawlerLang,
         "--context"_t,
         "general,worker,pageStatus,writer,storage,jsError,state,crawlStatus,fetch"_t,
         "wacz"_t,
         "--logLevel"_t,
         "debug,info"_t,
         "--logging"_t,
         "debug,stats,jserrors"_t,
         "--url"_t,
         ctx.link.httpUrl()}
    );
    if (crawlerSizeLimitMiB > 0) {
        const int64_t sizeLimitBytes = crawlerSizeLimitMiB * 1024 * 1024;
        createArgs.push_back("--sizeLimit"_t);
        createArgs.push_back(text::format("{}", sizeLimitBytes));
    }

    LOG_INFO() << text::format(
        "Starting crawl for {} with timeLimit={}s", ctx.link.httpUrl(), crawlerContainerTimeoutSec
    );
    ContainerGuard ctrGuard(starter, cname, createArgs);

    auto startProc = starter.Exec(
        "podman", std::vector<std::string>{"start", "-a", std::string(cname.view())},
        engine::subprocess::ExecOptions{.use_path = true}
    );
    auto status = startProc.Get();
    if (!status.IsExited()) {
        const auto msg = fmt::format(
            "Failed to crawl {}, child process did not exit cleanly", ctx.link.httpUrl()
        );
        LOG_INFO() << msg;
        throw errors::CrawlerFailedException(msg);
    }
    const auto code = status.GetExitCode();
    if (code != kCrawlerExitSuccess) {
        std::string reason = "crawler failed";
        switch (code) {
        case kCrawlerExitSizeLimit:
            reason = "crawler hit Browsertrix sizeLimit (max WARC size)";
            break;
        case kCrawlerExitTimeLimit:
            reason = "crawler hit Browsertrix timeLimit (max crawl duration)";
            break;
        case kCrawlerExitDiskUtilization:
            reason = "crawler hit Browsertrix diskUtilization limit";
            break;
        default:
            break;
        }
        const auto msg = fmt::format(
            "Failed to crawl {} (exit code {}: {})", ctx.link.httpUrl(), code, reason
        );
        LOG_INFO() << msg;
        if (code == kCrawlerExitSizeLimit)
            throw errors::CrawlerSizeLimitException(msg);
        throw errors::CrawlerFailedException(msg);
    }
    // do eagerly
    ctrGuard.remove();

    const auto pathToArchive = text::format(
        "{0}/collections/{1}/{1}.wacz", ctx.archiveRoot.GetPath(), collection
    );

    if (!us::fs::FileExists(
            engine::current_task::GetBlockingTaskProcessor(), std::string(pathToArchive.view())
        )) {
        const auto msg = fmt::format("Failed to crawl {}, no WACZ", ctx.link.httpUrl());
        LOG_INFO() << msg;
        throw errors::CrawlerFailedException(msg);
    }

    try {
        auto snapshot = s3State.Read();
        snapshot->client->PutObject(
            ctx.s3Key.view(), us::fs::blocking::ReadFileContents(std::string(pathToArchive.view())),
            {}, "application/zip", {}, {}
        );
    } catch (const std::exception &e) {
        const auto msg = fmt::format("S3 upload failed for {}: {}", ctx.s3Key, e.what());
        LOG_ERROR() << msg;
        throw;
    }
}

[[nodiscard]] std::optional<us::utils::datetime::TimePointTz>
WebshotCrud::Impl::persistMetadataForContext(const CrawlContext &ctx)
{
    const auto prefixKey = prefix::makePrefixKey(ctx.link);
    const auto host = ctx.link.host();

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
                       sql::kInsertWebshot, ctx.id, ctx.link.normalized(), prefixKey, ctx.location
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

void WebshotCrud::Impl::purgePrefix(const String &prefixKey)
{
    while (true) {
        try {
            auto res = readonly(
                sql::kSelectIdsByPrefixPaged, prefixKey, crud::upperExclusiveBound(prefixKey),
                purgeDeleteBatchSize
            );
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
            static_cast<void>(readwrite(sql::kDeleteWebshotsByIds, ids));
        } catch (const std::exception &e) {
            LOG_ERROR() << fmt::format("denylist purge failed for {}: {}", prefixKey, e.what());
            throw;
        }
    }
}

dto::UuidWithTimeLink WebshotCrud::createWebshot(Link link, std::vector<String> pinnedIps)
{
    auto *implPtr = impl.get();
    auto id = us::utils::generators::GenerateBoostUuid();
    return us::utils::Async(
               "create-webshot",
               [implPtr, id, link = std::move(link), pinned = std::move(pinnedIps)]() {
                   return implPtr->runCrawlJob(id, link, pinned);
               }
    ).Get();
}

dto::WebshotJob WebshotCrud::createWebshotJob(Link link, std::vector<String> pinnedIps)
{
    auto *implPtr = impl.get();
    const auto normalizedLink = link.normalized();

    if (implPtr->linkCooldownSec > 0) {
        auto latestJob = implPtr->findLatestJobForLink(normalizedLink);
        if (latestJob) {
            const auto now = us::utils::datetime::Now();
            const auto lastCreated = latestJob->created_at.GetTimePoint();
            const auto deadline = lastCreated + chrono::seconds(implPtr->linkCooldownSec);
            if (now < deadline)
                return *latestJob;
        }
    }

    auto id = us::utils::generators::GenerateBoostUuid();
    auto createdAt = implPtr->insertJob(id, normalizedLink);
    implPtr->crawlBackground.AsyncDetach(
        "crawl-job", [implPtr, id, link = std::move(link), pinned = std::move(pinnedIps)]() {
            try {
                implPtr->markJobRunning(id);
                auto result = implPtr->runCrawlJob(id, link, pinned);
                implPtr->markJobSucceeded(id, result.created_at);
            } catch (const errors::CrawlerSizeLimitException &e) {
                implPtr->markJobFailed(id, "size_limit"_t, "capture exceeded archive size limit"_t);
            } catch (const errors::CrawlerFailedException &e) {
                implPtr->markJobFailed(id, "crawler_failed"_t, "internal crawler error"_t);
            } catch (const std::exception &e) {
                implPtr->markJobFailed(id, "internal_server_error"_t, "internal server error"_t);
            }
        }
    );

    dto::WebshotJob job;
    job.uuid = id;
    job.link = std::string(normalizedLink.view());
    job.status = dto::WebshotJob::Status::kPending;
    job.created_at = createdAt;
    job.started_at = {};
    job.finished_at = {};
    job.result_created_at = {};
    job.result = {};
    job.error = {};
    return job;
}

std::optional<Link> WebshotCrud::findWebshot(Uuid uuid)
{
    const auto location =
        impl->readonly(sql::kSelectWebshot, uuid).AsOptionalSingleRow<std::string>();
    if (!location) {
        LOG_INFO() << fmt::format("UUID not found: {}", us::utils::ToString(uuid));
        return {};
    }
    return {Link::fromText(String::fromBytesThrow(*location), impl->svcCfg.queryPartLengthMax())};
}

std::optional<dto::WebshotJob> WebshotCrud::findCrawlJob(Uuid uuid) { return impl->loadJob(uuid); }

dto::PagedFindWebshotByUrlResponse
WebshotCrud::findWebshotByLinkPage(const Link &link, String pageToken)
{
    namespace crud = v1::crud;

    struct Row {
        Uuid uuid;
        pg::TimePointTz timepoint;
    };
    std::vector<Row> dbRows;
    if (pageToken.empty()) {
        dbRows = impl->readonly(
                         sql::kSelectWebshotByLinkFirst, link.normalized(), impl->webshotsPageMax
        )
                     .AsContainer<std::vector<Row>>(pg::kRowTag);
    } else {
        auto cur = crud::decodeCursor(pageToken);
        if (!cur)
            throw errors::InvalidPageTokenException("invalid page_token");
        dbRows = impl->readonly(
                         sql::kSelectWebshotByLinkNext, link.normalized(), impl->webshotsPageMax,
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
    if (v1::utils::ssize(items) == impl->webshotsPageMax && !items.empty()) {
        const auto &last = items.back();
        auto tp = last.created_at.GetTimePoint();
        crud::Cursor cursor(tp, last.uuid);
        return {items, std::string(crud::encodeCursor(cursor).view())};
    }
    return {items, {}};
}

dto::PagedFindWebshotByPrefixResponse
WebshotCrud::findWebshotsByPrefixPage(String normalizedPrefix, String pageToken)
{
    namespace crud = v1::crud;

    std::optional<crud::PrefixCursor> cur;
    if (!pageToken.empty()) {
        cur = crud::decodePrefixCursor(pageToken);
        if (!cur || cur->prefix != normalizedPrefix)
            throw errors::InvalidPageTokenException("invalid page_token");
    }
    const std::string upper = crud::upperExclusiveBound(normalizedPrefix);
    const auto linksPerPage = impl->webshotsLinksPerPageMax;

    auto selectLinksFirst = [&](int64_t limit) {
        return impl
            ->readonly(sql::kSelectDistinctLinksByPrefixFirst, normalizedPrefix, upper, limit)
            .AsContainer<std::vector<String>>();
    };
    auto selectLinksNext = [&](String fromLink, int64_t limit) {
        return impl
            ->readonly(
                sql::kSelectDistinctLinksByPrefixNext, normalizedPrefix, upper, fromLink, limit
            )
            .AsContainer<std::vector<String>>();
    };

    std::vector<String> links;
    links.reserve(static_cast<size_t>(linksPerPage));
    if (cur) {
        const auto &cursorLink = cur->link;
        if (cur->createdAt) {
            links.push_back(cursorLink);
            if (linksPerPage > 1) {
                auto more = selectLinksNext(cursorLink, linksPerPage - 1);
                links.insert(links.end(), more.begin(), more.end());
            }
        } else {
            auto more = selectLinksNext(cursorLink, linksPerPage);
            links.insert(links.end(), more.begin(), more.end());
        }
    } else {
        auto first = selectLinksFirst(linksPerPage);
        links.insert(links.end(), first.begin(), first.end());
    }

    struct Row {
        Uuid uuid;
        pg::TimePointTz tp;
    };
    std::vector<dto::UuidWithTimeLink> items;
    items.reserve(links.size() * static_cast<size_t>(impl->webshotsPerLinkMax));
    bool endedMidLink = false;
    String lastLink;
    std::optional<Row> lastRow;

    auto selectRowsForLink = [&](const String &link, size_t idx) {
        if (idx == 0 && cur && cur->createdAt && cur->id) {
            return impl
                ->readonly(
                    sql::kSelectWebshotByLinkNext, link, impl->webshotsPerLinkMax,
                    pg::TimePointTz(*cur->createdAt), *cur->id
                )
                .AsContainer<std::vector<Row>>(pg::kRowTag);
        }
        return impl->readonly(sql::kSelectWebshotByLinkFirst, link, impl->webshotsPerLinkMax)
            .AsContainer<std::vector<Row>>(pg::kRowTag);
    };

    for (size_t idx = 0; idx < links.size(); idx++) {
        const auto &link = links[idx];
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
            if (v1::utils::ssize(rows) == impl->webshotsPerLinkMax && idx == links.size() - 1) {
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

void WebshotCrud::disallowAndPurgePrefix(String prefixKey)
{
    impl->denylist.insertPrefix(prefixKey, "disallow-and-purge"_t);

    LOG_INFO() << fmt::format("enqueued for prefix {}", prefixKey);

    impl->purgeBackground.AsyncDetach("purge-prefix-lambda", [implPtr = impl.get(), prefixKey]() {
        try {
            engine::current_task::SetDeadline(
                engine::Deadline::FromDuration(chrono::seconds(implPtr->purgeJobTimeoutSec))
            );
            LOG_INFO() << fmt::format("Starting purge for denylisted prefix: {}", prefixKey);
            implPtr->purgePrefix(prefixKey);
        } catch (const std::exception &e) {
            LOG_CRITICAL() << fmt::format("Purge task failed for {}: {}", prefixKey, e.what());
            us::utils::AbortWithStacktrace("Purge task failed");
        }
    });
}
