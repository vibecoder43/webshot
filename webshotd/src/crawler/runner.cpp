#include "crawler/runner.hpp"

#include "config.hpp"
#include "crawler/artifacts.hpp"
#include "crawler/browser_session.hpp"
#include "crawler/cdp_client.hpp"
#include "crawler/egress_proxy.hpp"
#include "crawler/failure.hpp"
#include "crawler/launch_policy.hpp"
#include "crawler/limits.hpp"
#include "deadline_utils.hpp"
#include "denylist.hpp"
#include "grab_value.hpp"
#include "integers.hpp"
#include "json.hpp"
#include "link.hpp"
#include "prefix_utils.hpp"
#include "schema/cdp.hpp"
#include "text.hpp"
#include "try.hpp"
#include "url.hpp"
#include "uuid_format.hpp"

#include <generated/browser_sandbox.sh.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <absl/strings/ascii.h>

#include <userver/clients/dns/resolver.hpp>
#include <userver/concurrent/variable.hpp>
#include <userver/crypto/base64.hpp>
#include <userver/crypto/exception.hpp>
#include <userver/engine/async.hpp>
#include <userver/engine/condition_variable.hpp>
#include <userver/engine/deadline.hpp>
#include <userver/engine/exception.hpp>
#include <userver/engine/sleep.hpp>
#include <userver/engine/subprocess/process_starter.hpp>
#include <userver/formats/json.hpp>
#include <userver/formats/json/value_builder.hpp>
#include <userver/fs/blocking/read.hpp>
#include <userver/fs/blocking/temp_directory.hpp>
#include <userver/fs/blocking/write.hpp>
#include <userver/logging/log.hpp>
#include <userver/utils/assert.hpp>
#include <userver/utils/boost_uuid4.hpp>
#include <userver/utils/datetime.hpp>
#include <userver/utils/resources.hpp>
#include <userver/utils/traceful_exception.hpp>
namespace chrono = std::chrono;
namespace dns = us::clients::dns;

using namespace text::literals;

namespace v1 {
namespace {

using crawler::describeCdpFailure;
using v1::Expected;

constexpr auto kCdpWsPayloadSlackBytes = 2_i64 * 1024_i64 * 1024_i64;
const auto kLocalFixtureHttpPort = "18080"_t;
const auto kLocalFixtureHttpsPort = "18443"_t;
constexpr std::array kLocalFixtureHosts = {
    std::string_view{"test-target"},
    std::string_view{"asset.test-target"},
    std::string_view{"untrusted.test-target"},
};

[[nodiscard]] i64 computeCdpMaxRemotePayloadBytes(i64 maxArchiveBytes)
{
    return (maxArchiveBytes * 4_i64) / 3_i64 + kCdpWsPayloadSlackBytes;
}

[[nodiscard]] String currentTimestamp()
{
    return String::fromBytes(datetime::UtcTimestring(datetime::Now(), datetime::kRfc3339Format))
        .expect();
}

[[nodiscard]] std::unordered_map<std::string, std::string>
normalizeHeaders(const dto::CdpHeaders &headers)
{
    std::unordered_map<std::string, std::string> out;
    for (const auto &[name, value] : headers.extra)
        out.emplace(absl::AsciiStrToLower(std::string_view{name}), value);
    return out;
}

[[nodiscard]] bool isLocalFixtureHost(const String &host) noexcept
{
    return std::ranges::find(kLocalFixtureHosts, host.view()) != std::end(kLocalFixtureHosts);
}

[[nodiscard]] String canonicalizeCapturedUrl(const String &urlText)
{
    const auto maybeUrl = Url::fromText(urlText);
    if (!maybeUrl)
        return urlText;
    if (!maybeUrl->isHttp() && !maybeUrl->isHttps())
        return urlText;
    if (!maybeUrl->hasPort())
        return urlText;
    if (!isLocalFixtureHost(maybeUrl->hostname()))
        return urlText;

    const auto port = maybeUrl->port();
    const auto matchesFixturePort = (maybeUrl->isHttp() && port == kLocalFixtureHttpPort) ||
                                    (maybeUrl->isHttps() && port == kLocalFixtureHttpsPort);
    if (!matchesFixturePort)
        return urlText;

    return maybeUrl->stripped(Url::StripOptions::kStripPort).href();
}

[[nodiscard]] std::string
canonicalizeCapturedLocationHeader(const String &responseUrl, std::string_view locationValue)
{
    auto location = String::fromBytes(locationValue);
    if (!location)
        return std::string(locationValue);
    if (location->empty() || location->startsWith('/') || location->startsWith('?') ||
        location->startsWith("//")) {
        return std::to_string(*location);
    }

    const auto canonicalLocation = canonicalizeCapturedUrl(*location);
    const auto maybeCanonicalUrl = Url::fromText(canonicalLocation);
    const auto maybeResponseUrl = Url::fromText(responseUrl);
    if (!maybeCanonicalUrl || !maybeResponseUrl)
        return std::to_string(canonicalLocation);

    if (maybeCanonicalUrl->isHttp() == maybeResponseUrl->isHttp() &&
        maybeCanonicalUrl->host() == maybeResponseUrl->host()) {
        return std::to_string(maybeCanonicalUrl->pathWithSearch());
    }

    return std::to_string(canonicalLocation);
}

[[nodiscard]] std::unordered_map<std::string, std::string>
normalizeHeadersOrEmpty(const std::optional<dto::CdpHeaders> &headers)
{
    if (!headers)
        return {};
    return normalizeHeaders(*headers);
}

[[nodiscard]] std::unordered_map<std::string, std::string>
normalizeHeadersForCapture(const std::optional<dto::CdpHeaders> &headers, const String &responseUrl)
{
    auto normalized = normalizeHeadersOrEmpty(headers);
    if (const auto it = normalized.find("location"); it != std::end(normalized))
        it->second = canonicalizeCapturedLocationHeader(responseUrl, it->second);
    return normalized;
}

[[nodiscard]] std::optional<String> stringOrNull(const std::optional<std::string> &value)
{
    if (!value)
        return {};
    return TRY(String::fromBytes(*value));
}

[[nodiscard]] String generatePageId()
{
    return text::format("{}", us::utils::generators::GenerateBoostUuid());
}

struct [[nodiscard]] CaptureFailure final {
    String detail;
    std::optional<crawler::SeedPageProbe> seedProbe;
};

template <typename T>
[[nodiscard]] Expected<T, String> parseEventParams(const crawler::CdpEvent &event)
{
    if (!event.params)
        return Unex(text::format("{} missing params", event.method));
    return exu::json::as<T>(
        event.params->extra, text::format("{} has invalid params", event.method)
    );
}

struct [[nodiscard]] RetainedBodyBudget {
    RetainedBodyBudget(i64 maxBytes, i64 retainedBytes)
        : maxBytes(maxBytes), retainedBytes(retainedBytes)
    {
    }

