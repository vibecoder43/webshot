#include "include/webshot_crud.hpp"
/**
 * @file
 * @brief Implementation of storage and crawl orchestration.
 *
 * Implements the `WebshotCrud` component, including background crawl startup,
 * metadata writes, and various paged queries.
 */
#include "include/container_guard.hpp"
#include "include/link.hpp"
#include "include/s3_secdist.hpp"
#include "include/s3_v4_client.hpp"
#include "include/server_errors.hpp"
#include "include/sql.hpp"
#include "include/utils.hpp"
#include "include/webshot_config.hpp"
#include "include/webshot_cursor.hpp"
#include "include/webshot_denylist.hpp"
#include "include/webshot_pagination.hpp"
#include "include/webshot_prefix_pagination.hpp"
#include "schemas/webshot.hpp"

#include <chrono>
#include <exception>
#include <optional>
#include <string>
#include <userver/utils/assert.hpp>
#include <utility>

#include <boost/uuid/uuid.hpp>

#include <fmt/format.h>

#include <userver/clients/dns/component.hpp>
#include <userver/clients/http/component.hpp>
#include <userver/components/component.hpp>
#include <userver/components/component_base.hpp>
#include <userver/concurrent/background_task_storage.hpp>
#include <userver/crypto/base64.hpp>
#include <userver/engine/semaphore.hpp>
#include <userver/engine/subprocess/process_starter.hpp>
#include <userver/engine/task/current_task.hpp>
#include <userver/engine/task/task_processor_fwd.hpp>
#include <userver/formats/json.hpp>
#include <userver/fs/blocking/read.hpp>
#include <userver/fs/blocking/temp_directory.hpp>
#include <userver/fs/blocking/write.hpp>
#include <userver/fs/read.hpp>
#include <userver/fs/write.hpp>
#include <userver/logging/log.hpp>
#include <userver/storages/postgres/cluster.hpp>
#include <userver/storages/postgres/io/chrono.hpp>
#include <userver/storages/postgres/io/row_types.hpp>
#include <userver/storages/postgres/io/uuid.hpp>
#include <userver/storages/postgres/postgres.hpp>
#include <userver/storages/secdist/component.hpp>
#include <userver/storages/secdist/secdist.hpp>
#include <userver/utils/async.hpp>
#include <userver/utils/boost_uuid4.hpp>
#include <userver/utils/datetime/timepoint_tz.hpp>
#include <userver/yaml_config/merge_schemas.hpp>
#include <userver/yaml_config/yaml_config.hpp>

namespace pg = us::storages::postgres;
namespace engine = us::engine;
namespace concurrent = userver::concurrent;
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
    crawler-collection-name:
        type: string
        description: 'Collection name used by the crawler for the capture'
    crawler-workers:
        type: integer
        minimum: 1
        description: 'Number of crawler workers per job'
    crawler-page-limit:
        type: integer
        minimum: 1
        description: 'Max pages to crawl per job'
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
    crawler-lang:
        type: string
        description: 'Language hint passed to the crawler'
    crawler-scope-type:
        type: string
        description: 'Scope type passed to the crawler (e.g. page-spa)'
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

/** @brief Private pimpl that holds dependencies and query helpers. */
struct [[nodiscard]] CrawlContext;

class [[nodiscard]] WebshotCrud::Impl {
public:
    const int64_t webshotsPageMax;
    const int64_t webshotsPerLinkMax;
    const int64_t webshotsLinksPerPageMax;
    const std::string crawlerNetwork;
    const std::string crawlerImage;
    const std::string crawlerCollectionName;
    const int64_t crawlerWorkers;
    const int64_t crawlerPageLimit;
    const int64_t crawlerPageLoadTimeoutSec;
    const int64_t crawlerPostLoadDelaySec;
    const int64_t crawlerNetIdleWaitSec;
    const int64_t crawlerPageExtraDelaySec;
    const int64_t crawlerBehaviorTimeoutSec;
    const std::string crawlerLang;
    const std::string crawlerScopeType;
    const WebshotConfig &svcCfg;
    pg::ClusterPtr cluster;
    us::clients::dns::Resolver &resolver;
    us::clients::http::Client &httpClient;
    us::s3api::ClientPtr s3Client;
    WebshotDenylist &denylist;
    engine::CancellableSemaphore crawlSlots;
    // must die first
    concurrent::BackgroundTaskStorage backgroundTaskStorage;

