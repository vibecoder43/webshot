#include "crawler/runner.hpp"

#include "config.hpp"
#include "crawler/artifacts.hpp"
#include "crawler/browser_session.hpp"
#include "crawler/cdp_client.hpp"
#include "crawler/egress_proxy.hpp"
#include "crawler/failure.hpp"
#include "crawler/launch_policy.hpp"
#include "crawler/limits.hpp"
#include "crypto.hpp"
#include "deadline_utils.hpp"
#include "denylist.hpp"
#include "grab_value.hpp"
#include "integers.hpp"
#include "invariant.hpp"
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
#include <exception>
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
#include <userver/engine/async.hpp>
#include <userver/engine/condition_variable.hpp>
#include <userver/engine/deadline.hpp>
#include <userver/engine/sleep.hpp>
#include <userver/engine/subprocess/process_starter.hpp>
#include <userver/engine/task/cancel.hpp>
#include <userver/formats/json.hpp>
#include <userver/formats/json/value_builder.hpp>
#include <userver/logging/log.hpp>
#include <userver/utils/assert.hpp>
#include <userver/utils/boost_uuid4.hpp>
#include <userver/utils/datetime.hpp>
#include <userver/utils/resources.hpp>
namespace chrono = std::chrono;
namespace ujson = userver::formats::json;
using namespace std::chrono_literals;
using namespace text::literals;

namespace ws {
namespace us = userver;
namespace eng = us::engine;
namespace datetime = us::utils::datetime;
namespace dns = us::clients::dns;
namespace {

using crawler::DescribeCdpFailure;
using ws::Expected;

constexpr auto kCdpWsPayloadSlackBytes = 2_i64 * 1024_i64 * 1024_i64;
const auto kLocalFixtureHttpPort = "18080"_t;
const auto kLocalFixtureHttpsPort = "18443"_t;
constexpr std::array kLocalFixtureHosts = {
    std::string_view{"test-target"},
    std::string_view{"asset.test-target"},
    std::string_view{"untrusted.test-target"},
};

[[nodiscard]] i64 ComputeCdpMaxRemotePayloadBytes(i64 max_archive_bytes)
{
    return (max_archive_bytes * 4_i64) / 3_i64 + kCdpWsPayloadSlackBytes;
}

[[nodiscard]] String CurrentTimestamp()
{
    return *String::FromBytes(datetime::UtcTimestring(datetime::Now(), datetime::kRfc3339Format));
}

[[nodiscard]] std::unordered_map<std::string, std::string>
NormalizeHeaders(const dto::CdpHeaders &headers)
{
    std::unordered_map<std::string, std::string> out;
    for (const auto &[name, value] : headers.extra)
        out.emplace(absl::AsciiStrToLower(std::string_view{name}), value);
    return out;
}

[[nodiscard]] bool IsLocalFixtureHost(const String &host) noexcept
{
    return std::ranges::contains(kLocalFixtureHosts, host.View());
}

[[nodiscard]] String CanonicalizeCapturedUrl(const String &url_text)
{
    const auto maybe_url = Url::FromText(url_text);
    if (!maybe_url)
        return url_text;
    if (!maybe_url->IsHttpOrHttps())
        return url_text;
    if (!maybe_url->HasPort())
        return url_text;
    if (!IsLocalFixtureHost(maybe_url->Hostname()))
        return url_text;

    const auto port = maybe_url->Port();
    const auto matches_fixture_port = (maybe_url->IsHttp() && port == kLocalFixtureHttpPort) ||
                                      (maybe_url->IsHttps() && port == kLocalFixtureHttpsPort);
    if (!matches_fixture_port)
        return url_text;

    return maybe_url->Stripped(Url::StripOptions::kPort).Href();
}

[[nodiscard]] std::string
CanonicalizeCapturedLocationHeader(const String &response_url, std::string_view location_value)
{
    auto location = String::FromBytes(location_value);
    if (!location)
        return std::string(location_value);
    if (location->Empty() || location->StartsWith('/') || location->StartsWith('?') ||
        location->StartsWith("//")) {
        return location->ToBytes();
    }

    const auto canonical_location = CanonicalizeCapturedUrl(*location);
    const auto maybe_canonical_url = Url::FromText(canonical_location);
    const auto maybe_response_url = Url::FromText(response_url);
    if (!maybe_canonical_url || !maybe_response_url)
        return canonical_location.ToBytes();

    if (maybe_canonical_url->IsHttp() == maybe_response_url->IsHttp() &&
        maybe_canonical_url->Host() == maybe_response_url->Host()) {
        return maybe_canonical_url->PathWithSearch().ToBytes();
    }

    return canonical_location.ToBytes();
}

[[nodiscard]] std::unordered_map<std::string, std::string>
NormalizeHeadersOrEmpty(const std::optional<dto::CdpHeaders> &headers)
{
    if (!headers)
        return {};
    return NormalizeHeaders(*headers);
}

[[nodiscard]] std::unordered_map<std::string, std::string> NormalizeHeadersForCapture(
    const std::optional<dto::CdpHeaders> &headers, const String &response_url
)
{
    auto normalized = NormalizeHeadersOrEmpty(headers);
    if (const auto it = normalized.find("location"); it != std::end(normalized))
        it->second = CanonicalizeCapturedLocationHeader(response_url, it->second);
    return normalized;
}

[[nodiscard]] std::optional<String> StringOrNull(const std::optional<std::string> &value)
{
    return TRY(text::OptionalString(value));
}

[[nodiscard]] String GeneratePageId()
{
    return text::Format("{}", us::utils::generators::GenerateBoostUuid());
}

struct [[nodiscard]] CaptureFailure final {
    String detail;
    std::optional<crawler::SeedPageProbe> seed_probe;
};

template <typename T>
[[nodiscard]] Expected<T, String> ParseEventParams(const crawler::CdpEvent &event)
{
    if (!event.params)
        return Unex(text::Format("{} missing params", event.method));
    return ws::json::As<T>(
        event.params->extra, text::Format("{} has invalid params", event.method)
    );
}

struct [[nodiscard]] RetainedBodyBudget {
    RetainedBodyBudget(i64 max_bytes, i64 retained_bytes)
        : max_bytes(max_bytes), retained_bytes(retained_bytes)
    {
    }