    i64 maxBytes;
    i64 retainedBytes;
};

struct [[nodiscard]] CaptureWithNetwork final {
    crawler::CapturedExchange exchange;
    i64 proxyDownBytes{0};
};

[[nodiscard]] Expected<std::string, String>
retainBody(const std::string &body, RetainedBodyBudget &budget)
{
    const auto nextRetainedBytes = budget.retainedBytes + ssize(body);
    if (nextRetainedBytes > budget.maxBytes)
        return Unex(
            text::format(
                "size_limit: retained body bytes {} exceeded size limit {}", nextRetainedBytes,
                budget.maxBytes
            )
        );
    budget.retainedBytes = nextRetainedBytes;
    return body;
}

[[nodiscard]] bool responseCanHaveBody(const String &method, i64 statusCode)
{
    if (method == "HEAD"_t)
        return false;
    return (statusCode < 100_i64 || statusCode >= 200_i64) && statusCode != 204_i64 &&
           statusCode != 304_i64;
}

[[nodiscard]] std::optional<String> buildUrlOrigin(const String &urlText)
{
    const auto maybeUrl = TRY(Url::fromText(urlText));
    if (!maybeUrl.isHttp() && !maybeUrl.isHttps())
        return {};

    return maybeUrl.origin();
}

[[nodiscard]] String resolveRedirectTargetUrl(
    const String &baseUrl, const String &requestUrl,
    const std::optional<dto::NetworkResponse> &redirectResponse
)
{
    if (!redirectResponse)
        return canonicalizeCapturedUrl(requestUrl);

    const auto headers = normalizeHeadersForCapture(redirectResponse->headers, baseUrl);
    const auto locationIt = headers.find("location");
    if (locationIt == std::end(headers) || locationIt->second.empty())
        return canonicalizeCapturedUrl(requestUrl);

    const auto location = String::fromBytes(locationIt->second);
    if (!location)
        return canonicalizeCapturedUrl(requestUrl);
    if (const auto absoluteLocation = Url::fromText(*location))
        return canonicalizeCapturedUrl(absoluteLocation->href());

    const auto origin = buildUrlOrigin(baseUrl);
    if (!origin)
        return canonicalizeCapturedUrl(requestUrl);

    if (location->startsWith("//")) {
        const auto maybeBaseUrl = Url::fromText(baseUrl);
        if (!maybeBaseUrl)
            return canonicalizeCapturedUrl(requestUrl);
        return canonicalizeCapturedUrl(
            text::format("{}:{}", maybeBaseUrl->isHttps() ? "https" : "http", *location)
        );
    }

    if (location->startsWith('/'))
        return canonicalizeCapturedUrl(text::format("{}{}", *origin, *location));

    if (location->startsWith('?')) {
        const auto maybeBaseUrl = Url::fromText(baseUrl);
        if (!maybeBaseUrl)
            return canonicalizeCapturedUrl(requestUrl);
        return canonicalizeCapturedUrl(
            maybeBaseUrl->withoutSearch().withoutHash().withSearch(*location).href()
        );
    }

    return canonicalizeCapturedUrl(requestUrl);
}

[[nodiscard]] Expected<std::optional<Link>, String>
normalizeInterceptedLinkForDenylist(const Config &config, const String &requestUrl)
{
    if (requestUrl.startsWith("ws://")) {
        const auto parsed = Url::fromText(requestUrl);
        if (!parsed)
            return {};
        const auto link = Link::fromText(
            parsed->withProtocol("http"_t).href(), config.urlBytesMax()
        );
        if (!link)
            return Unex(text::format("failed to normalize intercepted request url {}", requestUrl));
        return *link;
    }
    if (requestUrl.startsWith("wss://")) {
        const auto parsed = Url::fromText(requestUrl);
        if (!parsed)
            return {};
        const auto link = Link::fromText(
            parsed->withProtocol("https"_t).href(), config.urlBytesMax()
        );
        if (!link)
            return Unex(text::format("failed to normalize intercepted request url {}", requestUrl));
        return *link;
    }

    const auto parsed = Url::fromText(requestUrl);
    if (!parsed || (!parsed->isHttp() && !parsed->isHttps()))
        return {};

    const auto link = Link::fromText(requestUrl, config.urlBytesMax());
    if (!link)
        return Unex(text::format("failed to normalize intercepted request url {}", requestUrl));
    return *link;
}

[[nodiscard]] Expected<bool, String>
isAllowedByDenylist(Denylist &denylist, const Config &config, const String &requestUrl)
{
    const auto normalized = TRY(normalizeInterceptedLinkForDenylist(config, requestUrl));
    if (!normalized)
        return true;

    const auto allowed = denylist.isAllowedPrefix(prefix::makePrefixKey(*normalized));
    if (!allowed)
        return Unex("denylist check failed during fetch interception"_t);
    return *allowed;
}

[[nodiscard]] std::vector<dto::FetchHeaderEntry> buildBlockedFetchHeaders(usize bodyBytes)
{
    std::vector<dto::FetchHeaderEntry> headers;
    headers.push_back(
        dto::FetchHeaderEntry{
            .name = "Content-Type",
            .value = "text/plain; charset=utf-8",
        }
    );
    headers.push_back(
        dto::FetchHeaderEntry{
            .name = "Content-Length",
            .value = std::to_string(bodyBytes),
        }
    );
    headers.push_back(
        dto::FetchHeaderEntry{
            .name = "Cache-Control",
            .value = "no-store",
        }
    );
    return headers;
}

struct [[nodiscard]] TrackedRequest {
    String requestUrl;
    String method;
    std::optional<i64> statusCode;
    std::optional<String> statusMessage;
    std::optional<std::unordered_map<std::string, std::string>> headers;
    std::optional<String> timestamp;
    std::optional<String> loaderId;
    std::optional<String> frameId;
    std::optional<String> resourceType;
    bool loaded{false};
    bool isTrackedMainDocument{false};
};

class [[nodiscard]] PageTracker final {
public:
    PageTracker(String sessionId, String targetId)
    {
        auto state = data.Lock();
        state->sessionId = std::move(sessionId);
        state->targetId = std::move(targetId);
        state->pageId = generatePageId();
    }

    void beginSeedNavigation(const String &seedUrl)
    {
        auto state = data.Lock();
        state->seedNavigationStarted = true;
        state->seedNavigationUrl = seedUrl;
    }

    void setMainFrameId(String mainFrameId)
    {
        auto state = data.Lock();
        state->mainFrameId = std::move(mainFrameId);
    }

    void setExpectedMainLoaderId(std::optional<String> loaderId)
    {
        if (!loaderId)
            return;
        auto state = data.Lock();
        state->mainLoaderId = std::move(*loaderId);
    }

    void handleEvent(const crawler::CdpEvent &event)
    {
        auto state = data.Lock();
        const auto method = event.method.view();
        if (method == "Target.targetCrashed") {
            if (event.params) {
                const auto crashed = event.params->extra.As<dto::TargetTargetCrashedEvent>();
                if (crashed.targetId && state->targetId.view() == *crashed.targetId)
                    state->mainRequestFailure = "page target crashed"_t;
            }
            return;
        }
        if (method == "Target.detachedFromTarget") {
            if (event.params) {
                const auto detachedSessionId = event.params->extra["sessionId"];
                if (!detachedSessionId.IsMissing() &&
                    detachedSessionId.As<std::string>() == state->sessionId.view()) {
                    state->mainRequestFailure = "target session detached"_t;
                }
            }
            return;
        }
        if (method == "Target.targetDestroyed") {
            if (event.params) {
                const auto destroyedTargetId = event.params->extra["targetId"];
                if (!destroyedTargetId.IsMissing() &&
                    destroyedTargetId.As<std::string>() == state->targetId.view()) {
                    state->mainRequestFailure = "page target destroyed"_t;
                }
            }
            return;
        }
        if (method == "Inspector.detached") {
            if (event.sessionId && *event.sessionId != state->sessionId)
                return;

            if (event.params) {
                const auto reason = event.params->extra["reason"];
                if (!reason.IsMissing()) {
                    state->mainRequestFailure = text::format(
                        "inspector detached: {}", reason.As<std::string>()
                    );
                    return;
                }
            }
            state->mainRequestFailure = "inspector detached"_t;
            return;
        }

        if (!event.sessionId || *event.sessionId != state->sessionId)
            return;

        if (method == "Page.loadEventFired") {
            state->loaded = true;
            return;
        }
        if (method == "Network.requestWillBeSent") {
            auto parsed = parseEventParams<dto::NetworkRequestWillBeSentEvent>(event);
            if (!parsed)
                state->mainRequestFailure = parsed.error();
            else
                handleRequestWillBeSent(*state, grabValueOf(parsed));
            return;
        }
        if (method == "Network.responseReceived") {
            auto parsed = parseEventParams<dto::NetworkResponseReceivedEvent>(event);
            if (!parsed)
                state->mainRequestFailure = parsed.error();
            else
                handleResponseReceived(*state, grabValueOf(parsed));
            return;
        }
        if (method == "Network.loadingFinished") {
            auto parsed = parseEventParams<dto::NetworkLoadingFinishedEvent>(event);
            if (!parsed)
                state->mainRequestFailure = parsed.error();
            else
                handleLoadingFinished(*state, grabValueOf(parsed));
            return;
        }
        if (method == "Network.loadingFailed") {
            auto parsed = parseEventParams<dto::NetworkLoadingFailedEvent>(event);
            if (!parsed)
                state->mainRequestFailure = parsed.error();
            else
                handleLoadingFailed(*state, grabValueOf(parsed));
        }
    }

    [[nodiscard]] bool isLoadedOrFailed() const
    {
        const auto state = data.Lock();
        return state->loaded || state->mainRequestFailure;
    }

    [[nodiscard]] bool hasMainDocumentOrFailure() const
    {
        const auto state = data.Lock();
        const auto *request = activeMainRequest(*state);
        return state->mainRequestFailure ||
               (state->completedMainRequest && state->completedMainRequest->loaded &&
                hasResponse(*state->completedMainRequest)) ||
               (request != nullptr && hasResponse(*request) && request->loaded);
    }

    [[nodiscard]] bool isIdleFor(chrono::seconds idle) const
    {
        const auto state = data.Lock();
        return state->inflight.empty() && datetime::SteadyNow() - state->lastNetworkAt >= idle;
    }

    [[nodiscard]] datetime::SteadyClock::time_point idleDeadline(chrono::seconds idle) const
    {
        const auto state = data.Lock();
        return state->lastNetworkAt + idle;
    }

