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
#include "s3_secdist.hpp"
#include "schemas/webshot.hpp"
#include "server_errors.hpp"
#include "sql.hpp"
#include "utils.hpp"
#include "webshot_config.hpp"
#include "webshot_denylist.hpp"
#include "webshot_pagination.hpp"
#include "webshot_prefix_pagination.hpp"

#include <chrono>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
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
#include <userver/yaml_config/merge_schemas.hpp>
#include <userver/yaml_config/yaml_config.hpp>

namespace pg = us::storages::postgres;
namespace engine = us::engine;
namespace concurrent = userver::concurrent;
namespace rcu_ns = userver::rcu;
namespace chrono = std::chrono;
using namespace v1;
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
        description: 'Name of the Docker network to run crawlers on (scoped egress rules)'
    crawl-concurrency:
        type: integer
        minimum: 1
        description: 'Max concurrent crawls; blocks above this'
    crawler-image:
        type: string
        description: 'Docker image used for the crawl container'
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
    crawler-overhead-timeout-sec:
        type: integer
        minimum: 1
        description: 'Overhead timeout added to crawler stage timeouts in seconds'
    purge-job-timeout-sec:
        type: integer
        minimum: 1
        description: 'Upper bound for a single purge job in seconds'
    crawler-lang:
        type: string
        description: 'Language hint passed to the crawler'
    crawler-scope-type:
        type: string
        description: 'Scope type passed to the crawler (e.g. page-spa)'
    s3-credentials-endpoint:
        type: string
        description: 'STS endpoint used to obtain temporary S3 credentials'
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
    const std::string crawlerNetwork;
    const std::string crawlerImage;
    const int64_t crawlerWorkers;
    const int64_t crawlerPageLoadTimeoutSec;
    const int64_t crawlerPostLoadDelaySec;
    const int64_t crawlerNetIdleWaitSec;
    const int64_t crawlerPageExtraDelaySec;
    const int64_t crawlerBehaviorTimeoutSec;
    const int64_t crawlerOverheadTimeoutSec;
    const int64_t purgeJobTimeoutSec;
    const std::string crawlerLang;
    const std::string crawlerScopeType;
    const std::string s3CredentialsEndpoint;
    const int64_t s3CredentialsDurationSec;
    const int64_t s3CredentialsRefreshMarginSec;
    const int64_t s3CredentialsRefreshRetrySec;
    const WebshotConfig &svcCfg;
    pg::ClusterPtr cluster;
    us::clients::http::Client &httpClient;
    struct [[nodiscard]] S3ClientState {
        s3v4::S3Credentials creds;
        std::chrono::system_clock::time_point expiresAt;
        std::shared_ptr<s3v4::S3V4Client> client;
    };
    rcu_ns::Variable<S3ClientState> s3State;
    s3v4::AccessKeyId staticAccessKeyId;
    s3v4::SecretAccessKey staticSecretAccessKey;
    WebshotDenylist &denylist;
    engine::CancellableSemaphore crawlSlots;
    engine::TaskProcessor &crawlingTaskProcessor;
    // must die first
    concurrent::BackgroundTaskStorage backgroundTaskStorage;

    [[nodiscard]] dto::UuidWithTimeLink runCrawlJob(Link link, std::vector<std::string> pinnedIps);
    void runCrawlerForContext(CrawlContext &ctx, engine::subprocess::ProcessStarter &starter);
    [[nodiscard]] std::optional<us::utils::datetime::TimePointTz>
    persistMetadataForContext(const CrawlContext &ctx);
    void purgeHost(const std::string &host);
    [[nodiscard]] S3ClientState fetchS3ClientStateFromSts() const;
    void startS3RefreshTask();
    void runS3RefreshLoop();
    explicit Impl(
        const us::components::ComponentConfig &cfg, const us::components::ComponentContext &ctx
    )
        : webshotsPageMax(cfg["webshots-page-max"].As<int64_t>()),
          webshotsPerLinkMax(cfg["webshots-per-link-max"].As<int64_t>()),
          webshotsLinksPerPageMax(cfg["webshots-links-per-page-max"].As<int64_t>()),
          crawlerNetwork(cfg["crawler-network"].As<std::string>()),
          crawlerImage(cfg["crawler-image"].As<std::string>()),
          crawlerWorkers(cfg["crawler-workers"].As<int64_t>()),
          crawlerPageLoadTimeoutSec(cfg["crawler-page-load-timeout-sec"].As<int64_t>()),
          crawlerPostLoadDelaySec(cfg["crawler-post-load-delay-sec"].As<int64_t>()),
          crawlerNetIdleWaitSec(cfg["crawler-net-idle-wait-sec"].As<int64_t>()),
          crawlerPageExtraDelaySec(cfg["crawler-page-extra-delay-sec"].As<int64_t>()),
          crawlerBehaviorTimeoutSec(cfg["crawler-behavior-timeout-sec"].As<int64_t>()),
          crawlerOverheadTimeoutSec(cfg["crawler-overhead-timeout-sec"].As<int64_t>()),
          purgeJobTimeoutSec(cfg["purge-job-timeout-sec"].As<int64_t>()),
          crawlerLang(cfg["crawler-lang"].As<std::string>()),
          crawlerScopeType(cfg["crawler-scope-type"].As<std::string>()),
          s3CredentialsEndpoint(cfg["s3-credentials-endpoint"].As<std::string>()),
          s3CredentialsDurationSec(cfg["s3-credentials-duration-sec"].As<int64_t>()),
          s3CredentialsRefreshMarginSec(cfg["s3-credentials-refresh-margin-sec"].As<int64_t>()),
          s3CredentialsRefreshRetrySec(cfg["s3-credentials-refresh-retry-sec"].As<int64_t>()),
          svcCfg(ctx.FindComponent<WebshotConfig>()),
          cluster(ctx.FindComponent<us::components::Postgres>("webshot-meta-db").GetCluster()),
          httpClient(ctx.FindComponent<us::components::HttpClient>().GetHttpClient()),
          denylist(ctx.FindComponent<WebshotDenylist>()),
          crawlSlots(cfg["crawl-concurrency"].As<size_t>()),
          crawlingTaskProcessor(ctx.GetTaskProcessor("crawling-task-processor")),
          backgroundTaskStorage(crawlingTaskProcessor)
    {
        const auto &secdist = ctx.FindComponent<us::components::Secdist>().Get();
        const auto &creds = secdist.Get<S3CredentialsSecdist>();
        UINVARIANT(
            creds.access_key_id && creds.secret_access_key,
            "missing required S3 secdist credentials"
        );
        staticAccessKeyId = *creds.access_key_id;
        staticSecretAccessKey = *creds.secret_access_key;
        const auto initialState = fetchS3ClientStateFromSts();
        s3State.Assign(initialState);
        startS3RefreshTask();
    }
    template <typename... Ts> [[nodiscard]] auto readonly(Ts &&...args)
    {
        return cluster->Execute(pg::ClusterHostType::kSlaveOrMaster, std::forward<Ts>(args)...);
    }

    template <typename... Ts> [[nodiscard]] auto readwrite(Ts &&...args)
    {
        return cluster->Execute(pg::ClusterHostType::kMaster, std::forward<Ts>(args)...);
    }
};