    i64 max_bytes;
    i64 retained_bytes;
};

struct [[nodiscard]] CaptureWithNetwork final {
    crawler::CapturedExchange exchange;
    i64 proxy_down_bytes{0};
};

[[nodiscard]] Expected<std::string, String>
RetainBody(const std::string &body, RetainedBodyBudget &budget)
{
    const auto next_retained_bytes = budget.retained_bytes + ssize(body);
    if (next_retained_bytes > budget.max_bytes)
        return Unex(
            text::Format(
                "size_limit: retained body bytes {} exceeded size limit {}", next_retained_bytes,
                budget.max_bytes
            )
        );
    budget.retained_bytes = next_retained_bytes;
    return body;
}

[[nodiscard]] std::optional<std::string>
DecodeCdpBody(const dto::NetworkGetResponseBodyResult &body)
{
    if (!body.base64Encoded)
        return body.body;

    const auto decoded = ws::crypto::Base64Decode(body.body, false);
    if (!decoded)
        return {};
    return *decoded;
}

[[nodiscard]] bool ResponseCanHaveBody(const String &method, i64 status_code)
{
    if (method == "HEAD"_t)
        return false;
    return (status_code < 100_i64 || status_code >= 200_i64) && status_code != 204_i64 &&
           status_code != 304_i64;
}

[[nodiscard]] bool IsSuccessfulMainDocumentExchange(const crawler::CapturedExchange &exchange)
{
    return exchange.status_code >= 200_i64 && exchange.status_code < 400_i64;
}

[[nodiscard]] std::optional<String> BuildUrlOrigin(const String &url_text)
{
    const auto maybe_url = TRY(Url::FromText(url_text));
    if (!maybe_url.IsHttpOrHttps())
        return {};

    return maybe_url.Origin();
}

[[nodiscard]] String ResolveRedirectTargetUrl(
    const String &base_text, const String &request_text,
    const std::optional<dto::NetworkResponse> &redirect_response
)
{
    if (!redirect_response)
        return CanonicalizeCapturedUrl(request_text);

    const auto headers = NormalizeHeadersForCapture(redirect_response->headers, base_text);
    const auto location_it = headers.find("location");
    if (location_it == std::end(headers) || location_it->second.empty())
        return CanonicalizeCapturedUrl(request_text);

    const auto location = String::FromBytes(location_it->second);
    if (!location)
        return CanonicalizeCapturedUrl(request_text);
    if (const auto absolute_location = Url::FromText(*location))
        return CanonicalizeCapturedUrl(absolute_location->Href());

    const auto origin = BuildUrlOrigin(base_text);
    if (!origin)
        return CanonicalizeCapturedUrl(request_text);

    if (location->StartsWith("//")) {
        const auto maybe_base_url = Url::FromText(base_text);
        if (!maybe_base_url)
            return CanonicalizeCapturedUrl(request_text);
        return CanonicalizeCapturedUrl(
            text::Format("{}:{}", maybe_base_url->IsHttps() ? "https" : "http", *location)
        );
    }

    if (location->StartsWith('/'))
        return CanonicalizeCapturedUrl(text::Format("{}{}", *origin, *location));

    if (location->StartsWith('?')) {
        const auto maybe_base_url = Url::FromText(base_text);
        if (!maybe_base_url)
            return CanonicalizeCapturedUrl(request_text);
        return CanonicalizeCapturedUrl(
            maybe_base_url->Stripped(Url::StripOptions::kHash | Url::StripOptions::kQuery)
                .WithSearch(*location)
                .Href()
        );
    }

    return CanonicalizeCapturedUrl(request_text);
}

[[nodiscard]] Expected<Link, String> LinkFromInterceptionUrl(const Config &config, const Url &url)
{
    const auto href = url.Href();
    if (href.StartsWith("ws://"))
        return TRY_MAP_ERR(
            Link::FromText(url.WithProtocol("http"_t).Href(), config.UrlBytesMax()),
            ([&](const auto &) {
                return text::Format("failed to normalize intercepted request url {}", href);
            })
        );
    if (href.StartsWith("wss://"))
        return TRY_MAP_ERR(
            Link::FromText(url.WithProtocol("https"_t).Href(), config.UrlBytesMax()),
            ([&](const auto &) {
                return text::Format("failed to normalize intercepted request url {}", href);
            })
        );

    return TRY_MAP_ERR(
        Link::FromText(href, config.UrlBytesMax()), ([&](const auto &) {
            return text::Format("failed to normalize intercepted request url {}", href);
        })
    );
}

[[nodiscard]] std::optional<Url> AccessPolicyUrlFromText(const String &text)
{
    auto url = TRY(Url::FromText(text));
    const auto href = url.Href();
    if (url.IsHttpOrHttps() || href.StartsWith("ws://") || href.StartsWith("wss://"))
        return url;
    return {};
}

[[nodiscard]] Expected<AccessDecision, String>
EvaluateAccessPolicy(Denylist &denylist, const Config &config, const Url &url)
{
    using enum AccessDecisionReason;

    const auto href = url.Href();
    if (config.HttpsOnly() && (url.IsHttp() || href.StartsWith("ws://")))
        return AccessDecision{.allowed = false, .reason = kNonHttps};

    const auto link = TRY(LinkFromInterceptionUrl(config, url));
    return TRY_ERR_AS(
        denylist.EvaluatePrefix(
            prefix::MakePrefixKey(link),
            config.AllowlistOnly() ? AccessPolicyMode::kAllowlistOnly : AccessPolicyMode::kRegular
        ),
        "access policy check failed during fetch interception"_t
    );
}

[[nodiscard]] std::vector<dto::FetchHeaderEntry> BuildBlockedFetchHeaders(usize body_bytes)
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
            .value = std::format("{}", body_bytes),
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
    String request_url;
    String method;
    std::optional<i64> status_code;
    std::optional<String> status_message;
    std::optional<std::unordered_map<std::string, std::string>> headers;
    std::optional<String> timestamp;
    std::optional<String> loader_id;
    std::optional<String> frame_id;
    std::optional<String> resource_type;
    bool loaded{false};
    bool is_tracked_main_document{false};
};

class [[nodiscard]] PageTracker final {
public:
    PageTracker(String session_id, String target_id)
    {
        auto state = data_.Lock();
        state->session_id = std::move(session_id);
        state->target_id = std::move(target_id);
        state->page_id = GeneratePageId();
    }

    void BeginSeedNavigation(const String &seed_url)
    {
        auto state = data_.Lock();
        state->seed_navigation_started = true;
        state->seed_navigation_url = seed_url;
    }

    void SetMainFrameId(String main_frame_id)
    {
        auto state = data_.Lock();
        state->main_frame_id = std::move(main_frame_id);
    }

    void SetExpectedMainLoaderId(std::optional<String> loader_id)
    {
        if (!loader_id)
            return;
        auto state = data_.Lock();
        state->main_loader_id = std::move(*loader_id);
    }

    void HandleEvent(const crawler::CdpEvent &event)
    {
        auto state = data_.Lock();
        const auto method = event.method.View();
        if (method == "Target.targetCrashed") {
            if (event.params) {
                const auto crashed = event.params->extra.As<dto::TargetTargetCrashedEvent>();
                if (crashed.targetId && state->target_id.View() == *crashed.targetId)
                    state->main_request_failure = "page target crashed"_t;
            }
            return;
        }
        if (method == "Target.detachedFromTarget") {
            if (event.params) {
                const auto detached_session_id = event.params->extra["sessionId"];
                if (!detached_session_id.IsMissing() &&
                    detached_session_id.As<std::string>() == state->session_id.View()) {
                    state->main_request_failure = "target session detached"_t;
                }
            }
            return;
        }
        if (method == "Target.targetDestroyed") {
            if (event.params) {
                const auto destroyed_target_id = event.params->extra["targetId"];
                if (!destroyed_target_id.IsMissing() &&
                    destroyed_target_id.As<std::string>() == state->target_id.View()) {
                    state->main_request_failure = "page target destroyed"_t;
                }
            }
            return;
        }
        if (method == "Inspector.detached") {
            if (event.session_id && *event.session_id != state->session_id)
                return;

            if (event.params) {
                const auto reason = event.params->extra["reason"];
                if (!reason.IsMissing()) {
                    state->main_request_failure = text::Format(
                        "inspector detached: {}", reason.As<std::string>()
                    );
                    return;
                }
            }
            state->main_request_failure = "inspector detached"_t;
            return;
        }

        if (!event.session_id || *event.session_id != state->session_id)
            return;

        if (method == "Page.loadEventFired") {
            state->loaded = true;
            return;
        }
        if (method == "Network.requestWillBeSent") {
            auto parsed = ParseEventParams<dto::NetworkRequestWillBeSentEvent>(event);
            if (!parsed)
                state->main_request_failure = parsed.Error();
            else
                HandleRequestWillBeSent(*state, GrabValueOf(parsed));
            return;
        }
        if (method == "Network.responseReceived") {
            auto parsed = ParseEventParams<dto::NetworkResponseReceivedEvent>(event);
            if (!parsed)
                state->main_request_failure = parsed.Error();
            else
                HandleResponseReceived(*state, GrabValueOf(parsed));
            return;
        }
        if (method == "Network.loadingFinished") {
            auto parsed = ParseEventParams<dto::NetworkLoadingFinishedEvent>(event);
            if (!parsed)
                state->main_request_failure = parsed.Error();
            else
                HandleLoadingFinished(*state, GrabValueOf(parsed));
            return;
        }
        if (method == "Network.loadingFailed") {
            auto parsed = ParseEventParams<dto::NetworkLoadingFailedEvent>(event);
            if (!parsed)
                state->main_request_failure = parsed.Error();
            else
                HandleLoadingFailed(*state, GrabValueOf(parsed));
        }
    }

    [[nodiscard]] bool IsLoadedOrFailed() const
    {
        const auto state = data_.Lock();
        return state->loaded || state->main_request_failure;
    }

    [[nodiscard]] bool HasMainDocumentOrFailure() const
    {
        const auto state = data_.Lock();
        const auto *request = ActiveMainRequest(*state);
        return state->main_request_failure ||
               (state->completed_main_request && state->completed_main_request->loaded &&
                HasResponse(*state->completed_main_request)) ||
               (request != nullptr && HasResponse(*request) && request->loaded);
    }

    [[nodiscard]] bool IsIdleFor(chrono::seconds idle) const
    {
        const auto state = data_.Lock();
        return state->inflight.empty() && datetime::SteadyNow() - state->last_network_at >= idle;
    }

    [[nodiscard]] datetime::SteadyClock::time_point IdleDeadline(chrono::seconds idle) const
    {
        const auto state = data_.Lock();
        return state->last_network_at + idle;
    }

    [[nodiscard]] std::optional<crawler::SeedPageProbe> CurrentSeedProbe() const
    {
        const auto state = data_.Lock();
        if (const auto *request = ResolvedMainRequest(*state);
            request != nullptr && request->status_code) {
            const i64 load_state{request->loaded && !state->main_request_failure ? 2_i64 : 0_i64};
            return crawler::SeedPageProbe{
                .status = Raw(*request->status_code),
                .load_state = Raw(load_state),
            };
        }

        if (state->main_request_id || state->main_request_failure || state->loaded)
            return crawler::SeedPageProbe{.status = Raw(0_i64), .load_state = Raw(0_i64)};

        return {};
    }