    [[nodiscard]] std::optional<crawler::SeedPageProbe> currentSeedProbe() const
    {
        const auto state = data.Lock();
        if (const auto *request = resolvedMainRequest(*state);
            request != nullptr && request->statusCode) {
            const i64 loadState{request->loaded && !state->mainRequestFailure ? 2_i64 : 0_i64};
            return crawler::SeedPageProbe{
                .status = raw(*request->statusCode),
                .loadState = raw(loadState),
            };
        }

        if (state->mainRequestId || state->mainRequestFailure || state->loaded)
            return crawler::SeedPageProbe{.status = raw(0_i64), .loadState = raw(0_i64)};

        return {};
    }

    [[nodiscard]] std::optional<String> failureReason() const
    {
        const auto state = data.Lock();
        return state->mainRequestFailure;
    }

    void fail(String reason)
    {
        auto state = data.Lock();
        state->mainRequestFailure = std::move(reason);
    }

    [[nodiscard]] Expected<std::string, String> readBody(
        crawler::CdpSession &cdpSession, RetainedBodyBudget &budget, const std::string &fallbackBody
    ) const
    {
        if (!fallbackBody.empty())
            return retainBody(fallbackBody, budget);
        std::optional<String> bodyRequestId;
        {
            const auto state = data.Lock();
            if (state->mainResponseRequestId)
                bodyRequestId = state->mainResponseRequestId;
            else if (state->mainRequestId)
                bodyRequestId = state->mainRequestId;
        }
        if (!bodyRequestId)
            return retainBody(fallbackBody, budget);

        try {
            dto::NetworkGetResponseBodyParams params;
            params.requestId = std::to_string(*bodyRequestId);
            const auto body = cdpSession.send<dto::NetworkGetResponseBodyResult>(
                "Network.getResponseBody"_t, params
            );
            if (!body)
                return retainBody(fallbackBody, budget);
            return retainBody(
                body->base64Encoded ? us::crypto::base64::Base64Decode(body->body) : body->body,
                budget
            );
        } catch (const us::crypto::CryptoException &) {
            return retainBody(fallbackBody, budget);
        }
    }

    [[nodiscard]] Expected<std::vector<crawler::CapturedResource>, String>
    readResources(crawler::CdpSession &cdpSession, RetainedBodyBudget &budget) const
    {
        std::vector<crawler::CapturedResource> resources{};
        std::vector<std::pair<String, TrackedRequest>> requests{};
        {
            const auto state = data.Lock();
            resources = state->redirectedResources;
            requests.reserve(state->activeRequests.size());
            for (const auto &[requestId, request] : state->activeRequests)
                requests.emplace_back(requestId, request);
        }

        for (const auto &[requestId, request] : requests) {
            if (request.isTrackedMainDocument || !hasResponse(request)) {
                continue;
            }

            const auto response = toMainResponse(request);

            if (!responseCanHaveBody(request.method, response.statusCode)) {
                resources.push_back({
                    request.requestUrl,
                    request.method,
                    request.resourceType,
                    response.statusCode,
                    response.statusMessage,
                    response.headers,
                    {},
                    response.timestamp,
                });
                continue;
            }

            try {
                dto::NetworkGetResponseBodyParams params;
                params.requestId = std::to_string(requestId);
                const auto bodyValue = cdpSession.send<dto::NetworkGetResponseBodyResult>(
                    "Network.getResponseBody"_t, params
                );
                if (!bodyValue) {
                    resources.push_back({
                        request.requestUrl,
                        request.method,
                        request.resourceType,
                        response.statusCode,
                        response.statusMessage,
                        response.headers,
                        {},
                        response.timestamp,
                    });
                    continue;
                }
                auto body = retainBody(
                    bodyValue->base64Encoded ? us::crypto::base64::Base64Decode(bodyValue->body)
                                             : bodyValue->body,
                    budget
                );
                if (!body)
                    return Unex(std::move(body).error());
                resources.push_back({
                    request.requestUrl,
                    request.method,
                    request.resourceType,
                    response.statusCode,
                    response.statusMessage,
                    response.headers,
                    grabValueOf(body),
                    response.timestamp,
                });
            } catch (const us::crypto::CryptoException &) {
                resources.push_back({
                    request.requestUrl,
                    request.method,
                    request.resourceType,
                    response.statusCode,
                    response.statusMessage,
                    response.headers,
                    {},
                    response.timestamp,
                });
            }
        }

        std::ranges::sort(resources, [](const auto &left, const auto &right) {
            return left.timestamp < right.timestamp;
        });
        return resources;
    }

    [[nodiscard]] crawler::CapturedExchange buildExchange(
        String finalUrl, std::optional<String> title, std::string body,
        std::vector<crawler::CapturedResource> resources
    ) const
    {
        const auto state = data.Lock();
        crawler::CapturedExchange exchange{};
        exchange.seedUrl = state->seedNavigationUrl ? *state->seedNavigationUrl : finalUrl;
        exchange.pageId = state->pageId;
        exchange.finalUrl = std::move(finalUrl);
        applyMainResponse(*state, exchange, exchange.finalUrl);
        exchange.redirectChain = buildRedirectChainForExchange(*state, exchange.finalUrl);
        exchange.mainDocumentRedirects = state->mainDocumentRedirects;
        exchange.body = std::move(body);
        exchange.resources = std::move(resources);
        exchange.title = std::move(title);
        return exchange;
    }

private:
    struct [[nodiscard]] MainResponse {
        String requestUrl;
        i64 statusCode{0};
        String statusMessage;
        std::unordered_map<std::string, std::string> headers;
        String timestamp;
    };

    struct [[nodiscard]] Data final {
        String sessionId;
        String targetId;
        String pageId;
        std::unordered_map<String, TrackedRequest> activeRequests;
        std::vector<crawler::CapturedResource> redirectedResources;
        std::vector<String> redirectChain;
        std::vector<crawler::CapturedMainDocumentRedirect> mainDocumentRedirects;
        std::unordered_set<String> inflight;
        std::optional<String> mainRequestId;
        std::optional<String> mainResponseRequestId;
        std::optional<String> mainLoaderId;
        std::optional<String> mainFrameId;
        std::optional<String> seedNavigationUrl;
        std::optional<TrackedRequest> completedMainRequest;
        bool loaded{false};
        bool seedNavigationStarted{false};
        std::optional<String> mainRequestFailure;
        chrono::steady_clock::time_point lastNetworkAt{datetime::SteadyNow()};
    };

    static void applyMainResponse(
        const Data &state, crawler::CapturedExchange &exchange, const String &finalUrl
    )
    {
        const auto response = selectMainResponse(state, finalUrl);
        invariant(response, "missing main response while building exchange");
        exchange.statusCode = response->statusCode;
        exchange.statusMessage = response->statusMessage;
        exchange.headers = response->headers;
        exchange.timestamp = response->timestamp;
    }

    [[nodiscard]] static std::vector<String>
    buildRedirectChainForExchange(const Data &state, const String &finalUrl)
    {
        if (!state.redirectChain.empty())
            return state.redirectChain;
        return {finalUrl};
    }

    [[nodiscard]] static bool hasResponse(const TrackedRequest &request)
    {
        return request.statusCode && request.statusMessage && request.headers && request.timestamp;
    }

    [[nodiscard]] static MainResponse toMainResponse(const TrackedRequest &request)
    {
        invariant(
            request.statusCode && request.statusMessage && request.headers && request.timestamp,
            "tracked request missing response"
        );
        return {
            request.requestUrl, *request.statusCode, *request.statusMessage,
            *request.headers,   *request.timestamp,
        };
    }

    [[nodiscard]] static TrackedRequest *activeMainRequest(Data &state)
    {
        if (!state.mainRequestId)
            return nullptr;
        if (const auto it = state.activeRequests.find(*state.mainRequestId);
            it != std::end(state.activeRequests)) {
            return &it->second;
        }
        return nullptr;
    }

    [[nodiscard]] static const TrackedRequest *activeMainRequest(const Data &state)
    {
        if (!state.mainRequestId)
            return nullptr;
        if (const auto it = state.activeRequests.find(*state.mainRequestId);
            it != std::end(state.activeRequests)) {
            return &it->second;
        }
        return nullptr;
    }

    [[nodiscard]] static const TrackedRequest *resolvedMainRequest(const Data &state)
    {
        if (state.completedMainRequest && hasResponse(*state.completedMainRequest))
            return &*state.completedMainRequest;
        return activeMainRequest(state);
    }

