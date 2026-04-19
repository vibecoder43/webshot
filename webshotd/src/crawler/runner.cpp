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
#include "link.hpp"
#include "prefix_utils.hpp"
#include "schema/cdp.hpp"
#include "text.hpp"
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
    return String::fromBytes(
               us::utils::datetime::UtcTimestring(
                   us::utils::datetime::Now(), us::utils::datetime::kRfc3339Format
               )
    )
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

    auto parsed = maybeUrl->copyParsed();
    parsed.clear_port();
    return Url::fromParsed(std::move(parsed)).href();
}

[[nodiscard]] std::string
canonicalizeCapturedLocationHeader(const String &responseUrl, std::string_view locationValue)
{
    auto location = String::fromBytes(locationValue);
    if (!location)
        return std::string(locationValue);
    if (location->empty() || location->startsWith('/') || location->startsWith('?') ||
        location->startsWith("//")) {
        return std::string(location->view());
    }

    const auto canonicalLocation = canonicalizeCapturedUrl(*location);
    const auto maybeCanonicalUrl = Url::fromText(canonicalLocation);
    const auto maybeResponseUrl = Url::fromText(responseUrl);
    if (!maybeCanonicalUrl || !maybeResponseUrl)
        return std::string(canonicalLocation.view());

    if (maybeCanonicalUrl->isHttp() == maybeResponseUrl->isHttp() &&
        maybeCanonicalUrl->host() == maybeResponseUrl->host()) {
        return std::string(maybeCanonicalUrl->pathWithSearch().view());
    }

    return std::string(canonicalLocation.view());
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
    auto parsed = String::fromBytes(*value);
    if (!parsed)
        return {};
    return grabValueOf(parsed);
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
        return std::unexpected(text::format("{} missing params", event.method.view()));
    try {
        return event.params->extra.As<T>();
    } catch (const json::Exception &) {
        return std::unexpected(text::format("{} has invalid params", event.method.view()));
    }
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
    const auto nextRetainedBytes = budget.retainedBytes + i64(body.size());
    if (nextRetainedBytes > budget.maxBytes)
        return std::unexpected(
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
    const auto maybeUrl = Url::fromText(urlText);
    if (!maybeUrl)
        return {};
    if (!maybeUrl->isHttp() && !maybeUrl->isHttps())
        return {};

    return text::format("{}://{}", maybeUrl->isHttps() ? "https" : "http", maybeUrl->host());
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
            text::format("{}{}{}", *origin, maybeBaseUrl->pathname(), *location)
        );
    }

    return canonicalizeCapturedUrl(requestUrl);
}

[[nodiscard]] std::optional<String> normalizeInterceptedUrlForDenylist(const String &requestUrl)
{
    if (requestUrl.startsWith("ws://")) {
        auto normalized = String::fromBytes("http://" + std::string(requestUrl.view().substr(5)));
        if (!normalized)
            return {};
        return grabValueOf(normalized);
    }
    if (requestUrl.startsWith("wss://")) {
        auto normalized = String::fromBytes("https://" + std::string(requestUrl.view().substr(6)));
        if (!normalized)
            return {};
        return grabValueOf(normalized);
    }

    const auto parsed = Url::fromText(requestUrl);
    if (!parsed)
        return {};
    if (!parsed->isHttp() && !parsed->isHttps())
        return {};
    return requestUrl;
}