    [[nodiscard]] std::optional<String> FailureReason() const
    {
        const auto state = data_.Lock();
        return state->main_request_failure;
    }

    void Fail(String reason)
    {
        auto state = data_.Lock();
        state->main_request_failure = std::move(reason);
    }

    [[nodiscard]] Expected<std::string, String> ReadBody(
        crawler::CdpSession &cdp_session, RetainedBodyBudget &budget,
        const std::string &fallback_body
    ) const
    {
        if (!fallback_body.empty())
            return RetainBody(fallback_body, budget);
        std::optional<String> body_request_id;
        {
            const auto state = data_.Lock();
            if (state->main_response_request_id)
                body_request_id = state->main_response_request_id;
            else if (state->main_request_id)
                body_request_id = state->main_request_id;
        }
        if (!body_request_id)
            return RetainBody(fallback_body, budget);

        dto::NetworkGetResponseBodyParams params;
        params.requestId = body_request_id->ToBytes();
        const auto body = cdp_session.Send<dto::NetworkGetResponseBodyResult>(
            "Network.getResponseBody"_t, params
        );
        if (!body)
            return RetainBody(fallback_body, budget);
        const auto decoded_body = DecodeCdpBody(*body);
        if (!decoded_body)
            return RetainBody(fallback_body, budget);
        return RetainBody(*decoded_body, budget);
    }

    [[nodiscard]] Expected<std::vector<crawler::CapturedResource>, String>
    ReadResources(crawler::CdpSession &cdp_session, RetainedBodyBudget &budget) const
    {
        std::vector<crawler::CapturedResource> resources{};
        std::vector<std::pair<String, TrackedRequest>> requests{};
        {
            const auto state = data_.Lock();
            resources = state->redirected_resources;
            requests.reserve(state->active_requests.size());
            for (const auto &[request_id, request] : state->active_requests)
                requests.emplace_back(request_id, request);
        }

        for (const auto &[request_id, request] : requests) {
            if (request.is_tracked_main_document || !HasResponse(request)) {
                continue;
            }

            const auto response = ToMainResponse(request);

            if (!ResponseCanHaveBody(request.method, response.status_code)) {
                resources.push_back({
                    request.request_url,
                    request.method,
                    request.resource_type,
                    response.status_code,
                    response.status_message,
                    response.headers,
                    {},
                    response.timestamp,
                });
                continue;
            }

            dto::NetworkGetResponseBodyParams params;
            params.requestId = request_id.ToBytes();
            const auto body_value = cdp_session.Send<dto::NetworkGetResponseBodyResult>(
                "Network.getResponseBody"_t, params
            );
            const auto decoded_body = body_value ? DecodeCdpBody(*body_value)
                                                 : std::optional<std::string>{};
            if (!decoded_body) {
                resources.push_back({
                    request.request_url,
                    request.method,
                    request.resource_type,
                    response.status_code,
                    response.status_message,
                    response.headers,
                    {},
                    response.timestamp,
                });
                continue;
            }
            resources.push_back({
                request.request_url,
                request.method,
                request.resource_type,
                response.status_code,
                response.status_message,
                response.headers,
                TRY(RetainBody(*decoded_body, budget)),
                response.timestamp,
            });
        }

        std::ranges::sort(resources, [](const auto &left, const auto &right) {
            return left.timestamp < right.timestamp;
        });
        return resources;
    }

    [[nodiscard]] crawler::CapturedExchange BuildExchange(
        String final_url, std::optional<String> title, std::string body,
        std::vector<crawler::CapturedResource> resources
    ) const
    {
        const auto state = data_.Lock();
        crawler::CapturedExchange exchange{};
        exchange.seed_url = state->seed_navigation_url ? *state->seed_navigation_url : final_url;
        exchange.page_id = state->page_id;
        exchange.final_url = std::move(final_url);
        ApplyMainResponse(*state, exchange, exchange.final_url);
        exchange.redirect_chain = BuildRedirectChainForExchange(*state, exchange.final_url);
        exchange.main_document_redirects = state->main_document_redirects;
        exchange.body = std::move(body);
        exchange.resources = std::move(resources);
        exchange.title = std::move(title);
        return exchange;
    }

private:
    struct [[nodiscard]] MainResponse {
        String request_url;
        i64 status_code{0};
        String status_message;
        std::unordered_map<std::string, std::string> headers;
        String timestamp;
    };

    struct [[nodiscard]] Data final {
        String session_id;
        String target_id;
        String page_id;
        std::unordered_map<String, TrackedRequest> active_requests;
        std::vector<crawler::CapturedResource> redirected_resources;
        std::vector<String> redirect_chain;
        std::vector<crawler::CapturedMainDocumentRedirect> main_document_redirects;
        std::unordered_set<String> inflight;
        std::optional<String> main_request_id;
        std::optional<String> main_response_request_id;
        std::optional<String> main_loader_id;
        std::optional<String> main_frame_id;
        std::optional<String> seed_navigation_url;
        std::optional<TrackedRequest> completed_main_request;
        bool loaded{false};
        bool seed_navigation_started{false};
        std::optional<String> main_request_failure;
        chrono::steady_clock::time_point last_network_at{datetime::SteadyNow()};
    };

    static void ApplyMainResponse(
        const Data &state, crawler::CapturedExchange &exchange, const String &final_url
    )
    {
        const auto response = SelectMainResponse(state, final_url);
        Invariant(response, "missing main response while building exchange"_t);
        exchange.status_code = response->status_code;
        exchange.status_message = response->status_message;
        exchange.headers = response->headers;
        exchange.timestamp = response->timestamp;
    }

    [[nodiscard]] static std::vector<String>
    BuildRedirectChainForExchange(const Data &state, const String &final_url)
    {
        if (!state.redirect_chain.empty())
            return state.redirect_chain;
        return {final_url};
    }

    [[nodiscard]] static bool HasResponse(const TrackedRequest &request)
    {
        return request.status_code && request.status_message && request.headers &&
               request.timestamp;
    }

    [[nodiscard]] static MainResponse ToMainResponse(const TrackedRequest &request)
    {
        Invariant(
            request.status_code && request.status_message && request.headers && request.timestamp,
            "tracked request missing response"_t
        );
        return {
            request.request_url, *request.status_code, *request.status_message,
            *request.headers,    *request.timestamp,
        };
    }

    [[nodiscard]] static TrackedRequest *ActiveMainRequest(Data &state)
    {
        if (!state.main_request_id)
            return nullptr;
        if (const auto it = state.active_requests.find(*state.main_request_id);
            it != std::end(state.active_requests)) {
            return &it->second;
        }
        return nullptr;
    }

    [[nodiscard]] static const TrackedRequest *ActiveMainRequest(const Data &state)
    {
        if (!state.main_request_id)
            return nullptr;
        if (const auto it = state.active_requests.find(*state.main_request_id);
            it != std::end(state.active_requests)) {
            return &it->second;
        }
        return nullptr;
    }

    [[nodiscard]] static const TrackedRequest *ResolvedMainRequest(const Data &state)
    {
        if (state.completed_main_request && HasResponse(*state.completed_main_request))
            return &*state.completed_main_request;
        return ActiveMainRequest(state);
    }

    [[nodiscard]] static std::optional<MainResponse>
    SelectMainResponse(const Data &state, const String &final_url)
    {
        if (state.completed_main_request && HasResponse(*state.completed_main_request)) {
            if (state.completed_main_request->request_url == final_url)
                return ToMainResponse(*state.completed_main_request);
        }
        if (const auto *request = ActiveMainRequest(state);
            request != nullptr && HasResponse(*request)) {
            if (request->request_url == final_url)
                return ToMainResponse(*request);
        }
        if (state.completed_main_request && HasResponse(*state.completed_main_request))
            return ToMainResponse(*state.completed_main_request);
        if (const auto *request = ActiveMainRequest(state);
            request != nullptr && HasResponse(*request)) {
            return ToMainResponse(*request);
        }
        return {};
    }

    [[nodiscard]] static bool
    MatchesTrackedMainLoader(const Data &state, const std::optional<std::string> &loader_id)
    {
        if (!state.main_loader_id)
            return true;
        return loader_id && *loader_id == state.main_loader_id->View();
    }