    [[nodiscard]] static std::optional<MainResponse>
    selectMainResponse(const Data &state, const String &finalUrl)
    {
        if (state.completedMainRequest && hasResponse(*state.completedMainRequest)) {
            if (state.completedMainRequest->requestUrl == finalUrl)
                return toMainResponse(*state.completedMainRequest);
        }
        if (const auto *request = activeMainRequest(state);
            request != nullptr && hasResponse(*request)) {
            if (request->requestUrl == finalUrl)
                return toMainResponse(*request);
        }
        if (state.completedMainRequest && hasResponse(*state.completedMainRequest))
            return toMainResponse(*state.completedMainRequest);
        if (const auto *request = activeMainRequest(state);
            request != nullptr && hasResponse(*request)) {
            return toMainResponse(*request);
        }
        return {};
    }

    [[nodiscard]] static bool
    matchesTrackedMainLoader(const Data &state, const std::optional<std::string> &loaderId)
    {
        if (!state.mainLoaderId)
            return true;
        return loaderId && *loaderId == state.mainLoaderId->view();
    }

    [[nodiscard]] static bool isMainFrameDocumentRequest(
        const Data &state, const dto::NetworkRequestWillBeSentEvent &requestWillBeSent
    )
    {
        return requestWillBeSent.frameId && state.mainFrameId &&
               *requestWillBeSent.frameId == state.mainFrameId->view() && requestWillBeSent.type &&
               *requestWillBeSent.type == "Document";
    }

    static void
    handleRequestWillBeSent(Data &state, dto::NetworkRequestWillBeSentEvent requestWillBeSent)
    {
        if (requestWillBeSent.request.url.starts_with("data:"))
            return;

        const auto requestIdText = String::fromBytes(requestWillBeSent.requestId).expect();
        const auto rawRequestUrl = String::fromBytes(requestWillBeSent.request.url).expect();
        const auto requestMethod = String::fromBytes(requestWillBeSent.request.method).expect();

        state.inflight.insert(requestIdText);
        state.lastNetworkAt = datetime::SteadyNow();

        auto isTrackedMainDocument = false;
        if (isMainFrameDocumentRequest(state, requestWillBeSent)) {
            invariant(
                state.seedNavigationStarted,
                "main document request observed before seed navigation started"
            );
            if (state.mainLoaderId &&
                !matchesTrackedMainLoader(state, requestWillBeSent.loaderId)) {
                return;
            }
            if (!state.mainLoaderId && !state.mainRequestId && state.seedNavigationUrl) {
                const auto matchesExact = rawRequestUrl == *state.seedNavigationUrl;
                const auto matchesTrailingSlash =
                    !state.seedNavigationUrl->endsWith('/') &&
                    rawRequestUrl.sizeBytes() == state.seedNavigationUrl->sizeBytes() + 1 &&
                    rawRequestUrl.startsWith(*state.seedNavigationUrl) &&
                    rawRequestUrl.endsWith('/');
                if (!matchesExact && !matchesTrailingSlash)
                    return;
            }
            if (!state.mainLoaderId && requestWillBeSent.loaderId)
                state.mainLoaderId = stringOrNull(requestWillBeSent.loaderId);
            isTrackedMainDocument = true;
        }

        std::optional<String> previousRequestUrl;
        if (const auto it = state.activeRequests.find(requestIdText);
            it != std::end(state.activeRequests)) {
            previousRequestUrl = it->second.requestUrl;
        }
        if (requestWillBeSent.redirectResponse)
            finalizeRedirectRequest(state, requestIdText, requestWillBeSent.redirectResponse);

        const auto canonicalRequestUrl = previousRequestUrl
                                             ? resolveRedirectTargetUrl(
                                                   *previousRequestUrl, rawRequestUrl,
                                                   requestWillBeSent.redirectResponse
                                               )
                                             : canonicalizeCapturedUrl(rawRequestUrl);

        TrackedRequest trackedRequest{
            canonicalRequestUrl,
            requestMethod,
            {},
            {},
            {},
            {},
            stringOrNull(requestWillBeSent.loaderId),
            stringOrNull(requestWillBeSent.frameId),
            stringOrNull(requestWillBeSent.type),
            false,
            isTrackedMainDocument,
        };
        state.activeRequests.insert_or_assign(requestIdText, std::move(trackedRequest));

        if (isTrackedMainDocument) {
            state.mainRequestId = requestIdText;
            if (state.redirectChain.empty() || state.redirectChain.back() != canonicalRequestUrl)
                state.redirectChain.push_back(canonicalRequestUrl);
            return;
        }
    }

    static void
    handleResponseReceived(Data &state, dto::NetworkResponseReceivedEvent responseReceived)
    {
        const auto requestIdText = String::fromBytes(responseReceived.requestId).expect();
        const auto requestIt = state.activeRequests.find(requestIdText);
        if (requestIt == std::end(state.activeRequests)) {
            if (state.mainRequestId && *state.mainRequestId == requestIdText) {
                invariant(
                    false,
                    std::format(
                        "main document response received for unknown request id {}", requestIdText
                    )
                );
            }
            return;
        }

        const auto timestamp = currentTimestamp();
        auto &request = requestIt->second;
        request.statusCode = responseReceived.response.status
                                 ? i64(*responseReceived.response.status)
                                 : 0_i64;
        request.statusMessage =
            String::fromBytes(responseReceived.response.statusText.value_or("")).expect();
        request.headers = normalizeHeadersForCapture(
            responseReceived.response.headers, request.requestUrl
        );
        request.timestamp = timestamp;
        if (responseReceived.type)
            request.resourceType = stringOrNull(responseReceived.type);
        if (responseReceived.loaderId)
            request.loaderId = stringOrNull(responseReceived.loaderId);
        if (request.isTrackedMainDocument && hasResponse(request)) {
            state.completedMainRequest = request;
            state.mainResponseRequestId = requestIdText;
        }
    }

    static void handleLoadingFinished(Data &state, dto::NetworkLoadingFinishedEvent loadingFinished)
    {
        const auto requestIdText = String::fromBytes(loadingFinished.requestId).expect();
        state.inflight.erase(requestIdText);
        state.lastNetworkAt = datetime::SteadyNow();
        if (const auto it = state.activeRequests.find(requestIdText);
            it != std::end(state.activeRequests)) {
            it->second.loaded = true;
            if (it->second.isTrackedMainDocument && hasResponse(it->second)) {
                state.completedMainRequest = it->second;
                state.mainResponseRequestId = requestIdText;
            }
        } else if (state.mainRequestId && *state.mainRequestId == requestIdText) {
            invariant(
                false, std::format(
                           "main document loading finished for unknown request id {}", requestIdText
                       )
            );
        }
    }

    static void handleLoadingFailed(Data &state, dto::NetworkLoadingFailedEvent loadingFailed)
    {
        const auto requestIdText = String::fromBytes(loadingFailed.requestId).expect();
        state.inflight.erase(requestIdText);
        state.lastNetworkAt = datetime::SteadyNow();

        const auto requestIt = state.activeRequests.find(requestIdText);
        if (requestIt == std::end(state.activeRequests)) {
            if (state.mainRequestId && *state.mainRequestId == requestIdText) {
                invariant(
                    false,
                    std::format(
                        "main document loading failed for unknown request id {}", requestIdText
                    )
                );
            }
            return;
        }

        auto &request = requestIt->second;
        request.loaded = true;
        if (!request.isTrackedMainDocument)
            return;
        if (hasResponse(request))
            return;

        state.mainRequestFailure =
            String::fromBytes(loadingFailed.errorText.value_or("main document request failed"))
                .expect();
    }

    static void finalizeRedirectRequest(
        Data &state, const String &requestId,
        const std::optional<dto::NetworkResponse> &redirectResponse
    )
    {
        invariant(
            redirectResponse && redirectResponse->status, "redirect response must include status"
        );

        const auto requestIt = state.activeRequests.find(requestId);
        invariant(
            requestIt != std::end(state.activeRequests),
            std::format("redirect response for unknown request id {}", requestId)
        );

        auto request = std::move(requestIt->second);
        state.activeRequests.erase(requestIt);

        request.statusCode = i64(*redirectResponse->status);
        request.statusMessage =
            String::fromBytes(redirectResponse->statusText.value_or("")).expect();
        request.headers = normalizeHeadersForCapture(redirectResponse->headers, request.requestUrl);
        request.timestamp = currentTimestamp();
        request.loaded = true;

        if (request.isTrackedMainDocument) {
            recordMainDocumentRedirect(state, request);
            if (state.mainRequestId && *state.mainRequestId == requestId)
                state.mainRequestId.reset();
            return;
        }

        recordResourceRedirect(state, request);
    }