[[nodiscard]] Expected<bool, String>
isAllowedByDenylist(Denylist &denylist, const Config &config, const String &requestUrl)
{
    const auto normalized = normalizeInterceptedUrlForDenylist(requestUrl);
    if (!normalized)
        return true;

    const auto link = Link::fromText(
        *normalized, config.urlBytesMax(), Link::FromTextOptions::kStripPort
    );
    if (!link)
        return std::unexpected(
            text::format("failed to normalize intercepted request url {}", normalized->view())
        );

    const auto allowed = denylist.isAllowedPrefix(prefix::makePrefixKey(*link));
    if (!allowed)
        return std::unexpected("denylist check failed during fetch interception"_t);
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
            .value = std::format("{}", bodyBytes),
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
        return state->loaded || state->mainRequestFailure.has_value();
    }

    [[nodiscard]] bool hasMainDocumentOrFailure() const
    {
        const auto state = data.Lock();
        const auto *request = activeMainRequest(*state);
        return state->mainRequestFailure.has_value() ||
               (state->completedMainRequest.has_value() && state->completedMainRequest->loaded &&
                hasResponse(*state->completedMainRequest)) ||
               (request != nullptr && hasResponse(*request) && request->loaded);
    }

    [[nodiscard]] bool isIdleFor(chrono::seconds idle) const
    {
        const auto state = data.Lock();
        return state->inflight.empty() &&
               us::utils::datetime::SteadyNow() - state->lastNetworkAt >= idle;
    }

    [[nodiscard]] us::utils::datetime::SteadyClock::time_point
    idleDeadline(chrono::seconds idle) const
    {
        const auto state = data.Lock();
        return state->lastNetworkAt + idle;
    }

    [[nodiscard]] std::optional<crawler::SeedPageProbe> currentSeedProbe() const
    {
        const auto state = data.Lock();
        if (const auto *request = resolvedMainRequest(*state);
            request != nullptr && request->statusCode) {
            const auto loadState = request->loaded && !state->mainRequestFailure ? 2_i64 : 0_i64;
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
            params.requestId = std::string(bodyRequestId->view());
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
        auto resources = std::vector<crawler::CapturedResource>{};
        auto requests = std::vector<std::pair<String, TrackedRequest>>{};
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
                params.requestId = std::string(requestId.view());
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
                    return std::unexpected(std::move(body).error());
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
        chrono::steady_clock::time_point lastNetworkAt{us::utils::datetime::SteadyNow()};
    };

    static void applyMainResponse(
        const Data &state, crawler::CapturedExchange &exchange, const String &finalUrl
    )
    {
        const auto response = selectMainResponse(state, finalUrl);
        UINVARIANT(response, "missing main response while building exchange");
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
        UINVARIANT(
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
        state.lastNetworkAt = us::utils::datetime::SteadyNow();

        auto isTrackedMainDocument = false;
        if (isMainFrameDocumentRequest(state, requestWillBeSent)) {
            UINVARIANT(
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
                UINVARIANT(
                    false, std::format(
                               "main document response received for unknown request id {}",
                               requestIdText.view()
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
        state.lastNetworkAt = us::utils::datetime::SteadyNow();
        if (const auto it = state.activeRequests.find(requestIdText);
            it != std::end(state.activeRequests)) {
            it->second.loaded = true;
            if (it->second.isTrackedMainDocument && hasResponse(it->second)) {
                state.completedMainRequest = it->second;
                state.mainResponseRequestId = requestIdText;
            }
        } else if (state.mainRequestId && *state.mainRequestId == requestIdText) {
            UINVARIANT(
                false,
                std::format(
                    "main document loading finished for unknown request id {}", requestIdText.view()
                )
            );
        }
    }

    static void handleLoadingFailed(Data &state, dto::NetworkLoadingFailedEvent loadingFailed)
    {
        const auto requestIdText = String::fromBytes(loadingFailed.requestId).expect();
        state.inflight.erase(requestIdText);
        state.lastNetworkAt = us::utils::datetime::SteadyNow();

        const auto requestIt = state.activeRequests.find(requestIdText);
        if (requestIt == std::end(state.activeRequests)) {
            if (state.mainRequestId && *state.mainRequestId == requestIdText) {
                UINVARIANT(
                    false, std::format(
                               "main document loading failed for unknown request id {}",
                               requestIdText.view()
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
        UINVARIANT(
            redirectResponse && redirectResponse->status, "redirect response must include status"
        );

        const auto requestIt = state.activeRequests.find(requestId);
        UINVARIANT(
            requestIt != std::end(state.activeRequests),
            std::format("redirect response for unknown request id {}", requestId.view())
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
        UINVARIANT(
            hasResponse(request),
            std::format(
                "main redirect request missing response fields for {}", request.requestUrl.view()
            )
        );

        crawler::CapturedMainDocumentRedirect redirect;
        redirect.redirectUrl = request.requestUrl;
        redirect.statusCode = *request.statusCode;
        redirect.statusMessage = *request.statusMessage;
        redirect.headers = *request.headers;
        redirect.timestamp = *request.timestamp;

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
        UINVARIANT(
            hasResponse(request), std::format(
                                      "resource redirect request missing response fields for {}",
                                      request.requestUrl.view()
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
    dto::RuntimeEvaluateParams params;
    params.expression =
        "(() => ({ finalUrl: location.href, title: document.title || undefined, html: "
        "document.documentElement ? document.documentElement.outerHTML : \"\" }))()";
    params.returnByValue = true;
    params.awaitPromise = false;

    const auto result = cdpSession.send<dto::RuntimeEvaluateDomStateResult>(
        "Runtime.evaluate"_t, params
    );
    if (!result)
        return std::unexpected(describeCdpFailure("failed to read dom state"_t, result.error()));
    const auto &value = result->result.value;
    auto title = value.title.transform([](const auto &t) -> std::optional<String> {
        auto parsed = String::fromBytes(t);
        if (!parsed)
            return {};
        return grabValueOf(parsed);
    });
    auto finalUrl = String::fromBytes(value.finalUrl);
    if (!finalUrl)
        return std::unexpected("Runtime.evaluate returned invalid finalUrl"_t);
    finalUrl = canonicalizeCapturedUrl(*finalUrl);
    return DomState{
        .finalUrl = grabValueOf(finalUrl),
        .title = title.value_or(std::nullopt),
        .html = value.html,
    };
}

Expected<void, String>
runSiteBehavior(crawler::CdpSession &cdpSession, us::engine::Deadline deadline)
{
    UINVARIANT(deadline.IsReachable(), "site behavior deadline must be reachable");
    auto budgetExpected = timeLeftMs(deadline);
    if (!budgetExpected)
        return std::unexpected("timed out running site behavior"_t);
    const auto budget = grabValueOf(budgetExpected);

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
    const auto result = cdpSession.send<json::Value>("Runtime.evaluate"_t, params);
    if (!result)
        return std::unexpected(describeCdpFailure("failed to run site behavior"_t, result.error()));
    return {};
}

class [[nodiscard]] CaptureSession final {
public:
    CaptureSession(
        Denylist &denylist, const Config &config, dns::Resolver &dnsResolver, usize urlBytesMax,
        i64 proxyDownBytesMax, us::engine::subprocess::ProcessStarter &processStarter,
        std::string browserRunsRootIn, std::string cgroupRootPathIn,
        std::optional<crawler::CgroupLimits> cgroupLimitsIn, crawler::CaptureTimings timings,
        crawler::CrawlerTunables tunablesIn, i64 maxArchiveBytesIn, us::engine::Deadline deadline,
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
            return std::unexpected(CaptureFailure{std::move(failureDetail), {}});
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
            return std::unexpected(
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
            return std::unexpected(
                describeCdpFailure(text::format("{} failed", method), result.error())
            );
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
                return std::unexpected(*failure);
            auto progress = eventProgress.UniqueLock();
            const auto version = progress->version;
            progress.GetLock().unlock();
            if (std::invoke(predicate))
                return {};
            if (const auto failure = currentWaitFailure())
                return std::unexpected(*failure);
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
                    return std::unexpected(*failure);
                return std::unexpected(timeoutMessage);
            }
        }
        if (const auto failure = currentWaitFailure())
            return std::unexpected(*failure);
        return {};
    }

    [[nodiscard]] Expected<void, String> waitForIdle(chrono::seconds idle)
    {
        while (!pageTracker().isIdleFor(idle)) {
            if (const auto failure = currentWaitFailure())
                return std::unexpected(*failure);
            auto progress = eventProgress.UniqueLock();
            const auto version = progress->version;
            const auto idleDeadline = us::engine::Deadline::FromTimePoint(
                pageTracker().idleDeadline(idle)
            );
            const auto waitDeadline = pickEarlierDeadline(deadline, idleDeadline);
            progress.GetLock().unlock();
            if (pageTracker().isIdleFor(idle))
                return {};
            if (const auto failure = currentWaitFailure())
                return std::unexpected(*failure);
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
                    return std::unexpected(*failure);
                if (deadline.IsReached())
                    return std::unexpected("timed out waiting for network idle"_t);
            }
        }
        if (const auto failure = currentWaitFailure())
            return std::unexpected(*failure);
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
        eventTask = std::move(us::engine::CriticalAsyncNoSpan([this]() {
                        runEventLoop();
                    })).AsTask();
    }

    void stopEventLoop()
    {
        stoppingEventLoop.store(true);
        if (!eventTask.IsValid())
            return;
        eventTask.RequestCancel();
        const us::engine::TaskCancellationBlocker blocker;
        static_cast<void>(eventTask.WaitNothrow());
        eventTask = {};
    }

    [[nodiscard]] Expected<void, String> launch()
    {
        auto launched = browser.launch();
        if (!launched)
            return std::unexpected(launched.error());
        browser.markPhase("connect_cdp");
        auto connected = browser.connectCdp(deadline);
        if (!connected)
            return std::unexpected(connected.error());
        cdp = grabValueOf(connected);
        pageSession = std::make_unique<crawler::BrowserPageSession>(cdpClient());

        browser.markPhase("create_browser_context");
        if (auto created = pageSession->createBrowserContext(); !created)
            return std::unexpected(created.error());

        browser.markPhase("create_target");
        if (auto created = pageSession->createBlankTarget(); !created)
            return std::unexpected(created.error());

        browser.markPhase("attach_target");
        if (auto attached = pageSession->attachToTarget(); !attached)
            return std::unexpected(attached.error());
        tracker = std::make_unique<PageTracker>(pageSession->sessionId(), pageSession->targetId());
        startEventLoop();
        return {};
    }

    [[nodiscard]] Expected<crawler::CapturedExchange, String> captureAttachedTarget()
    {
        browser.markPhase("enable_page");
        if (auto ok = sendSessionVoid("Page.enable"_t); !ok)
            return std::unexpected(ok.error());
        browser.markPhase("enable_runtime");
        if (auto ok = sendSessionVoid("Runtime.enable"_t); !ok)
            return std::unexpected(ok.error());
        browser.markPhase("enable_network");
        if (auto ok = sendSessionVoid("Network.enable"_t); !ok)
            return std::unexpected(ok.error());

        browser.markPhase("enable_fetch");
        dto::FetchEnableParams fetchParams;
        fetchParams.handleAuthRequests = true;
        if (auto ok = sendSessionVoid("Fetch.enable"_t, fetchParams); !ok)
            return std::unexpected(ok.error());

        browser.markPhase("enable_lifecycle_events");
        dto::PageSetLifecycleEventsEnabledParams lifecycleParams;
        lifecycleParams.enabled = true;
        if (auto ok = sendSessionVoid("Page.setLifecycleEventsEnabled"_t, lifecycleParams); !ok)
            return std::unexpected(ok.error());

        browser.markPhase("disable_cache");
        dto::NetworkSetCacheDisabledParams cacheParams;
        cacheParams.cacheDisabled = true;
        if (auto ok = sendSessionVoid("Network.setCacheDisabled"_t, cacheParams); !ok)
            return std::unexpected(ok.error());

        browser.markPhase("bypass_service_worker");
        dto::NetworkSetBypassServiceWorkerParams serviceWorkerParams;
        serviceWorkerParams.bypass = true;
        if (auto ok = sendSessionVoid("Network.setBypassServiceWorker"_t, serviceWorkerParams);
            !ok) {
            return std::unexpected(ok.error());
        }

        browser.markPhase("set_extra_headers");
        dto::NetworkSetExtraHTTPHeadersParams headerParams;
        headerParams.headers.extra.emplace(
            "Accept-Language", std::string(crawler::kBrowserAcceptLanguage)
        );
        if (auto ok = sendSessionVoid("Network.setExtraHTTPHeaders"_t, headerParams); !ok)
            return std::unexpected(ok.error());

        browser.markPhase("get_frame_tree");
        auto frameTree = cdpSession().send<dto::PageGetFrameTreeResult>("Page.getFrameTree"_t);
        if (!frameTree)
            return std::unexpected(
                describeCdpFailure("Page.getFrameTree failed"_t, frameTree.error())
            );
        pageTracker().setMainFrameId(String::fromBytes(frameTree->frameTree.frame.id).expect());

        browser.markPhase("navigate");
        dto::PageNavigateParams navigateParams;
        navigateParams.url = std::string(run.seedUrl.view());
        pageTracker().beginSeedNavigation(run.seedUrl);
        auto navigateResult = cdpSession().send<dto::PageNavigateResult>(
            "Page.navigate"_t, navigateParams
        );
        if (!navigateResult)
            return std::unexpected(
                describeCdpFailure("Page.navigate failed"_t, navigateResult.error())
            );
        if (navigateResult->errorText)
            return std::unexpected(String::fromBytes(*navigateResult->errorText).expect());
        pageTracker().setExpectedMainLoaderId(stringOrNull(navigateResult->loaderId));

        browser.markPhase("wait_for_load");
        if (auto waited = waitForPredicate(
                [this]() { return pageTracker().isLoadedOrFailed(); },
                "timed out waiting for page load"_t
            );
            !waited)
            return std::unexpected(waited.error());
        if (timings.postLoadDelay > chrono::seconds::zero()) {
            browser.markPhase("post_load_delay");
            const auto phaseDeadline = pickEarlierDeadline(
                deadline, us::engine::Deadline::FromDuration(timings.postLoadDelay)
            );
            const auto ok = sleepUntilDeadline(phaseDeadline);
            if (!ok)
                return std::unexpected("timed out waiting for post-load delay"_t);
            browser.markPhase("post_load_delay_done");
        }
        if (timings.behaviorTimeout > chrono::seconds::zero()) {
            browser.markPhase("run_site_behavior");
            browser.markPhase("run_site_behavior_runtime_evaluate");
            const auto behaviorDeadline = pickEarlierDeadline(
                deadline, us::engine::Deadline::FromDuration(timings.behaviorTimeout)
            );
            auto ranSiteBehavior = runSiteBehavior(cdpSession(), behaviorDeadline);
            if (!ranSiteBehavior)
                return std::unexpected(ranSiteBehavior.error());
            browser.markPhase("run_site_behavior_done");
        }
        if (timings.netIdleWait > chrono::seconds::zero()) {
            browser.markPhase("wait_for_idle");
            browser.markPhase("wait_for_idle_wait");
            auto waited = waitForIdle(timings.netIdleWait);
            if (!waited)
                return std::unexpected(waited.error());
            browser.markPhase("wait_for_idle_done");
        }
        if (timings.pageExtraDelay > chrono::seconds::zero()) {
            browser.markPhase("page_extra_delay");
            const auto phaseDeadline = pickEarlierDeadline(
                deadline, us::engine::Deadline::FromDuration(timings.pageExtraDelay)
            );
            const auto ok = sleepUntilDeadline(phaseDeadline);
            if (!ok)
                return std::unexpected("timed out waiting for extra page delay"_t);
            browser.markPhase("page_extra_delay_done");
        }
        browser.markPhase("wait_for_main_document");
        browser.markPhase("wait_for_main_document_wait");
        if (auto waited = waitForPredicate(
                [this]() { return pageTracker().hasMainDocumentOrFailure(); },
                "timed out waiting for main document response"_t
            );
            !waited)
            return std::unexpected(waited.error());
        browser.markPhase("wait_for_main_document_done");

        browser.markPhase("read_dom_state");
        browser.markPhase("read_dom_state_runtime_evaluate");
        auto domState = readDomState(cdpSession());
        if (!domState)
            return std::unexpected(domState.error());
        browser.markPhase("read_dom_state_done");
        RetainedBodyBudget budget{maxArchiveBytes, 0_i64};
        browser.markPhase("read_main_body");
        auto body = pageTracker().readBody(cdpSession(), budget, domState->html);
        if (!body)
            return std::unexpected(body.error());
        browser.markPhase("read_resources");
        auto resources = pageTracker().readResources(cdpSession(), budget);
        if (!resources)
            return std::unexpected(resources.error());

        stopEventLoop();
        browser.markPhase("detach_target");
        if (auto detached = pageSession->detach(); !detached)
            return std::unexpected(detached.error());
        browser.markPhase("dispose_browser_context");
        if (auto disposed = pageSession->disposeBrowserContext(); !disposed)
            return std::unexpected(disposed.error());
        pageSession.reset();

        browser.markPhase("build_exchange_start");
        LOG_INFO() << std::format(
            "captureViaProxy building exchange for {} (body_bytes={}, resources={})", run.seedUrl,
            body->size(), resources->size()
        );
        auto exchange = pageTracker().buildExchange(
            std::move(domState->finalUrl), std::move(domState->title), grabValueOf(body),
            grabValueOf(resources)
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
            return std::unexpected(*proxyFailure);
        LOG_INFO() << std::format("captureViaProxy returning capture for {}", run.seedUrl);
        return exchange;
    }

    void closeCdpForFailure()
    {
        stopEventLoop();
        pageSession.reset();
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
        UINVARIANT(cdp, "cdp client is not connected");
        return *cdp;
    }

    [[nodiscard]] PageTracker &pageTracker() const
    {
        UINVARIANT(tracker, "page tracker is not attached");
        return *tracker;
    }

    [[nodiscard]] crawler::CdpSession &cdpSession() const
    {
        UINVARIANT(pageSession, "cdp session is not attached");
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
        dto::FetchFulfillRequestParams params;
        params.requestId = paused->requestId;
        params.responseCode = 403;
        params.responsePhrase = "Forbidden";
        params.responseHeaders = buildBlockedFetchHeaders(kBody.size());
        params.body = us::crypto::base64::Base64Encode(kBody);
        const auto fulfilled = cdpSession().sendVoid("Fetch.fulfillRequest"_t, params);
        if (!fulfilled)
            noteInterceptionFailure(
                describeCdpFailure("Fetch.fulfillRequest failed"_t, fulfilled.error())
            );
    }

    struct EventProgressState final {
        us::engine::ConditionVariable cv;
        i64 version{0_i64};
    };

    Denylist &denylist;
    const Config &config;
    crawler::CaptureTimings timings;
    crawler::RunRequest run;
    us::engine::Deadline deadline;
    i64 maxArchiveBytes;
    crawler::BrowserSession browser;
    std::unique_ptr<crawler::CdpClient> cdp;
    std::unique_ptr<PageTracker> tracker;
    std::unique_ptr<crawler::BrowserPageSession> pageSession;
    us::concurrent::Variable<EventProgressState> eventProgress;
    us::engine::Task eventTask;
    std::atomic<bool> stoppingEventLoop{false};
    us::concurrent::Variable<std::optional<String>> interceptionFailure;
};

[[nodiscard]] Expected<CaptureWithNetwork, CaptureFailure> captureViaProxy(
    Denylist &denylist, const Config &config, dns::Resolver &dnsResolver, usize urlBytesMax,
    i64 proxyDownBytesMax, us::engine::subprocess::ProcessStarter &processStarter,
    const std::string &browserRunsRoot, const std::string &cgroupRootPath,
    std::optional<crawler::CgroupLimits> cgroupLimits, crawler::CaptureTimings timings,
    const crawler::CrawlerTunables &tunables, i64 maxArchiveBytes, us::engine::Deadline deadline,
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
    us::engine::subprocess::ProcessStarter &processStarter, const std::string &browserRunsRoot,
    const std::string &cgroupRootPath, std::optional<crawler::CgroupLimits> cgroupLimits,
    const crawler::CaptureTimings &timings, const crawler::CrawlerTunables &tunables,
    i64 maxArchiveBytes, i64 networkDownBytesRatioMax, us::engine::Deadline deadline,
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
            out.stderrLog = std::string(captured.error().detail.view()) + "\n";
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
                UINVARIANT(false, log.error().detail);
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
            out.stderrLog += std::string(detail.view()) + "\n";
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
            out.stderrLog += std::string(detail.view()) + "\n";
            return out;
        }

        const auto exitCode = exchange.statusCode >= 400_i64 ? 9_i64 : 0_i64;
        const auto loadState = exitCode != 0_i64 || exchange.statusCode >= 400_i64 ? 0_i64 : 2_i64;
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
    us::engine::subprocess::ProcessStarter &processStarter, chrono::seconds runTimeout,
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
    const auto deadline = us::engine::Deadline::FromDuration(runTimeout);
    return executeRun(
        denylist, config, dnsResolver, processStarter, browserRunsRoot, cgroupRootPath,
        cgroupLimits, timings, tunables, maxArchiveBytes, networkDownBytesRatioMax, deadline,
        crawler::RunRequest{.seedUrl = seedUrl}
    );
}

} // namespace v1