    [[nodiscard]] static bool IsMainFrameDocumentRequest(
        const Data &state, const dto::NetworkRequestWillBeSentEvent &request_will_be_sent
    )
    {
        return request_will_be_sent.frameId && state.main_frame_id &&
               *request_will_be_sent.frameId == state.main_frame_id->View() &&
               request_will_be_sent.type && *request_will_be_sent.type == "Document";
    }

    static void
    HandleRequestWillBeSent(Data &state, dto::NetworkRequestWillBeSentEvent request_will_be_sent)
    {
        if (request_will_be_sent.request.url.starts_with("data:"))
            return;

        const auto request_id_text = *String::FromBytes(request_will_be_sent.requestId);
        const auto raw_request_url = *String::FromBytes(request_will_be_sent.request.url);
        const auto request_method = *String::FromBytes(request_will_be_sent.request.method);

        state.inflight.insert(request_id_text);
        state.last_network_at = datetime::SteadyNow();

        auto is_tracked_main_document = false;
        if (IsMainFrameDocumentRequest(state, request_will_be_sent)) {
            Invariant(
                state.seed_navigation_started,
                "main document request observed before seed navigation started"_t
            );
            if (state.main_loader_id &&
                !MatchesTrackedMainLoader(state, request_will_be_sent.loaderId)) {
                return;
            }
            if (!state.main_loader_id && !state.main_request_id && state.seed_navigation_url) {
                const auto matches_exact = raw_request_url == *state.seed_navigation_url;
                const auto matches_trailing_slash =
                    !state.seed_navigation_url->EndsWith('/') &&
                    raw_request_url.SizeBytes() == state.seed_navigation_url->SizeBytes() + 1 &&
                    raw_request_url.StartsWith(*state.seed_navigation_url) &&
                    raw_request_url.EndsWith('/');
                if (!matches_exact && !matches_trailing_slash)
                    return;
            }
            if (!state.main_loader_id && request_will_be_sent.loaderId)
                state.main_loader_id = StringOrNull(request_will_be_sent.loaderId);
            is_tracked_main_document = true;
        }

        std::optional<String> previous_request_url;
        if (const auto it = state.active_requests.find(request_id_text);
            it != std::end(state.active_requests)) {
            previous_request_url = it->second.request_url;
        }
        if (request_will_be_sent.redirectResponse)
            FinalizeRedirectRequest(state, request_id_text, request_will_be_sent.redirectResponse);

        const auto canonical_request_url = previous_request_url
                                               ? ResolveRedirectTargetUrl(
                                                     *previous_request_url, raw_request_url,
                                                     request_will_be_sent.redirectResponse
                                                 )
                                               : CanonicalizeCapturedUrl(raw_request_url);

        TrackedRequest tracked_request{
            canonical_request_url,
            request_method,
            {},
            {},
            {},
            {},
            StringOrNull(request_will_be_sent.loaderId),
            StringOrNull(request_will_be_sent.frameId),
            StringOrNull(request_will_be_sent.type),
            false,
            is_tracked_main_document,
        };
        state.active_requests.insert_or_assign(request_id_text, std::move(tracked_request));

        if (is_tracked_main_document) {
            state.main_request_id = request_id_text;
            if (state.redirect_chain.empty() ||
                state.redirect_chain.back() != canonical_request_url)
                state.redirect_chain.push_back(canonical_request_url);
            return;
        }
    }

    static void
    HandleResponseReceived(Data &state, dto::NetworkResponseReceivedEvent response_received)
    {
        const auto request_id_text = *String::FromBytes(response_received.requestId);
        const auto request_it = state.active_requests.find(request_id_text);
        if (request_it == std::end(state.active_requests)) {
            if (state.main_request_id && *state.main_request_id == request_id_text) {
                state.main_request_failure = text::Format(
                    "main document response received for unknown request id {}", request_id_text
                );
            }
            return;
        }

        const auto timestamp = CurrentTimestamp();
        auto &request = request_it->second;
        request.status_code = response_received.response.status
                                  ? i64(*response_received.response.status)
                                  : 0_i64;
        request.status_message = *String::FromBytes(
            response_received.response.statusText.value_or("")
        );
        request.headers = NormalizeHeadersForCapture(
            response_received.response.headers, request.request_url
        );
        request.timestamp = timestamp;
        if (response_received.type)
            request.resource_type = StringOrNull(response_received.type);
        if (response_received.loaderId)
            request.loader_id = StringOrNull(response_received.loaderId);
        if (request.is_tracked_main_document && HasResponse(request)) {
            state.completed_main_request = request;
            state.main_response_request_id = request_id_text;
        }
    }

    static void
    HandleLoadingFinished(Data &state, dto::NetworkLoadingFinishedEvent loading_finished)
    {
        const auto request_id_text = *String::FromBytes(loading_finished.requestId);
        state.inflight.erase(request_id_text);
        state.last_network_at = datetime::SteadyNow();
        if (const auto it = state.active_requests.find(request_id_text);
            it != std::end(state.active_requests)) {
            it->second.loaded = true;
            if (it->second.is_tracked_main_document && HasResponse(it->second)) {
                state.completed_main_request = it->second;
                state.main_response_request_id = request_id_text;
            }
        } else if (state.main_request_id && *state.main_request_id == request_id_text) {
            state.main_request_failure = text::Format(
                "main document loading finished for unknown request id {}", request_id_text
            );
        }
    }

    static void HandleLoadingFailed(Data &state, dto::NetworkLoadingFailedEvent loading_failed)
    {
        const auto request_id_text = *String::FromBytes(loading_failed.requestId);
        state.inflight.erase(request_id_text);
        state.last_network_at = datetime::SteadyNow();

        const auto request_it = state.active_requests.find(request_id_text);
        if (request_it == std::end(state.active_requests)) {
            if (state.main_request_id && *state.main_request_id == request_id_text) {
                state.main_request_failure = text::Format(
                    "main document loading failed for unknown request id {}", request_id_text
                );
            }
            return;
        }

        auto &request = request_it->second;
        request.loaded = true;
        if (!request.is_tracked_main_document)
            return;
        if (HasResponse(request))
            return;

        state.main_request_failure = *String::FromBytes(
            loading_failed.errorText.value_or("main document request failed")
        );
    }

    static void FinalizeRedirectRequest(
        Data &state, const String &request_id,
        const std::optional<dto::NetworkResponse> &redirect_response
    )
    {
        Invariant(
            redirect_response && redirect_response->status,
            "redirect response must include status"_t
        );

        const auto request_it = state.active_requests.find(request_id);
        if (request_it == std::end(state.active_requests)) {
            state.main_request_failure = text::Format(
                "redirect response for unknown request id {}", request_id
            );
            return;
        }

        auto request = std::move(request_it->second);
        state.active_requests.erase(request_it);

        request.status_code = i64(*redirect_response->status);
        request.status_message = *String::FromBytes(redirect_response->statusText.value_or(""));
        request.headers = NormalizeHeadersForCapture(
            redirect_response->headers, request.request_url
        );
        request.timestamp = CurrentTimestamp();
        request.loaded = true;

        if (request.is_tracked_main_document) {
            RecordMainDocumentRedirect(state, request);
            if (state.main_request_id && *state.main_request_id == request_id)
                state.main_request_id.reset();
            return;
        }

        RecordResourceRedirect(state, request);
    }

    static void RecordMainDocumentRedirect(Data &state, const TrackedRequest &request)
    {
        Invariant(
            HasResponse(request),
            text::Format(
                "main redirect request missing response fields for {}", request.request_url
            )
        );

        crawler::CapturedMainDocumentRedirect redirect{
            .redirect_url = request.request_url,
            .status_code = *request.status_code,
            .status_message = *request.status_message,
            .headers = *request.headers,
            .timestamp = *request.timestamp,
        };

        if (!state.main_document_redirects.empty()) {
            const auto &previous = state.main_document_redirects.back();
            if (previous.redirect_url == redirect.redirect_url &&
                previous.status_code == redirect.status_code) {
                return;
            }
        }
        state.main_document_redirects.push_back(std::move(redirect));
    }

    static void RecordResourceRedirect(Data &state, const TrackedRequest &request)
    {
        Invariant(
            HasResponse(request),
            text::Format(
                "resource redirect request missing response fields for {}", request.request_url
            )
        );

        state.redirected_resources.push_back({
            request.request_url,
            request.method,
            request.resource_type,
            *request.status_code,
            *request.status_message,
            *request.headers,
            {},
            *request.timestamp,
        });
    }