/** Lightweight context shared across steps of a single crawl job. */
struct [[nodiscard]] CrawlContext {
    Link link;
    std::vector<std::string> pinnedIps;
    us::fs::blocking::TempDirectory archiveRoot;
    Uuid id;
    std::string keyOnly;
    std::string s3Key;
    std::string location;

    CrawlContext(Link linkIn, std::vector<std::string> pinnedIpsIn, const WebshotConfig &cfg)
        : link(std::move(linkIn)), pinnedIps(std::move(pinnedIpsIn)),
          archiveRoot(us::fs::blocking::TempDirectory::Create()),
          id(us::utils::generators::GenerateBoostUuid()), keyOnly(us::utils::ToString(id)),
          s3Key(fmt::format("{}/{}", cfg.s3Bucket(), keyOnly)),
          location(fmt::format("{}/{}", cfg.publicBaseUrl(), keyOnly))
    {
    }
};

[[nodiscard]] dto::UuidWithTimeLink
WebshotCrud::Impl::runCrawlJob(Link link, std::vector<std::string> pinnedIps)
{
    UINVARIANT(!pinnedIps.empty(), "can't crawl with no IPs");

    const auto totalSeconds = crawlerOverheadTimeoutSec + crawlerPageLoadTimeoutSec +
                              crawlerPostLoadDelaySec + crawlerNetIdleWaitSec +
                              crawlerPageExtraDelaySec + crawlerBehaviorTimeoutSec;
    engine::current_task::SetDeadline(
        engine::Deadline::FromDuration(std::chrono::seconds(totalSeconds))
    );

    std::shared_lock<engine::CancellableSemaphore> slotLock(crawlSlots);

    engine::subprocess::ProcessStarter starter(engine::current_task::GetBlockingTaskProcessor());

    CrawlContext ctx(std::move(link), std::move(pinnedIps), svcCfg);

    runCrawlerForContext(ctx, starter);

    auto createdAt = persistMetadataForContext(ctx);
    if (!createdAt)
        throw std::runtime_error("failed to persist metadata");
    return dto::UuidWithTimeLink{ctx.id, *createdAt, ctx.link.normalized()};
}