    static void recordMainDocumentRedirect(Data &state, const TrackedRequest &request)
    {
        invariant(
            hasResponse(request),
            std::format("main redirect request missing response fields for {}", request.requestUrl)
        );

        crawler::CapturedMainDocumentRedirect redirect{
            .redirectUrl = request.requestUrl,
            .statusCode = *request.statusCode,
            .statusMessage = *request.statusMessage,
            .headers = *request.headers,
            .timestamp = *request.timestamp,
        };

        if (!state.mainDocumentRedirects.empty()) {
            const auto &previous = state.mainDocumentRedirects.back();
            if (previous.redirectUrl == redirect.redirectUrl &&
                previous.statusCode == redirect.statusCode) {
                return;
            }
        }
        state.mainDocumentRedirects.push_back(std::move(redirect));
    }

    static void recordResourceRedirect(Data &state, const TrackedRequest &request)
    {
        invariant(
            hasResponse(request),
            std::format(
                "resource redirect request missing response fields for {}", request.requestUrl
            )
        );

        state.redirectedResources.push_back({
            request.requestUrl,
            request.method,
            request.resourceType,
            *request.statusCode,
            *request.statusMessage,
            *request.headers,
            {},
            *request.timestamp,
        });
    }

    us::concurrent::Variable<Data> data;
};

struct [[nodiscard]] DomState {
    String finalUrl;
    std::optional<String> title;
    std::string html;
};

[[nodiscard]] Expected<DomState, String> readDomState(crawler::CdpSession &cdpSession)
{
    dto::RuntimeEvaluateParams params{
        .expression =
            "(() => ({ finalUrl: location.href, title: document.title || undefined, html: "
            "document.documentElement ? document.documentElement.outerHTML : \"\" }))()",
        .returnByValue = true,
        .awaitPromise = false,
    };

    const auto result = TRY_MAP_ERR(
        cdpSession.send<dto::RuntimeEvaluateDomStateResult>("Runtime.evaluate"_t, params),
        [](auto failure) {
            return describeCdpFailure("failed to read dom state"_t, std::move(failure));
        }
    );
    const auto &value = result.result.value;
    auto title = value.title.transform([](const auto &t) -> std::optional<String> {
        return TRY(String::fromBytes(t));
    });
    const auto finalUrl = TRY_ERR_AS(
        String::fromBytes(value.finalUrl).transform([](String url) {
            return canonicalizeCapturedUrl(url);
        }),
        "Runtime.evaluate returned invalid finalUrl"_t
    );
    return DomState{
        .finalUrl = finalUrl,
        .title = title.value_or(std::nullopt),
        .html = value.html,
    };
}

Expected<void, String> runSiteBehavior(crawler::CdpSession &cdpSession, eng::Deadline deadline)
{
    invariant(deadline.IsReachable(), "site behavior deadline must be reachable");
    const auto budget = TRY_ERR_AS(timeLeftMs(deadline), "timed out running site behavior"_t);

    dto::RuntimeEvaluateParams params;
    params.expression = std::format(
        "(() => new Promise((resolve) => {{ const startedAt = Date.now(); const stepDelayMs = "
        "100; const maxSteps = Math.max(1, Math.floor({0} / stepDelayMs)); let steps = 0; const "
        "tick = () => {{ const root = document.scrollingElement || document.documentElement || "
        "document.body; if (!root) {{ resolve(true); return; }} const previous = root.scrollTop; "
        "root.scrollBy(0, Math.max(window.innerHeight, 600)); steps++; const exhausted = "
        "Date.now() - startedAt >= {0} || steps >= maxSteps; const stuck = root.scrollTop === "
        "previous; if (stuck || exhausted) {{ root.scrollTo(0, 0); resolve(true); return; }} "
        "setTimeout(tick, stepDelayMs); }}; tick(); }}))()",
        budget.count()
    );
    params.awaitPromise = true;
    params.returnByValue = true;
    TRY_MAP_ERR(cdpSession.send<json::Value>("Runtime.evaluate"_t, params), [](auto failure) {
        return describeCdpFailure("failed to run site behavior"_t, std::move(failure));
    });
    return {};
}

class [[nodiscard]] CaptureSession final {
public:
    CaptureSession(
        Denylist &denylist, const Config &config, dns::Resolver &dnsResolver, usize urlBytesMax,
        i64 proxyDownBytesMax, eng::subprocess::ProcessStarter &processStarter,
        std::string browserRunsRootIn, std::string cgroupRootPathIn,
        std::optional<crawler::CgroupLimits> cgroupLimitsIn, crawler::CaptureTimings timings,
        crawler::CrawlerTunables tunablesIn, i64 maxArchiveBytesIn, eng::Deadline deadline,
        crawler::RunRequest run
    )
        : denylist(denylist), config(config), timings(std::move(timings)), run(std::move(run)),
          deadline(deadline), maxArchiveBytes(maxArchiveBytesIn),
          browser(
              dnsResolver, processStarter,
              crawler::BrowserSessionConfig{
                  .urlBytesMax = urlBytesMax,
                  .proxyDownBytesMax = proxyDownBytesMax,
                  .browserRunsRoot = std::move(browserRunsRootIn),
                  .cgroupRootPath = std::move(cgroupRootPathIn),
                  .cgroupLimits = std::move(cgroupLimitsIn),
                  .localFixtureTrustDbSourcePath =
                      crawler::localFixtureTrustDbSourcePath(config.stateDir()),
                  .devtoolsStartupTimeout = tunablesIn.devtoolsStartupTimeout,
                  .cdpHandshakeTimeout = tunablesIn.cdpHandshakeTimeout,
                  .cdpCommandTimeout = tunablesIn.cdpCommandTimeout,
                  .devtoolsPollInterval = tunablesIn.devtoolsPollInterval,
                  .browserStopTimeout = tunablesIn.browserStopTimeout,
                  .cdpMaxRemotePayloadBytes = computeCdpMaxRemotePayloadBytes(maxArchiveBytesIn),
                  .proxyRequireAuth = true,
                  .enableLocalFixtureRewrite = tunablesIn.enableLocalFixtureRewrite,
                  .cgroupNamePrefix = "webshotd_crawler",
              }
          )
    {
    }

    [[nodiscard]] Expected<CaptureWithNetwork, CaptureFailure> capture()
    {
        auto launched = launch();
        if (!launched) {
            auto failureDetail = browser.buildFailureDetail(launched.error());
            closeCdpForFailure();
            browser.close();
            return Unex(CaptureFailure{std::move(failureDetail), {}});
        }

        auto captured = captureAttachedTarget();
        if (!captured) {
            auto failureDetail = browser.buildFailureDetail(captured.error());
            if (tracker) {
                const auto trackerFailure = tracker->failureReason();
                if (trackerFailure)
                    failureDetail = text::format(
                        "{}, tracker_failure={}", failureDetail, *trackerFailure
                    );
            }
            if (const auto proxyFailure = browser.proxyFailureReason())
                failureDetail = text::format("{}, proxy_failure={}", failureDetail, *proxyFailure);
            auto seedProbe = currentSeedProbe();
            closeCdpForFailure();
            browser.close();
            return Unex(
                CaptureFailure{
                    std::move(failureDetail),
                    std::move(seedProbe),
                }
            );
        }
        auto value = grabValueOf(captured);
        return CaptureWithNetwork{
            .exchange = std::move(value),
            .proxyDownBytes = browser.proxyDownBytes(),
        };
    }

private:
    template <typename... Args>
    [[nodiscard]] Expected<void, String> sendSessionVoid(const String &method, Args &&...args) const
    {
        auto result = cdpSession().sendVoid(method, std::forward<Args>(args)...);
        if (!result)
            return Unex(describeCdpFailure(text::format("{} failed", method), result.error()));
        return {};
    }

    void noteEventProgress()
    {
        auto progress = eventProgress.Lock();
        progress->version++;
        progress->cv.NotifyAll();
    }

    [[nodiscard]] std::optional<String> currentWaitFailure() const
    {
        {
            const auto failure = interceptionFailure.Lock();
            if (*failure)
                return *failure;
        }
        if (tracker) {
            if (const auto trackerFailure = tracker->failureReason())
                return trackerFailure;
        }
        return {};
    }

    template <typename Predicate>
    [[nodiscard]] Expected<void, String>
    waitForPredicate(Predicate &&predicate, String timeoutMessage)
    {
        while (!std::invoke(predicate)) {
            if (const auto failure = currentWaitFailure())
                return Unex(*failure);
            auto progress = eventProgress.UniqueLock();
            const auto version = progress->version;
            progress.GetLock().unlock();
            if (std::invoke(predicate))
                return {};
            if (const auto failure = currentWaitFailure())
                return Unex(*failure);
            progress.GetLock().lock();
            if (progress->version != version)
                continue;
            if (!progress->cv.WaitUntil(progress.GetLock(), deadline, [&]() {
                    return progress->version != version;
                })) {
                progress.GetLock().unlock();
                if (std::invoke(predicate))
                    return {};
                if (const auto failure = currentWaitFailure())
                    return Unex(*failure);
                return Unex(timeoutMessage);
            }
        }
        if (const auto failure = currentWaitFailure())
            return Unex(*failure);
        return {};
    }