    us::concurrent::Variable<Data> data_;
};

struct [[nodiscard]] DomState {
    String final_url;
    std::optional<String> title;
    std::string html;
};

[[nodiscard]] Expected<DomState, String> ReadDomState(crawler::CdpSession &cdp_session)
{
    dto::RuntimeEvaluateParams params{
        .expression =
            "(() => ({ finalUrl: location.href, title: document.title || undefined, html: "
            "document.documentElement ? document.documentElement.outerHTML : \"\" }))()",
        .returnByValue = true,
        .awaitPromise = false,
    };

    const auto result = TRY_MAP_ERR(
        cdp_session.Send<dto::RuntimeEvaluateDomStateResult>("Runtime.evaluate"_t, params),
        [](auto failure) {
            return DescribeCdpFailure("failed to read dom state"_t, std::move(failure));
        }
    );
    const auto &value = result.result.value;
    const auto title = text::OptionalString(value.title).ValueOr(std::nullopt);
    const auto final_url = TRY_ERR_AS(
        String::FromBytes(value.finalUrl).Transform([](String url) {
            return CanonicalizeCapturedUrl(url);
        }),
        "Runtime.evaluate returned invalid finalUrl"_t
    );
    return DomState{
        .final_url = final_url,
        .title = title,
        .html = value.html,
    };
}

Expected<void, String> RunSiteBehavior(crawler::CdpSession &cdp_session, eng::Deadline deadline)
{
    Invariant(deadline.IsReachable(), "site behavior deadline must be reachable"_t);
    const auto budget = TRY_ERR_AS(TimeLeftMs(deadline), "timed out running site behavior"_t);

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
    TRY_MAP_ERR(cdp_session.Send<ujson::Value>("Runtime.evaluate"_t, params), [](auto failure) {
        return DescribeCdpFailure("failed to run site behavior"_t, std::move(failure));
    });
    return {};
}

class [[nodiscard]] CaptureSession final {
public:
    CaptureSession(
        Denylist &denylist, const Config &config, dns::Resolver &dns_resolver, usize url_bytes_max,
        i64 proxy_down_bytes_max, eng::subprocess::ProcessStarter &process_starter,
        eng::TaskProcessor &fs_task_processor, std::string browser_runs_root_in,
        std::string cgroup_root_path_in, std::optional<crawler::CgroupLimits> cgroup_limits_in,
        crawler::CaptureTimings timings, crawler::CrawlerTunables tunables_in,
        i64 max_archive_bytes_in, eng::Deadline deadline, crawler::RunRequest run
    )
        : denylist_(denylist), config_(config), timings_(std::move(timings)), run_(std::move(run)),
          deadline_(deadline), max_archive_bytes_(max_archive_bytes_in),
          browser_(
              dns_resolver, process_starter, fs_task_processor,
              crawler::BrowserSessionConfig{
                  .url_bytes_max = url_bytes_max,
                  .proxy_down_bytes_max = proxy_down_bytes_max,
                  .browser_runs_root_ = std::move(browser_runs_root_in),
                  .cgroup_root_path_ = std::move(cgroup_root_path_in),
                  .cgroup_limits_ = std::move(cgroup_limits_in),
                  .local_fixture_trust_db_source_path =
                      crawler::LocalFixtureTrustDbSourcePath(config.StateDir()),
                  .devtools_startup_timeout = tunables_in.devtools_startup_timeout,
                  .cdp_handshake_timeout = tunables_in.cdp_handshake_timeout,
                  .cdp_command_timeout = tunables_in.cdp_command_timeout,
                  .devtools_poll_interval = tunables_in.devtools_poll_interval,
                  .browser_stop_timeout = tunables_in.browser_stop_timeout,
                  .cdp_max_remote_payload_bytes =
                      ComputeCdpMaxRemotePayloadBytes(max_archive_bytes_in),
                  .proxy_require_auth = true,
                  .enable_local_fixture_rewrite = tunables_in.enable_local_fixture_rewrite,
                  .testsuite_loopback_ports = {},
                  .cgroup_name_prefix = "webshotd_crawler",
              }
          )
    {
    }

    ~CaptureSession()
    {
        CloseCdpForFailure();
        browser_.Close();
    }

    [[nodiscard]] Expected<CaptureWithNetwork, CaptureFailure> Capture()
    {
        auto launched = Launch();
        if (!launched) {
            auto failure_detail = browser_.BuildFailureDetail(launched.Error());
            CloseCdpForFailure();
            browser_.Close();
            return Unex(CaptureFailure{std::move(failure_detail), {}});
        }

        auto captured = CaptureAttachedTarget();
        if (!captured) {
            auto failure_detail = browser_.BuildFailureDetail(captured.Error());
            if (tracker_) {
                const auto tracker_failure = tracker_->FailureReason();
                if (tracker_failure)
                    failure_detail = text::Format(
                        "{}, tracker_failure={}", failure_detail, *tracker_failure
                    );
            }
            if (const auto proxy_failure = browser_.ProxyFailureReason())
                failure_detail = text::Format(
                    "{}, proxy_failure={}", failure_detail, *proxy_failure
                );
            auto seed_probe = CurrentSeedProbe();
            CloseCdpForFailure();
            browser_.Close();
            return Unex(
                CaptureFailure{
                    std::move(failure_detail),
                    std::move(seed_probe),
                }
            );
        }
        auto value = GrabValueOf(captured);
        return CaptureWithNetwork{
            .exchange = std::move(value),
            .proxy_down_bytes = browser_.ProxyDownBytes(),
        };
    }

private:
    template <typename... Args>
    [[nodiscard]] Expected<void, String> SendSessionVoid(const String &method, Args &&...args) const
    {
        auto result = GetSession().SendVoid(method, std::forward<Args>(args)...);
        if (!result)
            return Unex(DescribeCdpFailure(text::Format("{} failed", method), result.Error()));
        return {};
    }

    void NoteEventProgress()
    {
        auto progress = event_progress_.Lock();
        progress->version++;
        progress->cv.NotifyAll();
    }

    [[nodiscard]] std::optional<String> CurrentWaitFailure() const
    {
        {
            const auto failure = interception_failure_.Lock();
            if (*failure)
                return *failure;
        }
        if (tracker_) {
            if (const auto tracker_failure = tracker_->FailureReason())
                return tracker_failure;
        }
        return {};
    }

    template <typename Predicate>
    [[nodiscard]] Expected<void, String>
    WaitForPredicate(Predicate &&predicate, String timeout_message)
    {
        while (!std::invoke(predicate)) {
            if (const auto failure = CurrentWaitFailure())
                return Unex(*failure);
            auto progress = event_progress_.UniqueLock();
            const auto version = progress->version;
            progress.GetLock().unlock();
            if (std::invoke(predicate))
                return {};
            if (const auto failure = CurrentWaitFailure())
                return Unex(*failure);
            progress.GetLock().lock();
            if (progress->version != version)
                continue;
            if (!progress->cv.WaitUntil(progress.GetLock(), deadline_, [&]() {
                    return progress->version != version;
                })) {
                progress.GetLock().unlock();
                if (std::invoke(predicate))
                    return {};
                if (const auto failure = CurrentWaitFailure())
                    return Unex(*failure);
                return Unex(timeout_message);
            }
        }
        if (const auto failure = CurrentWaitFailure())
            return Unex(*failure);
        return {};
    }

    [[nodiscard]] Expected<void, String> WaitForIdle(chrono::seconds idle)
    {
        while (!GetPageTracker().IsIdleFor(idle)) {
            if (const auto failure = CurrentWaitFailure())
                return Unex(*failure);
            auto progress = event_progress_.UniqueLock();
            const auto version = progress->version;
            const auto idle_deadline = eng::Deadline::FromTimePoint(
                GetPageTracker().IdleDeadline(idle)
            );
            const auto wait_deadline = PickEarlierDeadline(deadline_, idle_deadline);
            progress.GetLock().unlock();
            if (GetPageTracker().IsIdleFor(idle))
                return {};
            if (const auto failure = CurrentWaitFailure())
                return Unex(*failure);
            progress.GetLock().lock();
            if (progress->version != version)
                continue;
            if (!progress->cv.WaitUntil(progress.GetLock(), wait_deadline, [&]() {
                    return progress->version != version;
                })) {
                progress.GetLock().unlock();
                if (GetPageTracker().IsIdleFor(idle))
                    return {};
                if (const auto failure = CurrentWaitFailure())
                    return Unex(*failure);
                if (deadline_.IsReached())
                    return Unex("timed out waiting for network idle"_t);
            }
        }
        if (const auto failure = CurrentWaitFailure())
            return Unex(*failure);
        return {};
    }

    void RunEventLoop()
    {
        while (!stopping_event_loop_.load()) {
            auto event = GetSession().WaitEvent(deadline_, "timed out waiting for cdp event"_t);
            if (!event) {
                if (!stopping_event_loop_.load()) {
                    NoteInterceptionFailure(
                        DescribeCdpFailure("cdp event loop failed"_t, event.Error())
                    );
                    NoteEventProgress();
                }
                return;
            }
            HandleSessionEvent(*event);
            NoteEventProgress();
            if (CurrentWaitFailure())
                return;
        }
    }