WebshotCrud::Impl::S3ClientState WebshotCrud::Impl::fetchS3ClientStateFromSts() const
{
    const auto stsLink = Link::fromUserInput(
        s3CredentialsEndpoint, static_cast<size_t>(s3CredentialsEndpoint.size())
    );
    std::string schemeRaw = std::string(stsLink.url.get_protocol());
    std::string scheme;
    if (schemeRaw.empty()) {
        scheme = "https";
    } else {
        if (!schemeRaw.empty() && schemeRaw.back() == ':')
            schemeRaw.pop_back();
        if (schemeRaw == "https") {
            scheme = "https";
        } else {
            UINVARIANT(false, "STS endpoint must use https scheme");
        }
    }
    std::string host = stsLink.host();
    std::string path = std::string(stsLink.url.get_pathname());
    if (path.empty())
        path = "/";

    const auto sessionUuid = us::utils::ToString(us::utils::generators::GenerateBoostUuid());
    const std::string sessionName = fmt::format("webshot-{}", sessionUuid);
    constexpr std::string_view kRoleArnDescription = "webshot-ephemeral-s3-credentials";

    std::string policyJson = fmt::format(
        "{{\"Version\":\"2012-10-17\",\"Statement\":{{\"Sid\":\"webshot-access\",\"Effect\":"
        "\"Allow\",\"Principal\":\"*\",\"Action\":[\"s3:PutObject\",\"s3:DeleteObject\","
        "\"s3:GetObject\"],\"Resource\":\"arn:aws:s3:::{}/*\"}}}}",
        svcCfg.s3Bucket()
    );

    const auto sts = FetchStsCredentials(
        httpClient, s3CredentialsEndpoint, staticAccessKeyId, staticSecretAccessKey,
        svcCfg.s3Region(), std::string{kRoleArnDescription}, sessionName, policyJson,
        std::chrono::seconds{s3CredentialsDurationSec}, svcCfg.s3Timeout()
    );

    S3ClientState state;
    state.creds = s3v4::S3Credentials{sts.accessKeyId, sts.secretAccessKey, sts.sessionToken};
    state.expiresAt = sts.expiresAt;
    state.client = std::make_shared<s3v4::S3V4Client>(
        httpClient,
        s3v4::S3V4Config{svcCfg.s3Endpoint(), svcCfg.s3Region(), svcCfg.s3Timeout(), false},
        state.creds, std::string{}
    );
    return state;
}

void WebshotCrud::Impl::startS3RefreshTask()
{
    backgroundTaskStorage.CriticalAsyncDetach("s3-credentials-refresh", [this]() {
        try {
            runS3RefreshLoop();
        } catch (const std::exception &e) {
            LOG_ERROR() << fmt::format("S3 credentials refresh loop terminated: {}", e.what());
        }
    });
}