    [[nodiscard]] Expected<void, String> waitForIdle(chrono::seconds idle)
    {
        while (!pageTracker().isIdleFor(idle)) {
            if (const auto failure = currentWaitFailure())
                return Unex(*failure);
            auto progress = eventProgress.UniqueLock();
            const auto version = progress->version;
            const auto idleDeadline = eng::Deadline::FromTimePoint(
                pageTracker().idleDeadline(idle)
            );
            const auto waitDeadline = pickEarlierDeadline(deadline, idleDeadline);
            progress.GetLock().unlock();
            if (pageTracker().isIdleFor(idle))
                return {};
            if (const auto failure = currentWaitFailure())
                return Unex(*failure);
            progress.GetLock().lock();
            if (progress->version != version)
                continue;
            if (!progress->cv.WaitUntil(progress.GetLock(), waitDeadline, [&]() {
                    return progress->version != version;
                })) {
                progress.GetLock().unlock();
                if (pageTracker().isIdleFor(idle))
                    return {};
                if (const auto failure = currentWaitFailure())
                    return Unex(*failure);
                if (deadline.IsReached())
                    return Unex("timed out waiting for network idle"_t);
            }
        }
        if (const auto failure = currentWaitFailure())
            return Unex(*failure);
        return {};
    }

    void runEventLoop()
    {
        while (!stoppingEventLoop.load()) {
            auto event = cdpSession().waitEvent(deadline, "timed out waiting for cdp event"_t);
            if (!event) {
                if (!stoppingEventLoop.load()) {
                    noteInterceptionFailure(
                        describeCdpFailure("cdp event loop failed"_t, event.error())
                    );
                    noteEventProgress();
                }
                return;
            }
            handleSessionEvent(*event);
            noteEventProgress();
            if (currentWaitFailure())
                return;
        }
    }

    void startEventLoop()
    {
        stoppingEventLoop.store(false);
        eventTask = std::move(eng::CriticalAsyncNoSpan([this]() { runEventLoop(); })).AsTask();
    }

    void stopEventLoop()
    {
        stoppingEventLoop.store(true);
        if (!eventTask.IsValid())
            return;
        eventTask.RequestCancel();
        const eng::TaskCancellationBlocker blocker;
        static_cast<void>(eventTask.WaitNothrow());
        eventTask = {};
    }

    [[nodiscard]] Expected<void, String> launch()
    {
        TRY(browser.launch());
        browser.markPhase("connect_cdp");
        cdp = TRY(browser.connectCdp(deadline));
        pageSession = std::make_unique<crawler::BrowserPageSession>(cdpClient());

        TRY(pageSession->attachFreshTarget([this](std::string_view phase) {
            browser.markPhase(phase);
        }));
        tracker = std::make_unique<PageTracker>(pageSession->sessionId(), pageSession->targetId());
        startEventLoop();
        return {};
    }

    [[nodiscard]] Expected<crawler::CapturedExchange, String> captureAttachedTarget()
    {
        TRY(pageSession->enableBaseDomains([this](std::string_view phase) {
            browser.markPhase(phase);
        }));

        browser.markPhase("enable_fetch");
        dto::FetchEnableParams fetchParams;
        fetchParams.handleAuthRequests = true;
        TRY(sendSessionVoid("Fetch.enable"_t, fetchParams));

        browser.markPhase("disable_cache");
        dto::NetworkSetCacheDisabledParams cacheParams;
        cacheParams.cacheDisabled = true;
        TRY(sendSessionVoid("Network.setCacheDisabled"_t, cacheParams));

        browser.markPhase("bypass_service_worker");
        dto::NetworkSetBypassServiceWorkerParams serviceWorkerParams;
        serviceWorkerParams.bypass = true;
        TRY(sendSessionVoid("Network.setBypassServiceWorker"_t, serviceWorkerParams));

        browser.markPhase("set_extra_headers");
        dto::NetworkSetExtraHTTPHeadersParams headerParams;
        headerParams.headers.extra.emplace(
            "Accept-Language", std::string(crawler::kBrowserAcceptLanguage)
        );
        TRY(sendSessionVoid("Network.setExtraHTTPHeaders"_t, headerParams));

        browser.markPhase("get_frame_tree");
        const auto frameTree = TRY_MAP_ERR(
            cdpSession().send<dto::PageGetFrameTreeResult>("Page.getFrameTree"_t),
            [](auto failure) {
                return describeCdpFailure("Page.getFrameTree failed"_t, std::move(failure));
            }
        );
        pageTracker().setMainFrameId(String::fromBytes(frameTree.frameTree.frame.id).expect());

        browser.markPhase("navigate");
        dto::PageNavigateParams navigateParams;
        navigateParams.url = std::to_string(run.seedUrl);
        pageTracker().beginSeedNavigation(run.seedUrl);
        const auto navigateResult = TRY_MAP_ERR(
            cdpSession().send<dto::PageNavigateResult>("Page.navigate"_t, navigateParams),
            [](auto failure) {
                return describeCdpFailure("Page.navigate failed"_t, std::move(failure));
            }
        );
        ENSURE(!navigateResult.errorText, String::fromBytes(*navigateResult.errorText).expect());
        pageTracker().setExpectedMainLoaderId(stringOrNull(navigateResult.loaderId));

        browser.markPhase("wait_for_load");
        TRY(waitForPredicate(
            [this]() { return pageTracker().isLoadedOrFailed(); },
            "timed out waiting for page load"_t
        ));
        if (timings.postLoadDelay > chrono::seconds::zero()) {
            browser.markPhase("post_load_delay");
            const auto phaseDeadline = pickEarlierDeadline(
                deadline, eng::Deadline::FromDuration(timings.postLoadDelay)
            );
            TRY_ERR_AS(
                sleepUntilDeadline(phaseDeadline), "timed out waiting for post-load delay"_t
            );
            browser.markPhase("post_load_delay_done");
        }
        if (timings.behaviorTimeout > chrono::seconds::zero()) {
            browser.markPhase("run_site_behavior");
            browser.markPhase("run_site_behavior_runtime_evaluate");
            const auto behaviorDeadline = pickEarlierDeadline(
                deadline, eng::Deadline::FromDuration(timings.behaviorTimeout)
            );
            TRY(runSiteBehavior(cdpSession(), behaviorDeadline));
            browser.markPhase("run_site_behavior_done");
        }
        if (timings.netIdleWait > chrono::seconds::zero()) {
            browser.markPhase("wait_for_idle");
            browser.markPhase("wait_for_idle_wait");
            TRY(waitForIdle(timings.netIdleWait));
            browser.markPhase("wait_for_idle_done");
        }
        if (timings.pageExtraDelay > chrono::seconds::zero()) {
            browser.markPhase("page_extra_delay");
            const auto phaseDeadline = pickEarlierDeadline(
                deadline, eng::Deadline::FromDuration(timings.pageExtraDelay)
            );
            TRY_ERR_AS(
                sleepUntilDeadline(phaseDeadline), "timed out waiting for extra page delay"_t
            );
            browser.markPhase("page_extra_delay_done");
        }
        browser.markPhase("wait_for_main_document");
        browser.markPhase("wait_for_main_document_wait");
        TRY(waitForPredicate(
            [this]() { return pageTracker().hasMainDocumentOrFailure(); },
            "timed out waiting for main document response"_t
        ));
        browser.markPhase("wait_for_main_document_done");

        browser.markPhase("read_dom_state");
        browser.markPhase("read_dom_state_runtime_evaluate");
        auto domState = TRY(readDomState(cdpSession()));
        browser.markPhase("read_dom_state_done");
        RetainedBodyBudget budget{maxArchiveBytes, 0_i64};
        browser.markPhase("read_main_body");
        auto body = TRY(pageTracker().readBody(cdpSession(), budget, domState.html));
        browser.markPhase("read_resources");
        auto resources = TRY(pageTracker().readResources(cdpSession(), budget));

        stopEventLoop();
        TRY(pageSession->close([this](std::string_view phase) { browser.markPhase(phase); }));
        pageSession.reset();

        browser.markPhase("build_exchange_start");
        LOG_INFO() << std::format(
            "captureViaProxy building exchange for {} (body_bytes={}, resources={})", run.seedUrl,
            body.size(), resources.size()
        );
        auto exchange = pageTracker().buildExchange(
            std::move(domState.finalUrl), std::move(domState.title), std::move(body),
            std::move(resources)
        );
        browser.markPhase("build_exchange_done");
        LOG_INFO() << std::format(
            "captureViaProxy built exchange for {} (status={}, resources={}, body_bytes={})",
            run.seedUrl, exchange.statusCode, exchange.resources.size(), exchange.body.size()
        );
        tracker.reset();
        cdp.reset();

        browser.markPhase("close_browser_success");
        browser.markPhase("before_browser_close");
        LOG_INFO() << std::format("captureViaProxy closing browser for {}", run.seedUrl);
        browser.close();
        if (const auto proxyFailure = browser.proxyFailureReason())
            return Unex(*proxyFailure);
        LOG_INFO() << std::format("captureViaProxy returning capture for {}", run.seedUrl);
        return exchange;
    }