    void StartEventLoop()
    {
        stopping_event_loop_.store(false);
        event_task_ = std::move(eng::CriticalAsyncNoSpan([this]() { RunEventLoop(); })).AsTask();
    }

    void StopEventLoop()
    {
        stopping_event_loop_.store(true);
        if (!event_task_.IsValid())
            return;
        event_task_.RequestCancel();
        const eng::TaskCancellationBlocker blocker;
        static_cast<void>(event_task_.WaitNothrow());
        event_task_ = {};
    }

    [[nodiscard]] Expected<void, String> Launch()
    {
        TRY(browser_.Launch());
        browser_.MarkPhase("connect_cdp");
        cdp_ = TRY(browser_.ConnectCdp(deadline_));
        page_session_ = std::make_unique<crawler::BrowserPageSession>(GetCdpClient());

        TRY(page_session_->AttachFreshTarget([this](std::string_view phase) {
            browser_.MarkPhase(phase);
        }));
        tracker_ = std::make_unique<PageTracker>(
            page_session_->SessionId(), page_session_->TargetId()
        );
        StartEventLoop();
        return {};
    }

    [[nodiscard]] Expected<crawler::CapturedExchange, String> CaptureAttachedTarget()
    {
        TRY(page_session_->EnableBaseDomains([this](std::string_view phase) {
            browser_.MarkPhase(phase);
        }));

        browser_.MarkPhase("enable_fetch");
        dto::FetchEnableParams fetch_params;
        fetch_params.handleAuthRequests = true;
        TRY(SendSessionVoid("Fetch.enable"_t, fetch_params));

        browser_.MarkPhase("disable_cache");
        dto::NetworkSetCacheDisabledParams cache_params;
        cache_params.cacheDisabled = true;
        TRY(SendSessionVoid("Network.setCacheDisabled"_t, cache_params));

        browser_.MarkPhase("bypass_service_worker");
        dto::NetworkSetBypassServiceWorkerParams service_worker_params;
        service_worker_params.bypass = true;
        TRY(SendSessionVoid("Network.setBypassServiceWorker"_t, service_worker_params));

        browser_.MarkPhase("set_extra_headers");
        dto::NetworkSetExtraHTTPHeadersParams header_params;
        header_params.headers.extra.emplace(
            "Accept-Language", std::string(crawler::kBrowserAcceptLanguage)
        );
        TRY(SendSessionVoid("Network.setExtraHTTPHeaders"_t, header_params));

        browser_.MarkPhase("get_frame_tree");
        const auto frame_tree = TRY_MAP_ERR(
            GetSession().Send<dto::PageGetFrameTreeResult>("Page.getFrameTree"_t),
            [](auto failure) {
                return DescribeCdpFailure("Page.getFrameTree failed"_t, std::move(failure));
            }
        );
        GetPageTracker().SetMainFrameId(*String::FromBytes(frame_tree.frameTree.frame.id));

        browser_.MarkPhase("navigate");
        dto::PageNavigateParams navigate_params;
        navigate_params.url = run_.seed_url.ToBytes();
        GetPageTracker().BeginSeedNavigation(run_.seed_url);
        const auto navigate_result = TRY_MAP_ERR(
            GetSession().Send<dto::PageNavigateResult>("Page.navigate"_t, navigate_params),
            [](auto failure) {
                return DescribeCdpFailure("Page.navigate failed"_t, std::move(failure));
            }
        );
        ENSURE(!navigate_result.errorText, *String::FromBytes(*navigate_result.errorText));
        GetPageTracker().SetExpectedMainLoaderId(StringOrNull(navigate_result.loaderId));

        browser_.MarkPhase("wait_for_load");
        TRY(WaitForPredicate(
            [this]() { return GetPageTracker().IsLoadedOrFailed(); },
            "timed out waiting for page load"_t
        ));
        if (timings_.post_load_delay > 0s) {
            browser_.MarkPhase("post_load_delay");
            const auto phase_deadline = PickEarlierDeadline(
                deadline_, eng::Deadline::FromDuration(timings_.post_load_delay)
            );
            TRY_ERR_AS(
                SleepUntilDeadline(phase_deadline), "timed out waiting for post-load delay"_t
            );
            browser_.MarkPhase("post_load_delay_done");
        }
        if (timings_.behavior_timeout > 0s) {
            browser_.MarkPhase("run_site_behavior");
            browser_.MarkPhase("run_site_behavior_runtime_evaluate");
            const auto behavior_deadline = PickEarlierDeadline(
                deadline_, eng::Deadline::FromDuration(timings_.behavior_timeout)
            );
            TRY(RunSiteBehavior(GetSession(), behavior_deadline));
            browser_.MarkPhase("run_site_behavior_done");
        }
        if (timings_.net_idle_wait > 0s) {
            browser_.MarkPhase("wait_for_idle");
            browser_.MarkPhase("wait_for_idle_wait");
            TRY(WaitForIdle(timings_.net_idle_wait));
            browser_.MarkPhase("wait_for_idle_done");
        }
        if (timings_.page_extra_delay > 0s) {
            browser_.MarkPhase("page_extra_delay");
            const auto phase_deadline = PickEarlierDeadline(
                deadline_, eng::Deadline::FromDuration(timings_.page_extra_delay)
            );
            TRY_ERR_AS(
                SleepUntilDeadline(phase_deadline), "timed out waiting for extra page delay"_t
            );
            browser_.MarkPhase("page_extra_delay_done");
        }
        browser_.MarkPhase("wait_for_main_document");
        browser_.MarkPhase("wait_for_main_document_wait");
        TRY(WaitForPredicate(
            [this]() { return GetPageTracker().HasMainDocumentOrFailure(); },
            "timed out waiting for main document response"_t
        ));
        browser_.MarkPhase("wait_for_main_document_done");

        browser_.MarkPhase("read_dom_state");
        browser_.MarkPhase("read_dom_state_runtime_evaluate");
        auto dom_state = TRY(ReadDomState(GetSession()));
        browser_.MarkPhase("read_dom_state_done");
        RetainedBodyBudget budget{max_archive_bytes_, 0_i64};
        browser_.MarkPhase("read_main_body");
        auto body = TRY(GetPageTracker().ReadBody(GetSession(), budget, dom_state.html));
        browser_.MarkPhase("read_resources");
        auto resources = TRY(GetPageTracker().ReadResources(GetSession(), budget));

        StopEventLoop();
        TRY(page_session_->Close([this](std::string_view phase) { browser_.MarkPhase(phase); }));
        page_session_.reset();

        browser_.MarkPhase("build_exchange_start");
        LOG_INFO() << std::format(
            "captureViaProxy building exchange for {} (body_bytes={}, resources={})", run_.seed_url,
            body.size(), resources.size()
        );
        auto exchange = GetPageTracker().BuildExchange(
            std::move(dom_state.final_url), std::move(dom_state.title), std::move(body),
            std::move(resources)
        );
        browser_.MarkPhase("build_exchange_done");
        LOG_INFO() << std::format(
            "captureViaProxy built exchange for {} (status={}, resources={}, body_bytes={})",
            run_.seed_url, exchange.status_code, exchange.resources.size(), exchange.body.size()
        );
        tracker_.reset();
        cdp_.reset();

        browser_.MarkPhase("close_browser_success");
        browser_.MarkPhase("before_browser_close");
        LOG_INFO() << std::format("captureViaProxy closing browser for {}", run_.seed_url);
        browser_.Close();
        if (const auto proxy_failure = browser_.ProxyFailureReason()) {
            if (!IsSuccessfulMainDocumentExchange(exchange))
                return Unex(*proxy_failure);
            LOG_WARNING() << std::format(
                "Ignoring late proxy failure after successful main document capture for {} "
                "(status={}): {}",
                run_.seed_url, exchange.status_code, *proxy_failure
            );
        }
        LOG_INFO() << std::format("captureViaProxy returning capture for {}", run_.seed_url);
        return exchange;
    }

    void CloseCdpForFailure()
    {
        StopEventLoop();
        if (page_session_) {
            if (const auto closed_page = page_session_->Close(); !closed_page) {
                LOG_WARNING() << std::format(
                    "Suppressing page session close failure during capture cleanup: {}",
                    closed_page.Error()
                );
            }
            page_session_.reset();
        }
        if (!cdp_)
            return;

        if (auto closed = cdp_->Close(); !closed) {
            LOG_WARNING() << std::format(
                "Suppressing CDP close failure during capture cleanup: code={}{}",
                NumericCast<int>(closed.Error().code),
                closed.Error().detail ? std::format(", detail={}", *closed.Error().detail)
                                      : std::string{}
            );
        }
        cdp_.reset();
    }