void WebshotCrud::Impl::runS3RefreshLoop()
{
    while (!engine::current_task::ShouldCancel()) {
        auto snapshot = s3State.Read();
        const auto now = std::chrono::system_clock::now();
        auto refreshDelay = snapshot->expiresAt - now -
                            std::chrono::seconds{s3CredentialsRefreshMarginSec};
        if (refreshDelay < std::chrono::seconds{0})
            refreshDelay = std::chrono::seconds{0};

        engine::SleepFor(refreshDelay);
        if (engine::current_task::ShouldCancel())
            return;

        for (;;) {
            if (engine::current_task::ShouldCancel())
                return;
            try {
                const auto newState = fetchS3ClientStateFromSts();
                s3State.Assign(newState);
                break;
            } catch (const std::exception &e) {
                LOG_ERROR(
                ) << fmt::format("Failed to refresh S3 credentials from STS: {}", e.what());
                engine::SleepFor(std::chrono::seconds{s3CredentialsRefreshRetrySec});
            }
        }
    }
}

void WebshotCrud::Impl::runCrawlerForContext(
    CrawlContext &ctx, engine::subprocess::ProcessStarter &starter
)
{
    const auto cname = fmt::format(
        "btcx-{}", us::utils::ToString(us::utils::generators::GenerateBoostUuid())
    );

    std::vector<std::string> createArgs = {
        "create",
        "-v",
        fmt::format("{}:/crawls", ctx.archiveRoot.GetPath()),
        "-e",
        "CHROME_FLAGS=\"--dns-over-https-mode=off\"",
        "--shm-size",
        "1g",
    };

    createArgs.push_back("--network");
    createArgs.push_back(crawlerNetwork);
    for (const auto &ip : ctx.pinnedIps) {
        createArgs.push_back("--add-host");
        createArgs.push_back(fmt::format("{}:{}", ctx.link.host(), ip));
    }

    const std::string collection = "1";
    createArgs.push_back("--name");
    createArgs.push_back(cname);
    createArgs.push_back(crawlerImage);
    createArgs.insert(
        createArgs.end(),
        {"crawl",
         "--collection",
         collection,
         "--generateWACZ",
         "--workers",
         fmt::format("{}", crawlerWorkers),
         "--headless",
         "--scopeType",
         crawlerScopeType,
         "--pageLimit",
         "1",
         "--pageLoadTimeout",
         fmt::format("{}", crawlerPageLoadTimeoutSec),
         "--postLoadDelay",
         fmt::format("{}", crawlerPostLoadDelaySec),
         "--netIdleWait",
         fmt::format("{}", crawlerNetIdleWaitSec),
         "--pageExtraDelay",
         fmt::format("{}", crawlerPageExtraDelaySec),
         "--behaviorTimeout",
         fmt::format("{}", crawlerBehaviorTimeoutSec),
         "--waitUntil",
         "load",
         "--blockAds",
         "--behaviors",
         "siteSpecific",
         "--lang",
         crawlerLang,
         "--context",
         "general,worker,pageStatus,writer,storage,jsError,state,crawlStatus,fetch",
         "wacz",
         "--logLevel",
         "debug,info",
         "--logging",
         "debug,stats,jserrors",
         "--url",
         ctx.link.httpUrl()}
    );

    ContainerGuard ctrGuard(starter, cname, createArgs);

    auto startProc = starter.Exec(
        "docker", std::vector<std::string>{"start", "-a", cname},
        engine::subprocess::ExecOptions{.use_path = true}
    );
    auto status = startProc.Get();
    if (!status.IsExited() || status.GetExitCode() != 0) {
        const auto msg = fmt::format(
            "Failed to crawl {}, child process failed", ctx.link.httpUrl()
        );
        LOG_INFO() << msg;
        throw std::runtime_error(msg);
    }
    // do eagerly
    ctrGuard.remove();

    const auto pathToArchive = fmt::format(
        "{0}/collections/{1}/{1}.wacz", ctx.archiveRoot.GetPath(), collection
    );

    if (!us::fs::FileExists(engine::current_task::GetBlockingTaskProcessor(), pathToArchive)) {
        const auto msg = fmt::format("Failed to crawl {}, no WACZ", ctx.link.httpUrl());
        LOG_INFO() << msg;
        throw std::runtime_error(msg);
    }

    try {
        auto snapshot = s3State.Read();
        snapshot->client->PutObject(
            ctx.s3Key, us::fs::blocking::ReadFileContents(pathToArchive), std::nullopt,
            "application/zip", std::nullopt, std::nullopt
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
    const auto &host = ctx.link.host();
    std::string hostRev(rbegin(host), rend(host));

    if (!denylist.isAllowedHost(host)) {
        try {
            auto snapshot = s3State.Read();
            snapshot->client->DeleteObject(ctx.s3Key);
        } catch (const std::exception &) {
            LOG_ERROR() << fmt::format("error deleting {}", ctx.s3Key);
        }
        LOG_INFO() << fmt::format("Host became denylisted during crawl: {}", host);
        return {};
    }

    try {
        struct Row {
            Uuid id;
            pg::TimePointTz created_at;
        };
        auto row = readwrite(
                       sql::kInsertWebshot.data(), ctx.id, ctx.link.normalized(), hostRev,
                       ctx.location
        )
                       .AsSingleRow<Row>(pg::kRowTag);
        static_cast<void>(row.id);
        return us::utils::datetime::TimePointTz(static_cast<system_clock::time_point>(row.created_at
        ));
    } catch (const std::exception &e) {
        try {
            auto snapshot = s3State.Read();
            snapshot->client->DeleteObject(ctx.s3Key);
        } catch (const std::exception &) {
            LOG_ERROR() << fmt::format("error deleting {}", ctx.s3Key);
        }
        LOG_ERROR(
        ) << fmt::format("DB insert failed for {}: {}", us::utils::ToString(ctx.id), e.what());
        return {};
    }
}

void WebshotCrud::Impl::purgeHost(const std::string &host)
{
    std::string d_rev(rbegin(host), rend(host));
    const int64_t kBatch = 1000;
    while (true) {
        try {
            auto res = readonly(sql::kSelectIdsByHostOrSubhostsPaged.data(), d_rev, kBatch);
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
            static_cast<void>(readwrite(sql::kDeleteWebshotsByIds.data(), ids));
        } catch (const std::exception &e) {
            LOG_ERROR() << fmt::format("denylist purge failed for {}: {}", host, e.what());
            break;
        }
    }
}

dto::UuidWithTimeLink WebshotCrud::createWebshot(Link link, std::vector<std::string> pinnedIps)
{
    auto *implPtr = impl.get();
    return us::utils::Async(
               implPtr->crawlingTaskProcessor, "create-webshot",
               [implPtr, link = std::move(link), pinned = std::move(pinnedIps)]() mutable {
                   return implPtr->runCrawlJob(std::move(link), std::move(pinned));
               }
    ).Get();
}

std::optional<Webshot> WebshotCrud::findWebshot(Uuid uuid)
{
    const auto location =
        impl->readonly(sql::kSelectWebshot.data(), uuid).AsOptionalSingleRow<std::string>();
    if (!location) {
        LOG_INFO() << fmt::format("UUID not found: {}", us::utils::ToString(uuid));
        return {};
    }
    return {{*location}};
}

dto::PagedFindWebshotByUrlResponse
WebshotCrud::findWebshotByLinkPage(const Link &link, const std::optional<std::string> &pageToken)
{
    namespace crud = v1::crud;

    struct Row {
        Uuid uuid;
        pg::TimePointTz timepoint;
    };
    std::vector<Row> dbRows;
    const auto &norm = link.normalized();
    if (!pageToken || pageToken->empty()) {
        dbRows = impl->readonly(sql::kSelectWebshotByLinkFirst.data(), norm, impl->webshotsPageMax)
                     .AsContainer<std::vector<Row>>(pg::kRowTag);
    } else {
        auto cur = crud::decodeCursor(*pageToken);
        if (!cur)
            throw errors::InvalidPageTokenException("invalid page_token");
        dbRows = impl->readonly(
                         sql::kSelectWebshotByLinkNext.data(), norm, impl->webshotsPageMax,
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
    std::optional<std::string> next;
    if (v1::utils::ssize(items) == impl->webshotsPageMax && !items.empty()) {
        const auto &last = items.back();
        auto tp = last.created_at.GetTimePoint();
        crud::Cursor cursor{tp, last.uuid};
        next = crud::encodeCursor(cursor);
    }
    return {items, next};
}

dto::PagedFindWebshotByPrefixResponse WebshotCrud::findWebshotsByPrefixPage(
    const std::string &normalizedPrefix, const std::optional<std::string> &pageToken
)
{
    namespace crud = v1::crud;

    const auto lower = normalizedPrefix;
    const auto upperOpt = crud::upperExclusiveBound(normalizedPrefix);
    const auto linksPerPage = impl->webshotsLinksPerPageMax;

    std::optional<crud::PrefixCursor> cur;
    if (pageToken && !pageToken->empty()) {
        cur = crud::decodePrefixCursor(*pageToken);
        if (!cur || cur->prefix != normalizedPrefix)
            throw errors::InvalidPageTokenException("invalid page_token");
    }

    auto selectLinksFirst = [&](int64_t limit) {
        return impl->readonly(sql::kSelectDistinctLinksByPrefixFirst.data(), lower, upperOpt, limit)
            .AsContainer<std::vector<std::string>>();
    };
    auto selectLinksNext = [&](const std::string &fromLink, int64_t limit) {
        return impl
            ->readonly(
                sql::kSelectDistinctLinksByPrefixNext.data(), lower, upperOpt, fromLink, limit
            )
            .AsContainer<std::vector<std::string>>();
    };

    std::vector<std::string> links;
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
    std::string lastLink;
    std::optional<Row> lastRow;

    auto selectRowsForLink = [&](const std::string &link, size_t idx) {
        if (idx == 0 && cur && cur->createdAt && cur->id) {
            return impl
                ->readonly(
                    sql::kSelectWebshotByLinkNext.data(), link, impl->webshotsPerLinkMax,
                    pg::TimePointTz(*cur->createdAt), *cur->id
                )
                .AsContainer<std::vector<Row>>(pg::kRowTag);
        }
        return impl->readonly(sql::kSelectWebshotByLinkFirst.data(), link, impl->webshotsPerLinkMax)
            .AsContainer<std::vector<Row>>(pg::kRowTag);
    };

    for (size_t idx = 0; idx < links.size(); idx++) {
        const auto &link = links[idx];
        auto rows = selectRowsForLink(link, idx);
        for (auto &&r : rows) {
            items.emplace_back(
                r.uuid,
                us::utils::datetime::TimePointTz(static_cast<system_clock::time_point>(r.tp)), link
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
            next = crud::encodePrefixCursor(normalizedPrefix, lastLink, tp, lastRow->uuid);
        } else {
            next = crud::encodePrefixCursor(normalizedPrefix, lastLink);
        }
    }

    return {items, next};
}

void WebshotCrud::disallowAndPurgeHost(std::string host)
{
    impl->denylist.insertHost(host, "disallow-and-purge");

    LOG_INFO() << fmt::format("enqueued for host {}", host);

    impl->backgroundTaskStorage.AsyncDetach("purge-host-lambda", [implPtr = impl.get(), host]() {
        try {
            engine::current_task::SetDeadline(
                engine::Deadline::FromDuration(std::chrono::seconds(implPtr->purgeJobTimeoutSec))
            );
            LOG_INFO() << fmt::format("Starting purge for denylisted host {}", host);
            implPtr->purgeHost(host);
        } catch (const std::exception &e) {
            LOG_ERROR() << fmt::format("Purge task failed for {}: {}", host, e.what());
        }
    });
}