    void closeCdpForFailure()
    {
        stopEventLoop();
        if (pageSession) {
            if (const auto closedPage = pageSession->close(); !closedPage) {
                LOG_WARNING() << std::format(
                    "Suppressing page session close failure during capture cleanup: {}",
                    closedPage.error()
                );
            }
            pageSession.reset();
        }
        if (!cdp)
            return;

        if (auto closed = cdp->close(); !closed) {
            LOG_WARNING() << std::format(
                "Suppressing CDP close failure during capture cleanup: code={}{}",
                numericCast<int>(closed.error().code),
                closed.error().detail ? std::format(", detail={}", *closed.error().detail)
                                      : std::string{}
            );
        }
        cdp.reset();
    }

    [[nodiscard]] std::optional<crawler::SeedPageProbe> currentSeedProbe() const
    {
        if (!tracker)
            return {};
        return tracker->currentSeedProbe();
    }

    [[nodiscard]] crawler::CdpClient &cdpClient() const
    {
        invariant(cdp, "cdp client is not connected");
        return *cdp;
    }

    [[nodiscard]] PageTracker &pageTracker() const
    {
        invariant(tracker, "page tracker is not attached");
        return *tracker;
    }

    [[nodiscard]] crawler::CdpSession &cdpSession() const
    {
        invariant(pageSession, "cdp session is not attached");
        return pageSession->cdpSession();
    }

    void noteInterceptionFailure(String reason)
    {
        auto trackerReason = reason;
        {
            auto failure = interceptionFailure.UniqueLock();
            if (*failure)
                return;
            *failure = std::move(reason);
        }
        if (tracker)
            tracker->fail(std::move(trackerReason));
    }

    void handleSessionEvent(const crawler::CdpEvent &event)
    {
        if (event.method == "Fetch.authRequired"_t) {
            handleFetchAuthRequired(event);
            return;
        }
        if (event.method == "Fetch.requestPaused"_t) {
            handleFetchRequestPaused(event);
            return;
        }
        if (tracker)
            tracker->handleEvent(event);
    }

    void handleFetchAuthRequired(const crawler::CdpEvent &event)
    {
        const auto authRequired = parseEventParams<dto::FetchAuthRequiredEvent>(event);
        if (!authRequired) {
            noteInterceptionFailure(authRequired.error());
            return;
        }

        dto::FetchContinueWithAuthParams params;
        params.requestId = authRequired->requestId;

        dto::FetchAuthChallengeResponse authChallengeResponse;
        const auto isProxyChallenge = !authRequired->authChallenge.source ||
                                      *authRequired->authChallenge.source == "Proxy";
        if (isProxyChallenge) {
            authChallengeResponse.response = "ProvideCredentials";
            authChallengeResponse.username = browser.runId();
            authChallengeResponse.password = "x";
        } else {
            authChallengeResponse.response = "Default";
        }
        params.authChallengeResponse = std::move(authChallengeResponse);

        const auto continued = cdpSession().sendVoid("Fetch.continueWithAuth"_t, params);
        if (!continued)
            noteInterceptionFailure(
                describeCdpFailure("Fetch.continueWithAuth failed"_t, continued.error())
            );
    }

    void handleFetchRequestPaused(const crawler::CdpEvent &event)
    {
        const auto paused = parseEventParams<dto::FetchRequestPausedEvent>(event);
        if (!paused) {
            noteInterceptionFailure(paused.error());
            return;
        }

        const auto requestUrl = String::fromBytes(paused->request.url);
        if (!requestUrl) {
            noteInterceptionFailure("Fetch.requestPaused contained invalid request url"_t);
            return;
        }

        const auto allowed = isAllowedByDenylist(denylist, config, *requestUrl);
        if (!allowed) {
            noteInterceptionFailure(allowed.error());
            return;
        }

        if (*allowed) {
            dto::FetchContinueRequestParams params;
            params.requestId = paused->requestId;
            const auto continued = cdpSession().sendVoid("Fetch.continueRequest"_t, params);
            if (!continued)
                noteInterceptionFailure(
                    describeCdpFailure("Fetch.continueRequest failed"_t, continued.error())
                );
            return;
        }

        static constexpr std::string_view kBody = "Blocked by webshot denylist\n";
        dto::FetchFulfillRequestParams params{
            .requestId = paused->requestId,
            .responseCode = 403,
            .responseHeaders = buildBlockedFetchHeaders(kBody.size()),
            .body = us::crypto::base64::Base64Encode(kBody),
            .responsePhrase = "Forbidden",
        };
        const auto fulfilled = cdpSession().sendVoid("Fetch.fulfillRequest"_t, params);
        if (!fulfilled)
            noteInterceptionFailure(
                describeCdpFailure("Fetch.fulfillRequest failed"_t, fulfilled.error())
            );
    }

    struct EventProgressState final {
        eng::ConditionVariable cv;
        i64 version{0};
    };