    [[nodiscard]] std::optional<crawler::SeedPageProbe> CurrentSeedProbe() const
    {
        if (!tracker_)
            return {};
        return tracker_->CurrentSeedProbe();
    }

    [[nodiscard]] crawler::CdpClient &GetCdpClient() const
    {
        Invariant(cdp_, "cdp client is not connected"_t);
        return *cdp_;
    }

    [[nodiscard]] PageTracker &GetPageTracker() const
    {
        Invariant(tracker_, "page tracker is not attached"_t);
        return *tracker_;
    }

    [[nodiscard]] crawler::CdpSession &GetSession() const
    {
        Invariant(page_session_, "cdp session is not attached"_t);
        return page_session_->GetSession();
    }

    void NoteInterceptionFailure(String reason)
    {
        auto tracker_reason = reason;
        {
            auto failure = interception_failure_.UniqueLock();
            if (*failure)
                return;
            *failure = std::move(reason);
        }
        if (tracker_)
            tracker_->Fail(std::move(tracker_reason));
    }

    void HandleSessionEvent(const crawler::CdpEvent &event)
    {
        if (event.method == "Fetch.authRequired"_t) {
            HandleFetchAuthRequired(event);
            return;
        }
        if (event.method == "Fetch.requestPaused"_t) {
            HandleFetchRequestPaused(event);
            return;
        }
        if (tracker_)
            tracker_->HandleEvent(event);
    }

    void HandleFetchAuthRequired(const crawler::CdpEvent &event)
    {
        const auto auth_required = ParseEventParams<dto::FetchAuthRequiredEvent>(event);
        if (!auth_required) {
            NoteInterceptionFailure(auth_required.Error());
            return;
        }

        dto::FetchContinueWithAuthParams params;
        params.requestId = auth_required->requestId;

        dto::FetchAuthChallengeResponse auth_challenge_response;
        const auto is_proxy_challenge = !auth_required->authChallenge.source ||
                                        *auth_required->authChallenge.source == "Proxy";
        if (is_proxy_challenge) {
            auth_challenge_response.response = "ProvideCredentials";
            auth_challenge_response.username = browser_.RunId();
            auth_challenge_response.password = "x";
        } else {
            auth_challenge_response.response = "Default";
        }
        params.authChallengeResponse = std::move(auth_challenge_response);

        const auto continued = GetSession().SendVoid("Fetch.continueWithAuth"_t, params);
        if (!continued)
            NoteInterceptionFailure(
                DescribeCdpFailure("Fetch.continueWithAuth failed"_t, continued.Error())
            );
    }

    void HandleFetchRequestPaused(const crawler::CdpEvent &event)
    {
        const auto paused = ParseEventParams<dto::FetchRequestPausedEvent>(event);
        if (!paused) {
            NoteInterceptionFailure(paused.Error());
            return;
        }

        const auto request_text = String::FromBytes(paused->request.url);
        if (!request_text) {
            NoteInterceptionFailure("Fetch.requestPaused contained invalid request url"_t);
            return;
        }

        const auto url = AccessPolicyUrlFromText(*request_text);
        if (!url) {
            dto::FetchContinueRequestParams params;
            params.requestId = paused->requestId;
            const auto continued = GetSession().SendVoid("Fetch.continueRequest"_t, params);
            if (!continued)
                NoteInterceptionFailure(
                    DescribeCdpFailure("Fetch.continueRequest failed"_t, continued.Error())
                );
            return;
        }

        const auto decision = EvaluateAccessPolicy(denylist_, config_, *url);
        if (!decision) {
            NoteInterceptionFailure(decision.Error());
            return;
        }

        if (decision->allowed) {
            dto::FetchContinueRequestParams params;
            params.requestId = paused->requestId;
            const auto continued = GetSession().SendVoid("Fetch.continueRequest"_t, params);
            if (!continued)
                NoteInterceptionFailure(
                    DescribeCdpFailure("Fetch.continueRequest failed"_t, continued.Error())
                );
            return;
        }

        const auto body = text::Format("{}\n", AccessDecisionMessage(decision->reason));
        dto::FetchFulfillRequestParams params{
            .requestId = paused->requestId,
            .responseCode = 403,
            .responseHeaders = BuildBlockedFetchHeaders(body.SizeBytes()),
            .body = us::crypto::base64::Base64Encode(body.ToBytes()),
            .responsePhrase = "Forbidden",
        };
        const auto fulfilled = GetSession().SendVoid("Fetch.fulfillRequest"_t, params);
        if (!fulfilled)
            NoteInterceptionFailure(
                DescribeCdpFailure("Fetch.fulfillRequest failed"_t, fulfilled.Error())
            );
    }

    struct EventProgressState final {
        eng::ConditionVariable cv;
        i64 version{0};
    };