    [[nodiscard]] CrawlContext makeCrawlContext(Link link, std::vector<std::string> pinnedIps);
    [[nodiscard]] bool
    runCrawlerForContext(CrawlContext &ctx, engine::subprocess::ProcessStarter &starter);
    [[nodiscard]] bool persistMetadataForContext(const CrawlContext &ctx);
    void purgeHostAndSubdomains(const std::string &hostLowerPunycode);
    explicit Impl(
        const us::components::ComponentConfig &cfg, const us::components::ComponentContext &ctx
    )
        : webshotsPageMax(cfg["webshots-page-max"].As<int64_t>()),
          webshotsPerLinkMax(cfg["webshots-per-link-max"].As<int64_t>()),
          webshotsLinksPerPageMax(cfg["webshots-links-per-page-max"].As<int64_t>()),
          crawlerNetwork(cfg["crawler-network"].As<std::string>()),
          crawlerImage(cfg["crawler-image"].As<std::string>("webrecorder/browsertrix-crawler")),
          crawlerCollectionName(cfg["crawler-collection-name"].As<std::string>("webshot")),
          crawlerWorkers(cfg["crawler-workers"].As<int64_t>()),
          crawlerPageLimit(cfg["crawler-page-limit"].As<int64_t>()),
          crawlerPageLoadTimeoutSec(cfg["crawler-page-load-timeout-sec"].As<int64_t>()),
          crawlerPostLoadDelaySec(cfg["crawler-post-load-delay-sec"].As<int64_t>()),
          crawlerNetIdleWaitSec(cfg["crawler-net-idle-wait-sec"].As<int64_t>()),
          crawlerPageExtraDelaySec(cfg["crawler-page-extra-delay-sec"].As<int64_t>()),
          crawlerBehaviorTimeoutSec(cfg["crawler-behavior-timeout-sec"].As<int64_t>()),
          crawlerLang(cfg["crawler-lang"].As<std::string>("en")),
          crawlerScopeType(cfg["crawler-scope-type"].As<std::string>("page-spa")),
          svcCfg(ctx.FindComponent<WebshotConfig>()),
          cluster(ctx.FindComponent<us::components::Postgres>("webshot-meta-db").GetCluster()),
          resolver(ctx.FindComponent<us::clients::dns::Component>().GetResolver()),
          httpClient(ctx.FindComponent<us::components::HttpClient>().GetHttpClient()),
          denylist(ctx.FindComponent<WebshotDenylist>()),
          crawlSlots(cfg["crawl-concurrency"].As<size_t>()),
          backgroundTaskStorage(engine::current_task::GetBlockingTaskProcessor())
    {
        const auto &secdist = ctx.FindComponent<us::components::Secdist>().Get();
        const auto &creds = secdist.Get<S3CredentialsSecdist>();
        UINVARIANT(
            creds.access_key_id && creds.secret_access_key,
            "missing required S3 secdist credentials"
        );
        s3Client = s3v4::MakeS3ClientV4(
            httpClient,
            s3v4::S3V4Config{
                svcCfg.s3Endpoint(), svcCfg.s3Region(), svcCfg.s3Timeout(), /*virtualHosted*/ false
            },
            s3v4::S3Credentials{*creds.access_key_id, *creds.secret_access_key, std::nullopt},
            std::string{}
        );
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
    std::string host;
    std::vector<std::string> pinnedIps;
    us::fs::blocking::TempDirectory archiveRoot;
    Uuid id;
    std::string keyOnly;
    std::string s3Key;
    std::string location;
};

[[nodiscard]] CrawlContext
WebshotCrud::Impl::makeCrawlContext(Link link, std::vector<std::string> pinnedIps)
{
    CrawlContext ctx{std::move(link),
                     std::string{},
                     std::move(pinnedIps),
                     us::fs::blocking::TempDirectory::Create(),
                     Uuid{},
                     std::string{},
                     std::string{},
                     std::string{}};
    ctx.host = ctx.link.host();
    ctx.id = us::utils::generators::GenerateBoostUuid();
    ctx.keyOnly = us::utils::ToString(ctx.id);
    ctx.s3Key = fmt::format("{}/{}", svcCfg.s3Bucket(), ctx.keyOnly);
    ctx.location = fmt::format("{}/{}", svcCfg.publicBaseUrl(), ctx.keyOnly);
    return ctx;
}

[[nodiscard]] bool WebshotCrud::Impl::runCrawlerForContext(
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
        createArgs.push_back(fmt::format("{}:{}", ctx.host, ip));
    }