    Denylist &denylist;
    const Config &config;
    crawler::CaptureTimings timings;
    crawler::RunRequest run;
    eng::Deadline deadline;
    i64 maxArchiveBytes;
    crawler::BrowserSession browser;
    std::unique_ptr<crawler::CdpClient> cdp;
    std::unique_ptr<PageTracker> tracker;
    std::unique_ptr<crawler::BrowserPageSession> pageSession;
    us::concurrent::Variable<EventProgressState> eventProgress;
    eng::Task eventTask;
    std::atomic<bool> stoppingEventLoop{false};
    us::concurrent::Variable<std::optional<String>> interceptionFailure;
};

[[nodiscard]] Expected<CaptureWithNetwork, CaptureFailure> captureViaProxy(
    Denylist &denylist, const Config &config, dns::Resolver &dnsResolver, usize urlBytesMax,
    i64 proxyDownBytesMax, eng::subprocess::ProcessStarter &processStarter,
    const std::string &browserRunsRoot, const std::string &cgroupRootPath,
    std::optional<crawler::CgroupLimits> cgroupLimits, crawler::CaptureTimings timings,
    const crawler::CrawlerTunables &tunables, i64 maxArchiveBytes, eng::Deadline deadline,
    const crawler::RunRequest &run
)
{
    auto session = CaptureSession(
        denylist, config, dnsResolver, urlBytesMax, proxyDownBytesMax, processStarter,
        std::string(browserRunsRoot), std::string(cgroupRootPath), std::move(cgroupLimits),
        std::move(timings), tunables, maxArchiveBytes, deadline,
        crawler::RunRequest{.seedUrl = run.seedUrl}
    );
    return session.capture();
}

[[nodiscard]] CrawlerRunArtifacts executeRun(
    Denylist &denylist, const Config &config, dns::Resolver &dnsResolver,
    eng::subprocess::ProcessStarter &processStarter, const std::string &browserRunsRoot,
    const std::string &cgroupRootPath, std::optional<crawler::CgroupLimits> cgroupLimits,
    const crawler::CaptureTimings &timings, const crawler::CrawlerTunables &tunables,
    i64 maxArchiveBytes, i64 networkDownBytesRatioMax, eng::Deadline deadline,
    const crawler::RunRequest &run
)
{
    CrawlerRunArtifacts out;
    out.attempt.exited = true;
    try {
        LOG_INFO() << std::format("crawler executeRun starting for {}", run.seedUrl);

        const auto failArtifact =
            [&out](const crawler::ArtifactFailure &failure) -> CrawlerRunArtifacts {
            auto failureDetailOpt = String::fromBytes(failure.detail)
                                        .transform([](String s) -> std::optional<String> {
                                            return {std::move(s)};
                                        })
                                        .valueOr(std::nullopt);
            out.attempt.exitCode = 9;
            out.attempt.waczExists = false;
            out.attempt.seedProbe.reset();
            out.attempt.failureDetail = std::move(failureDetailOpt);
            out.stdoutLog.clear();
            out.stderrLog = failure.detail + "\n";
            out.wacz.reset();
            out.pagesJsonl.reset();
            out.contentSha256.reset();
            return out;
        };

        const auto maxDownBytes = [&]() -> i64 {
            const auto max = maxArchiveBytes;
            const auto ratio = networkDownBytesRatioMax;
            const auto maxI64 = std::numeric_limits<i64>::max();
            if (ratio > maxI64 / max)
                return maxI64;
            return ratio * max;
        }();

        auto captured = captureViaProxy(
            denylist, config, dnsResolver, config.urlBytesMax(), maxDownBytes, processStarter,
            browserRunsRoot, cgroupRootPath, std::move(cgroupLimits), timings, tunables,
            maxArchiveBytes, deadline, run
        );
        if (!captured) {
            constexpr std::string_view kSizeLimitPrefix = "size_limit:";
            constexpr std::string_view kNetLimitPrefix = "net_limit:";
            if (captured.error().detail.startsWith(kSizeLimitPrefix)) {
                auto detailText = captured.error().detail.view();
                detailText.remove_prefix(kSizeLimitPrefix.size());
                if (!detailText.empty() && detailText.front() == ' ')
                    detailText.remove_prefix(1);
                auto parsed = String::fromBytes(std::string(detailText));
                out.attempt.exitCode = us::utils::UnderlyingValue(
                    crawler::CrawlerExitCode::kSizeLimit
                );
                out.attempt.waczExists = false;
                out.attempt.seedProbe = captured.error().seedProbe;
                out.attempt.failureDetail.reset();
                if (parsed)
                    out.attempt.failureDetail = grabValueOf(parsed);
            } else if (captured.error().detail.startsWith(kNetLimitPrefix)) {
                out.attempt.exitCode = us::utils::UnderlyingValue(
                    crawler::CrawlerExitCode::kFailedLimit
                );
                out.attempt.waczExists = false;
                out.attempt.seedProbe = captured.error().seedProbe;
                out.attempt.failureDetail = captured.error().detail;
            } else {
                out.attempt.exitCode = 9;
                out.attempt.waczExists = false;
                out.attempt.seedProbe = captured.error().seedProbe;
                out.attempt.failureDetail = captured.error().detail;
            }
            out.stdoutLog.clear();
            out.stderrLog = std::to_string(captured.error().detail) + "\n";
            out.wacz.reset();
            out.pagesJsonl.reset();
            out.contentSha256.reset();
            out.replayUrl.reset();
            return out;
        }

        auto exchange = std::move(captured->exchange);
        const auto proxyDownBytes = captured->proxyDownBytes;
        LOG_INFO() << std::format(
            "crawler captureViaProxy finished for {} with status={}", run.seedUrl,
            exchange.statusCode
        );
        auto pages = crawler::buildPagesJsonl(exchange);
        LOG_INFO() << std::format("crawler buildPagesJsonl finished for {}", run.seedUrl);
        out.contentSha256 = crawler::computeContentSha256(exchange);
        {
            auto log = crawler::buildSuccessStdoutLog(
                run, exchange, 0_i64, crawler::ReusedBrowser::kNo
            );
            if (!log)
                invariant(false, log.error().detail);
            out.stdoutLog = grabValueOf(log);
        }
        out.stderrLog.clear();
        auto warc = crawler::buildWarc(exchange);
        if (!warc)
            return failArtifact(warc.error());
        LOG_INFO() << std::format("crawler buildWarc finished for {}", run.seedUrl);
        auto wacz = crawler::buildWacz(run, pages, grabValueOf(warc), out.stdoutLog, out.stderrLog);
        if (!wacz)
            return failArtifact(wacz.error());
        LOG_INFO() << std::format(
            "crawler buildWacz finished for {} (wacz_bytes={}, pages_bytes={})", run.seedUrl,
            wacz->size(), pages.size()
        );

        const auto waczBytes = i64(wacz->size());
        if (waczBytes > maxArchiveBytes) {
            const auto maxArchiveMiB = maxArchiveBytes / (1024_i64 * 1024_i64);
            const auto detail = text::format(
                "archive bytes {} exceeded size limit {} MiB", waczBytes, maxArchiveMiB
            );
            out.attempt.exitCode = us::utils::UnderlyingValue(crawler::CrawlerExitCode::kSizeLimit);
            out.attempt.waczExists = false;
            out.attempt.seedProbe = crawler::SeedPageProbe{
                .status = raw(exchange.statusCode),
                .loadState = raw(0_i64),
            };
            out.attempt.failureDetail = detail;
            out.wacz.reset();
            out.pagesJsonl.reset();
            out.contentSha256.reset();
            out.replayUrl.reset();
            out.stderrLog += std::to_string(detail) + "\n";
            return out;
        }

        const auto maxDownByFinal = [&]() -> i64 {
            const auto ratio = networkDownBytesRatioMax;
            const auto maxI64 = std::numeric_limits<i64>::max();
            if (waczBytes <= 0_i64)
                return 0_i64;
            if (ratio > maxI64 / waczBytes)
                return maxI64;
            return ratio * waczBytes;
        }();
        if (proxyDownBytes > maxDownByFinal) {
            const auto detail = text::format(
                "net_limit: proxy downstream bytes {} exceeded post-run limit {}", proxyDownBytes,
                maxDownByFinal
            );
            out.attempt.exitCode = us::utils::UnderlyingValue(
                crawler::CrawlerExitCode::kFailedLimit
            );
            out.attempt.waczExists = false;
            out.attempt.seedProbe = crawler::SeedPageProbe{
                .status = raw(exchange.statusCode),
                .loadState = raw(0_i64),
            };
            out.attempt.failureDetail = detail;
            out.wacz.reset();
            out.pagesJsonl.reset();
            out.contentSha256.reset();
            out.replayUrl.reset();
            out.stderrLog += std::to_string(detail) + "\n";
            return out;
        }

        const i64 exitCode{exchange.statusCode >= 400_i64 ? 9_i64 : 0_i64};
        const i64 loadState{exitCode != 0_i64 || exchange.statusCode >= 400_i64 ? 0_i64 : 2_i64};
        if (exchange.statusCode >= 400_i64) {
            out.attempt.failureDetail = text::format("seed returned HTTP {}", exchange.statusCode);
        }

        LOG_INFO() << std::format(
            "crawler executeRun finished for {} (exit_code={}, wacz_exists=true)", run.seedUrl,
            exitCode
        );

        out.attempt.exitCode = numericCast<int>(exitCode);
        out.attempt.waczExists = true;
        out.attempt.seedProbe = crawler::SeedPageProbe{
            .status = raw(exchange.statusCode),
            .loadState = raw(loadState),
        };
        out.wacz = grabValueOf(wacz);
        out.pagesJsonl = std::move(pages);
        out.replayUrl = exchange.finalUrl;
        return out;
    } catch (const us::utils::TracefulException &e) {
        out.attempt.exitCode = 9;
        out.attempt.waczExists = false;
        out.attempt.seedProbe.reset();
        {
            auto parsed = String::fromBytes(e.what());
            if (parsed)
                out.attempt.failureDetail = grabValueOf(parsed);
            else
                out.attempt.failureDetail.reset();
        }
        out.stdoutLog.clear();
        out.stderrLog = std::string(e.what()) + "\n";
        out.wacz.reset();
        out.pagesJsonl.reset();
        out.contentSha256.reset();
        out.replayUrl.reset();
        return out;
    }
}

} // namespace

CrawlerRunner::CrawlerRunner(
    Denylist &denylist, const Config &config, dns::Resolver &dnsResolver,
    eng::subprocess::ProcessStarter &processStarter, chrono::seconds runTimeout,
    std::string stateDir, std::optional<crawler::CgroupLimits> limits, i64 maxArchiveBytes,
    crawler::CaptureTimings timings, crawler::CrawlerTunables tunables, i64 networkDownBytesRatioMax
)
    : denylist(denylist), config(config), dnsResolver(dnsResolver), processStarter(processStarter),
      runTimeout(runTimeout), browserRunsRoot(crawler::buildBrowserRunsRoot(std::move(stateDir))),
      cgroupRootPath(limits ? crawler::resolveDelegatedCgroupRootPath() : std::string()),
      cgroupLimits(std::move(limits)), maxArchiveBytes(maxArchiveBytes),
      timings(std::move(timings)), tunables(std::move(tunables)),
      networkDownBytesRatioMax(networkDownBytesRatioMax)
{
}

CrawlerRunArtifacts CrawlerRunner::run(const String &seedUrl) const
{
    const auto deadline = eng::Deadline::FromDuration(runTimeout);
    return executeRun(
        denylist, config, dnsResolver, processStarter, browserRunsRoot, cgroupRootPath,
        cgroupLimits, timings, tunables, maxArchiveBytes, networkDownBytesRatioMax, deadline,
        crawler::RunRequest{.seedUrl = seedUrl}
    );
}

} // namespace v1