    Denylist &denylist_;
    const Config &config_;
    crawler::CaptureTimings timings_;
    crawler::RunRequest run_;
    eng::Deadline deadline_;
    i64 max_archive_bytes_;
    crawler::BrowserSession browser_;
    std::unique_ptr<crawler::CdpClient> cdp_;
    std::unique_ptr<PageTracker> tracker_;
    std::unique_ptr<crawler::BrowserPageSession> page_session_;
    us::concurrent::Variable<EventProgressState> event_progress_;
    eng::Task event_task_;
    std::atomic<bool> stopping_event_loop_{false};
    us::concurrent::Variable<std::optional<String>> interception_failure_;
};

[[nodiscard]] Expected<CaptureWithNetwork, CaptureFailure> CaptureViaProxy(
    Denylist &denylist, const Config &config, dns::Resolver &dns_resolver, usize url_bytes_max,
    i64 proxy_down_bytes_max, eng::subprocess::ProcessStarter &process_starter,
    eng::TaskProcessor &fs_task_processor, const std::string &browser_runs_root,
    const std::string &cgroup_root_path, std::optional<crawler::CgroupLimits> cgroup_limits,
    crawler::CaptureTimings timings, const crawler::CrawlerTunables &tunables,
    i64 max_archive_bytes, eng::Deadline deadline, const crawler::RunRequest &run
)
{
    auto session = CaptureSession(
        denylist, config, dns_resolver, url_bytes_max, proxy_down_bytes_max, process_starter,
        fs_task_processor, std::string(browser_runs_root), std::string(cgroup_root_path),
        std::move(cgroup_limits), std::move(timings), tunables, max_archive_bytes, deadline,
        crawler::RunRequest{.seed_url = run.seed_url}
    );
    return session.Capture();
}

[[nodiscard]] CrawlerRunArtifacts ExecuteRun(
    Denylist &denylist, const Config &config, dns::Resolver &dns_resolver,
    eng::subprocess::ProcessStarter &process_starter, eng::TaskProcessor &fs_task_processor,
    const std::string &browser_runs_root, const std::string &cgroup_root_path,
    std::optional<crawler::CgroupLimits> cgroup_limits, const crawler::CaptureTimings &timings,
    const crawler::CrawlerTunables &tunables, i64 max_archive_bytes,
    i64 network_down_bytes_ratio_max, eng::Deadline deadline, const crawler::RunRequest &run
)
{
    CrawlerRunArtifacts out;
    out.attempt.exited = true;
    try {
        LOG_INFO() << std::format("crawler executeRun starting for {}", run.seed_url);

        const auto fail_artifact =
            [&out](const crawler::ArtifactFailure &failure) -> CrawlerRunArtifacts {
            auto failure_detail_opt = String::FromBytes(failure.detail)
                                          .Transform([](String s) -> std::optional<String> {
                                              return {std::move(s)};
                                          })
                                          .ValueOr(std::nullopt);
            out.attempt.exit_code = 9;
            out.attempt.wacz_exists = false;
            out.attempt.seed_probe.reset();
            out.attempt.failure_detail = std::move(failure_detail_opt);
            out.stdout_log.clear();
            out.stderr_log = failure.detail + "\n";
            out.wacz.reset();
            out.pages_jsonl.reset();
            out.content_sha256.reset();
            return out;
        };

        const auto max_down_bytes = [&]() -> i64 {
            const auto max = max_archive_bytes;
            const auto ratio = network_down_bytes_ratio_max;
            const auto max_i64 = std::numeric_limits<i64>::max();
            if (ratio > max_i64 / max)
                return max_i64;
            return ratio * max;
        }();

        auto captured = CaptureViaProxy(
            denylist, config, dns_resolver, config.UrlBytesMax(), max_down_bytes, process_starter,
            fs_task_processor, browser_runs_root, cgroup_root_path, std::move(cgroup_limits),
            timings, tunables, max_archive_bytes, deadline, run
        );
        if (!captured) {
            constexpr std::string_view size_limit_prefix = "size_limit:";
            constexpr std::string_view net_limit_prefix = "net_limit:";
            if (captured.Error().detail.StartsWith(size_limit_prefix)) {
                auto detail_text = captured.Error().detail.View();
                detail_text.remove_prefix(size_limit_prefix.size());
                if (!detail_text.empty() && detail_text.front() == ' ')
                    detail_text.remove_prefix(1);
                auto parsed = String::FromBytes(std::string(detail_text));
                out.attempt.exit_code = us::utils::UnderlyingValue(
                    crawler::CrawlerExitCode::kSizeLimit
                );
                out.attempt.wacz_exists = false;
                out.attempt.seed_probe = captured.Error().seed_probe;
                out.attempt.failure_detail.reset();
                if (parsed)
                    out.attempt.failure_detail = GrabValueOf(parsed);
            } else if (captured.Error().detail.StartsWith(net_limit_prefix)) {
                out.attempt.exit_code = us::utils::UnderlyingValue(
                    crawler::CrawlerExitCode::kFailedLimit
                );
                out.attempt.wacz_exists = false;
                out.attempt.seed_probe = captured.Error().seed_probe;
                out.attempt.failure_detail = captured.Error().detail;
            } else {
                out.attempt.exit_code = 9;
                out.attempt.wacz_exists = false;
                out.attempt.seed_probe = captured.Error().seed_probe;
                out.attempt.failure_detail = captured.Error().detail;
            }
            out.stdout_log.clear();
            out.stderr_log = captured.Error().detail.ToBytes() + "\n";
            out.wacz.reset();
            out.pages_jsonl.reset();
            out.content_sha256.reset();
            out.replay_url.reset();
            return out;
        }

        auto exchange = std::move(captured->exchange);
        const auto proxy_down_bytes = captured->proxy_down_bytes;
        LOG_INFO() << std::format(
            "crawler captureViaProxy finished for {} with status={}", run.seed_url,
            exchange.status_code
        );
        auto pages = crawler::BuildPagesJsonl(exchange);
        LOG_INFO() << std::format("crawler buildPagesJsonl finished for {}", run.seed_url);
        out.content_sha256 = crawler::ComputeContentSha256(exchange);
        {
            auto log = crawler::BuildSuccessStdoutLog(
                run, exchange, 0_i64, crawler::ReusedBrowser::kNo
            );
            if (!log)
                Invariant(*String::FromBytes(log.Error().detail));
            out.stdout_log = GrabValueOf(log);
        }
        out.stderr_log.clear();
        auto warc = crawler::BuildWarc(exchange);
        if (!warc)
            return fail_artifact(warc.Error());
        LOG_INFO() << std::format("crawler buildWarc finished for {}", run.seed_url);
        auto wacz = crawler::BuildWacz(
            run, pages, GrabValueOf(warc), out.stdout_log, out.stderr_log
        );
        if (!wacz)
            return fail_artifact(wacz.Error());
        LOG_INFO() << std::format(
            "crawler buildWacz finished for {} (wacz_bytes={}, pages_bytes={})", run.seed_url,
            wacz->size(), pages.size()
        );

        const auto wacz_bytes = ssize(wacz);
        if (wacz_bytes > max_archive_bytes) {
            const auto max_archive_mi_b = max_archive_bytes / (1024_i64 * 1024_i64);
            const auto detail = text::Format(
                "archive bytes {} exceeded size limit {} MiB", wacz_bytes, max_archive_mi_b
            );
            out.attempt.exit_code = us::utils::UnderlyingValue(
                crawler::CrawlerExitCode::kSizeLimit
            );
            out.attempt.wacz_exists = false;
            out.attempt.seed_probe = crawler::SeedPageProbe{
                .status = Raw(exchange.status_code),
                .load_state = 0,
            };
            out.attempt.failure_detail = detail;
            out.wacz.reset();
            out.pages_jsonl.reset();
            out.content_sha256.reset();
            out.replay_url.reset();
            out.stderr_log += detail.ToBytes() + "\n";
            return out;
        }

        const auto max_down_by_final = [&]() -> i64 {
            const auto ratio = network_down_bytes_ratio_max;
            const auto max_i64 = std::numeric_limits<i64>::max();
            if (wacz_bytes <= 0_i64)
                return 0_i64;
            if (ratio > max_i64 / wacz_bytes)
                return max_i64;
            return ratio * wacz_bytes;
        }();
        if (proxy_down_bytes > max_down_by_final) {
            const auto detail = text::Format(
                "net_limit: proxy downstream bytes {} exceeded post-run limit {}", proxy_down_bytes,
                max_down_by_final
            );
            out.attempt.exit_code = us::utils::UnderlyingValue(
                crawler::CrawlerExitCode::kFailedLimit
            );
            out.attempt.wacz_exists = false;
            out.attempt.seed_probe = crawler::SeedPageProbe{
                .status = Raw(exchange.status_code),
                .load_state = 0,
            };
            out.attempt.failure_detail = detail;
            out.wacz.reset();
            out.pages_jsonl.reset();
            out.content_sha256.reset();
            out.replay_url.reset();
            out.stderr_log += detail.ToBytes() + "\n";
            return out;
        }

        const i64 exit_code{exchange.status_code >= 400_i64 ? 9_i64 : 0_i64};
        const i64 load_state{exit_code != 0_i64 || exchange.status_code >= 400_i64 ? 0_i64 : 2_i64};
        if (exchange.status_code >= 400_i64) {
            out.attempt.failure_detail = text::Format(
                "seed returned HTTP {}", exchange.status_code
            );
        }

        LOG_INFO() << std::format(
            "crawler executeRun finished for {} (exit_code={}, wacz_exists=true)", run.seed_url,
            exit_code
        );

        out.attempt.exit_code = NumericCast<int>(exit_code);
        out.attempt.wacz_exists = true;
        out.attempt.seed_probe = crawler::SeedPageProbe{
            .status = Raw(exchange.status_code),
            .load_state = Raw(load_state),
        };
        out.wacz = GrabValueOf(wacz);
        out.pages_jsonl = std::move(pages);
        out.replay_url = exchange.final_url;
        return out;
    } catch (const std::exception &e) {
        if (eng::current_task::IsCancelRequested())
            throw;
        out.attempt.exit_code = 9;
        out.attempt.wacz_exists = false;
        out.attempt.seed_probe.reset();
        {
            auto parsed = String::FromBytes(e.what());
            if (parsed)
                out.attempt.failure_detail = GrabValueOf(parsed);
            else
                out.attempt.failure_detail.reset();
        }
        out.stdout_log.clear();
        out.stderr_log = std::string(e.what()) + "\n";
        out.wacz.reset();
        out.pages_jsonl.reset();
        out.content_sha256.reset();
        out.replay_url.reset();
        return out;
    }
}

} // namespace

CrawlerRunner::CrawlerRunner(
    Denylist &denylist, const Config &config, dns::Resolver &dns_resolver,
    eng::subprocess::ProcessStarter &process_starter, chrono::seconds run_timeout,
    eng::TaskProcessor &fs_task_processor, std::string state_dir,
    std::optional<crawler::CgroupLimits> limits, i64 max_archive_bytes,
    crawler::CaptureTimings timings, crawler::CrawlerTunables tunables,
    i64 network_down_bytes_ratio_max
)
    : denylist_(denylist), config_(config), dns_resolver_(dns_resolver),
      process_starter_(process_starter), fs_task_processor_(fs_task_processor),
      run_timeout_(run_timeout),
      browser_runs_root_(crawler::BuildBrowserRunsRoot(std::move(state_dir))),
      cgroup_root_path_(
          limits ? crawler::ResolveDelegatedCgroupRootPath(fs_task_processor) : std::string()
      ),
      cgroup_limits_(std::move(limits)), max_archive_bytes_(max_archive_bytes),
      timings_(std::move(timings)), tunables_(std::move(tunables)),
      network_down_bytes_ratio_max_(network_down_bytes_ratio_max)
{
}

CrawlerRunArtifacts CrawlerRunner::Run(const String &seed_url) const
{
    const auto deadline = eng::Deadline::FromDuration(run_timeout_);
    return ExecuteRun(
        denylist_, config_, dns_resolver_, process_starter_, fs_task_processor_, browser_runs_root_,
        cgroup_root_path_, cgroup_limits_, timings_, tunables_, max_archive_bytes_,
        network_down_bytes_ratio_max_, deadline, crawler::RunRequest{.seed_url = seed_url}
    );
}

} // namespace ws