    const std::string &collection = crawlerCollectionName;
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
         fmt::format("{}", crawlerPageLimit),
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
        LOG_INFO() << fmt::format("Failed to crawl {}, child process failed", ctx.link.httpUrl());
        return false;
    }
    // do eagerly
    ctrGuard.remove();

    const auto pathToArchive = fmt::format(
        "{0}/collections/{1}/{1}.wacz", ctx.archiveRoot.GetPath(), collection
    );

    if (!us::fs::FileExists(engine::current_task::GetBlockingTaskProcessor(), pathToArchive)) {
        LOG_INFO() << fmt::format("Failed to crawl {}, no WACZ", ctx.link.httpUrl());
        return false;
    }

    try {
        s3Client->PutObject(
            ctx.s3Key, us::fs::blocking::ReadFileContents(pathToArchive), std::nullopt,
            "application/zip", std::nullopt, std::nullopt
        );
    } catch (const std::exception &e) {
        LOG_ERROR() << fmt::format("S3 upload failed for {}: {}", ctx.s3Key, e.what());
        return false;
    }

    return true;
}

[[nodiscard]] bool WebshotCrud::Impl::persistMetadataForContext(const CrawlContext &ctx)
{
    auto host = ctx.host;
    std::string hostRev(rbegin(host), rend(host));

    if (!denylist.isAllowedHost(host)) {
        try {
            s3Client->DeleteObject(ctx.s3Key);
        } catch (const std::exception &) {
            LOG_ERROR() << fmt::format("error deleting {}", ctx.s3Key);
        }
        LOG_INFO() << fmt::format("Host became denylisted during crawl: {}", host);
        return false;
    }

    try {
        readwrite(sql::kInsertWebshot.data(), ctx.id, ctx.link.normalized(), hostRev, ctx.location)
            .AsSingleRow<Uuid>();
    } catch (const std::exception &e) {
        try {
            s3Client->DeleteObject(ctx.s3Key);
        } catch (const std::exception &) {
            LOG_ERROR() << fmt::format("error deleting {}", ctx.s3Key);
        }
        LOG_ERROR(
        ) << fmt::format("DB insert failed for {}: {}", us::utils::ToString(ctx.id), e.what());
        return false;
    }

    return true;
}

void WebshotCrud::Impl::purgeHostAndSubdomains(const std::string &hostLowerPunycode)
{
    std::string d_rev(hostLowerPunycode.rbegin(), hostLowerPunycode.rend());
    const std::size_t kBatch = 1000;
    while (true) {
        try {
            auto res = readonly(
                sql::kSelectIdsByHostOrSubdomainsPaged.data(), d_rev, static_cast<int64_t>(kBatch)
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
                    s3Client->DeleteObject(key);
                } catch (const std::exception &e) {
                    LOG_ERROR() << fmt::format("S3 delete failed for key {}: {}", key, e.what());
                }
            }
            static_cast<void>(readwrite(sql::kDeleteWebshotsByIds.data(), ids));
        } catch (const std::exception &e) {
            LOG_ERROR(
            ) << fmt::format("denylist purge failed for {}: {}", hostLowerPunycode, e.what());
            break;
        }
    }
}

void WebshotCrud::createWebshot(Link link, std::vector<std::string> pinnedIps)
{
    UINVARIANT(!pinnedIps.empty(), "can't crawl with no IPs");
    impl->backgroundTaskStorage.AsyncDetach(
        "create-webshot-lambda",
        [impl = impl.get(), link = std::move(link), pinnedIps = std::move(pinnedIps)]() mutable {
            try {
                std::shared_lock<engine::CancellableSemaphore> slotLock(impl->crawlSlots);

                engine::subprocess::ProcessStarter starter(
                    engine::current_task::GetBlockingTaskProcessor()
                );

                CrawlContext ctx = impl->makeCrawlContext(std::move(link), std::move(pinnedIps));

                if (!impl->runCrawlerForContext(ctx, starter))
                    return;

                if (!impl->persistMetadataForContext(ctx))
                    return;
            } catch (const engine::SemaphoreLockCancelledError &e) {
                LOG_INFO() << "Crawl task cancelled while waiting for slot: " << e.what();
            }
        }
    );
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

void WebshotCrud::disallowAndPurgeDomain(std::string domain)
{
    impl->denylist.insertDomain(domain);

    LOG_INFO() << fmt::format("enqueued for domain {}", domain);

    impl->backgroundTaskStorage.AsyncDetach(
        "purge-domain-lambda",
        [implPtr = impl.get(), domain]() {
            try {
                LOG_INFO() << fmt::format("Starting purge for denylisted domain {}", domain);
                implPtr->purgeHostAndSubdomains(domain);
            } catch (const std::exception &e) {
                LOG_ERROR() << fmt::format("Purge task failed for {}: {}", domain, e.what());
            }
        }
    );
}
