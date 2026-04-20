#include "browser_probe_handler.hpp"
/**
 * @file
 * @brief Monitor-only handler that drives Chromium for browser assertions in tests.
 */
#include "config.hpp"
#include "crawler/browser_session.hpp"
#include "crawler/cdp_client.hpp"
#include "deadline_utils.hpp"
#include "grab_value.hpp"
#include "http_utils.hpp"
#include "schema/browser_probe.hpp"
#include "schema/cdp.hpp"
#include "text.hpp"
#include "try.hpp"
#include "userver_expected.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <userver/clients/dns/component.hpp>
#include <userver/components/component.hpp>
#include <userver/components/process_starter.hpp>
#include <userver/engine/task/current_task.hpp>
#include <userver/formats/json.hpp>
#include <userver/formats/json/value.hpp>
#include <userver/logging/log.hpp>
#include <userver/server/http/http_method.hpp>
#include <userver/server/http/http_request.hpp>
#include <userver/server/http/http_response.hpp>
#include <userver/server/http/http_status.hpp>
#include <userver/yaml_config/merge_schemas.hpp>

using namespace text::literals;
using namespace std::chrono_literals;

namespace chrono = std::chrono;

namespace v1 {
namespace {

using crawler::describeCdpFailure;

struct [[nodiscard]] ProbeConfig final {
    const Config &svcConfig;
    us::clients::dns::Resolver &dnsResolver;
    eng::subprocess::ProcessStarter &processStarter;
    chrono::milliseconds requestTimeout;
    chrono::milliseconds devtoolsStartupTimeout;
    chrono::milliseconds cdpHandshakeTimeout;
    chrono::milliseconds cdpCommandTimeout;
    chrono::milliseconds devtoolsPollInterval;
    chrono::milliseconds browserStopTimeout;
    i64 cdpMaxRemotePayloadBytes;
    bool localFixtureRewrite;
};

[[nodiscard]] Expected<dto::BrowserProbeRequest, String> parseProbeRequest(std::string_view body)
{
    const auto request = TRY(
        exu::json::parse<dto::BrowserProbeRequest>(body, "invalid request body"_t)
    );
    if (request.url.empty() || request.wait_expression.empty() || request.timeout_ms <= 0)
        return Unex("invalid request body"_t);
    return request;
}

[[nodiscard]] Expected<json::Value, String>
evaluateExpression(crawler::CdpSession &cdpSession, const String &expression)
{
    dto::RuntimeEvaluateParams params{
        .expression = std::to_string(expression),
        .returnByValue = true,
        .awaitPromise = true,
    };

    const auto result = cdpSession.send<json::Value>("Runtime.evaluate"_t, params);
    if (!result)
        return Unex(describeCdpFailure("Runtime.evaluate failed"_t, result.error()));

    const auto exception = (*result)["exceptionDetails"];
    if (!exception.IsMissing()) {
        return Unex(
            text::format(
                "Runtime.evaluate threw: {}",
                TRY(
                    exu::json::stringify(exception, "Runtime.evaluate returned invalid exception"_t)
                )
            )
        );
    }

    const auto resultValue = (*result)["result"];
    if (resultValue.IsMissing())
        return Unex("Runtime.evaluate missing result"_t);

    const auto value = resultValue["value"];
    if (!value.IsMissing())
        return value;

    return exu::json::parse<json::Value>("null", "Runtime.evaluate missing value"_t);
}

template <typename T>
[[nodiscard]] Expected<T, String>
evaluateExpressionAs(crawler::CdpSession &cdpSession, const String &expression)
{
    return exu::json::as<T>(
        TRY(evaluateExpression(cdpSession, expression)), "expression returned invalid shape"_t
    );
}

[[nodiscard]] Expected<bool, String>
evaluateBoolExpression(crawler::CdpSession &cdpSession, const String &expression)
{
    return TRY(evaluateExpressionAs<bool>(cdpSession, expression));
}

[[nodiscard]] Expected<std::optional<dto::BrowserProbeFrameState>, String>
evaluateFrameExpression(crawler::CdpSession &cdpSession, const String &expression)
{
    const auto value = evaluateExpression(cdpSession, expression);
    if (!value)
        return Unex(value.error());
    if (json::ToString(*value) == "null")
        return std::optional<dto::BrowserProbeFrameState>{};

    const auto parsed = exu::json::as<dto::BrowserProbeFrameState>(
        *value, "expression returned invalid shape"_t
    );
    if (!parsed)
        return Unex(parsed.error());
    return std::optional<dto::BrowserProbeFrameState>{*parsed};
}

[[nodiscard]] Expected<void, String> handleProbeEvent(
    const crawler::CdpEvent &event, std::vector<std::string> &console,
    std::vector<std::string> &pageErrors
)
{
    if (!event.params)
        return {};
    if (event.method == "Runtime.consoleAPICalled"_t) {
        console.push_back(
            TRY(exu::json::stringify(event.params->extra, "invalid console payload"_t))
        );
        return {};
    }
    if (event.method == "Runtime.exceptionThrown"_t) {
        pageErrors.push_back(
            TRY(exu::json::stringify(event.params->extra, "invalid exception payload"_t))
        );
        return {};
    }
    return {};
}

[[nodiscard]] Expected<void, String> drainProbeEvents(
    crawler::CdpSession &cdpSession, std::vector<std::string> &console,
    std::vector<std::string> &pageErrors
)
{
    for (const auto &event : cdpSession.drainAvailableEvents())
        TRY(handleProbeEvent(event, console, pageErrors));
    return {};
}

[[nodiscard]] Expected<void, String> waitForExpression(
    crawler::CdpSession &cdpSession, const String &expression, eng::Deadline deadline,
    chrono::milliseconds recheckInterval, std::vector<std::string> &console,
    std::vector<std::string> &pageErrors
)
{
    std::optional<String> lastError;

    const auto updateMatchState = [&]() -> Expected<bool, String> {
        const auto matched = evaluateBoolExpression(cdpSession, expression);
        if (matched)
            return *matched;
        lastError = matched.error();
        return false;
    };

    if (const auto matched = updateMatchState(); matched) {
        if (*matched)
            return {};
    } else {
        return Unex(matched.error());
    }

    while (!deadline.IsReached()) {
        TRY(drainProbeEvents(cdpSession, console, pageErrors));
        if (const auto matched = updateMatchState(); matched) {
            if (*matched)
                return {};
        } else {
            return Unex(matched.error());
        }

        const auto eventDeadline = eng::Deadline::FromDuration(
            std::min(
                chrono::duration_cast<chrono::milliseconds>(deadline.TimeLeft()), recheckInterval
            )
        );
        auto event = cdpSession.waitEvent(eventDeadline, "timed out waiting for expression"_t);
        if (!event) {
            if (event.error().code == crawler::CdpError::kTimeout)
                continue;
            return Unex(describeCdpFailure("wait for cdp event failed"_t, event.error()));
        }
        TRY(handleProbeEvent(*event, console, pageErrors));
    }

    if (lastError)
        return Unex(text::format("timed out waiting for expression: {}", *lastError));
    return Unex("timed out waiting for expression"_t);
}

[[nodiscard]] Expected<void, String> settleProbeEvents(
    crawler::CdpSession &cdpSession, eng::Deadline deadline, chrono::milliseconds recheckInterval,
    std::vector<std::string> &console, std::vector<std::string> &pageErrors
)
{
    const auto settleWindow = std::max(recheckInterval * 2, 250ms);
    const auto settleDeadline = pickEarlierDeadline(
        deadline, eng::Deadline::FromDuration(settleWindow)
    );

    while (!settleDeadline.IsReached()) {
        TRY(drainProbeEvents(cdpSession, console, pageErrors));

        const auto eventDeadline = eng::Deadline::FromDuration(
            std::min(
                chrono::duration_cast<chrono::milliseconds>(settleDeadline.TimeLeft()),
                recheckInterval
            )
        );
        auto event = cdpSession.waitEvent(eventDeadline, "timed out waiting for probe settle"_t);
        if (!event) {
            if (event.error().code == crawler::CdpError::kTimeout)
                continue;
            return Unex(describeCdpFailure("wait for cdp event failed"_t, event.error()));
        }
        TRY(handleProbeEvent(*event, console, pageErrors));
    }

    return {};
}

[[nodiscard]] Expected<dto::BrowserProbeFrameState, String> waitForFrameExpression(
    crawler::CdpSession &cdpSession, const String &expression, eng::Deadline deadline,
    chrono::milliseconds recheckInterval, std::vector<std::string> &console,
    std::vector<std::string> &pageErrors
)
{
    std::optional<String> lastError;

    const auto updateFrameState =
        [&]() -> Expected<std::optional<dto::BrowserProbeFrameState>, String> {
        auto frame = evaluateFrameExpression(cdpSession, expression);
        if (frame)
            return frame;
        lastError = frame.error();
        return std::optional<dto::BrowserProbeFrameState>{};
    };

    if (auto frame = updateFrameState(); frame) {
        if (*frame)
            return **frame;
    } else {
        return Unex(frame.error());
    }

    while (!deadline.IsReached()) {
        TRY(drainProbeEvents(cdpSession, console, pageErrors));
        if (auto frame = updateFrameState(); frame) {
            if (*frame)
                return **frame;
        } else {
            return Unex(frame.error());
        }

        const auto eventDeadline = eng::Deadline::FromDuration(
            std::min(
                chrono::duration_cast<chrono::milliseconds>(deadline.TimeLeft()), recheckInterval
            )
        );
        auto event = cdpSession.waitEvent(
            eventDeadline, "timed out waiting for frame expression"_t
        );
        if (!event) {
            if (event.error().code == crawler::CdpError::kTimeout)
                continue;
            return Unex(describeCdpFailure("wait for cdp event failed"_t, event.error()));
        }
        TRY(handleProbeEvent(*event, console, pageErrors));
    }

    if (lastError) {
        return Unex(text::format("timed out waiting for frame expression: {}", *lastError));
    }
    return Unex("timed out waiting for frame expression"_t);
}

[[nodiscard]] Expected<dto::BrowserProbeResponse, String>
runProbe(const dto::BrowserProbeRequest &request, const ProbeConfig &config, eng::Deadline deadline)
{
    crawler::BrowserSession browser{
        config.dnsResolver, config.processStarter,
        crawler::BrowserSessionConfig{
            .urlBytesMax = config.svcConfig.urlBytesMax(),
            .proxyDownBytesMax = config.cdpMaxRemotePayloadBytes * 4_i64,
            .browserRunsRoot =
                crawler::buildBrowserRunsRoot(std::string(config.svcConfig.stateDir())),
            .cgroupRootPath = crawler::resolveDelegatedCgroupRootPath(),
            .cgroupLimits = {},
            .localFixtureTrustDbSourcePath =
                crawler::localFixtureTrustDbSourcePath(config.svcConfig.stateDir()),
            .devtoolsStartupTimeout = config.devtoolsStartupTimeout,
            .cdpHandshakeTimeout = config.cdpHandshakeTimeout,
            .cdpCommandTimeout = config.cdpCommandTimeout,
            .devtoolsPollInterval = config.devtoolsPollInterval,
            .browserStopTimeout = config.browserStopTimeout,
            .cdpMaxRemotePayloadBytes = config.cdpMaxRemotePayloadBytes,
            .proxyRequireAuth = false,
            .enableLocalFixtureRewrite = config.localFixtureRewrite,
            .cgroupNamePrefix = "webshotd_browser_probe",
        }
    };

    auto launched = browser.launch();
    if (!launched)
        return Unex(browser.buildFailureDetail(launched.error()));

    browser.markPhase("connect_cdp");
    auto connected = browser.connectCdp(deadline);
    if (!connected)
        return Unex(browser.buildFailureDetail(connected.error()));
    auto cdp = std::move(*connected);
    crawler::BrowserPageSession pageSession{*cdp};
    auto *cdpSession = static_cast<crawler::CdpSession *>(nullptr);

    std::vector<std::string> console;
    std::vector<std::string> pageErrors;

    auto cleanup = [&]() {
        if (const auto closedPage = pageSession.close(); !closedPage) {
            LOG_WARNING() << std::format(
                "Suppressing page session close failure during probe cleanup: {}",
                closedPage.error()
            );
        }
        if (const auto closed = cdp->close(); !closed) {
            LOG_WARNING() << std::format(
                "Suppressing CDP close failure during probe cleanup: code={}{}",
                numericCast<int>(closed.error().code),
                closed.error().detail ? std::format(", detail={}", *closed.error().detail)
                                      : std::string{}
            );
        }
        browser.close();
    };

    const auto failProbe = [&](String detail) -> Expected<dto::BrowserProbeResponse, String> {
        cleanup();
        return Unex(std::move(detail));
    };
    const auto runPageSessionFlow = [&](auto &&step) -> Expected<void, String> {
        const auto result = step([&browser](std::string_view phase) { browser.markPhase(phase); });
        if (!result)
            return Unex(browser.buildFailureDetail(result.error()));
        return {};
    };
    const auto drainSessionEvents = [&]() -> Expected<void, String> {
        const auto drained = drainProbeEvents(*cdpSession, console, pageErrors);
        if (!drained)
            return Unex(browser.buildFailureDetail(drained.error()));
        return {};
    };

    if (const auto attached = runPageSessionFlow([&](const auto &markPhase) {
            return pageSession.attachFreshTarget(markPhase);
        });
        !attached) {
        return failProbe(attached.error());
    }
    cdpSession = &pageSession.cdpSession();

    if (const auto ok = runPageSessionFlow([&](const auto &markPhase) {
            return pageSession.enableBaseDomains(markPhase);
        });
        !ok) {
        return failProbe(ok.error());
    }
    if (const auto drained = drainSessionEvents(); !drained)
        return failProbe(drained.error());

    browser.markPhase("navigate");
    dto::PageNavigateParams navigateParams;
    navigateParams.url = request.url;
    const auto navigateResult = cdpSession->send<dto::PageNavigateResult>(
        "Page.navigate"_t, navigateParams
    );
    if (!navigateResult)
        return failProbe(browser.buildFailureDetail(
            describeCdpFailure("Page.navigate failed"_t, navigateResult.error())
        ));
    if (navigateResult->errorText) {
        return failProbe(
            browser.buildFailureDetail(String::fromBytes(*navigateResult->errorText).expect())
        );
    }
    if (const auto drained = drainSessionEvents(); !drained)
        return failProbe(drained.error());

    browser.markPhase("wait_expression");
    const auto waitExpression = String::fromBytes(request.wait_expression).expect();
    if (const auto waited = waitForExpression(
            *cdpSession, waitExpression, deadline, config.devtoolsPollInterval, console, pageErrors
        );
        !waited) {
        return failProbe(browser.buildFailureDetail(waited.error()));
    }
    if (const auto drained = drainSessionEvents(); !drained)
        return failProbe(drained.error());
    if (const auto settled = settleProbeEvents(
            *cdpSession, deadline, config.devtoolsPollInterval, console, pageErrors
        );
        !settled) {
        return failProbe(browser.buildFailureDetail(settled.error()));
    }

    dto::BrowserProbeResponse result{};

    const auto state = evaluateExpressionAs<dto::BrowserProbePageState>(
        *cdpSession,
        R"JS((() => ({ final_url: location.href, title: document.title || '', text: (document.body ? document.body.innerText : '') }))())JS"_t
    );
    if (!state)
        return failProbe(browser.buildFailureDetail(state.error()));
    result.final_url = state->final_url;
    result.title = state->title;
    result.text = state->text;

    if (request.frame_expression) {
        const auto frameExpression = String::fromBytes(*request.frame_expression).expect();
        auto frame = waitForFrameExpression(
            *cdpSession, frameExpression, deadline, config.devtoolsPollInterval, console, pageErrors
        );
        if (!frame) {
            return failProbe(browser.buildFailureDetail(
                text::format("frame_expression failed: {}", frame.error())
            ));
        }
        result.frame = grabValueOf(frame);
    }
    if (const auto drained = drainSessionEvents(); !drained)
        return failProbe(drained.error());
    result.console = std::move(console);
    result.page_errors = std::move(pageErrors);

    cleanup();
    return result;
}

} // namespace

struct BrowserProbeHandler::Impl final {
    ProbeConfig probeConfig;
};

BrowserProbeHandler::BrowserProbeHandler(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : HttpHandlerBase(config, context),
      impl(
          std::make_unique<Impl>(Impl{
              .probeConfig = ProbeConfig{
                  .svcConfig = context.FindComponent<Config>(),
                  .dnsResolver = context.FindComponent<us::clients::dns::Component>().GetResolver(),
                  .processStarter = context.FindComponent<us::components::ProcessStarter>().Get(),
                  .requestTimeout = config["request-timeout-ms"].As<int64_t>() * 1ms,
                  .devtoolsStartupTimeout = config["devtools_startup_timeout_ms"].As<int64_t>() *
                                            1ms,
                  .cdpHandshakeTimeout = config["cdp_handshake_timeout_ms"].As<int64_t>() * 1ms,
                  .cdpCommandTimeout = config["cdp_command_timeout_ms"].As<int64_t>() * 1ms,
                  .devtoolsPollInterval = config["devtools_poll_interval_ms"].As<int64_t>() * 1ms,
                  .browserStopTimeout = config["browser_stop_timeout_ms"].As<int64_t>() * 1ms,
                  .cdpMaxRemotePayloadBytes =
                      i64(config["cdp_max_remote_payload_bytes"].As<int64_t>()),
                  .localFixtureRewrite = config["local_fixture_rewrite"].As<bool>(),
              },
          })
      )
{
}

BrowserProbeHandler::~BrowserProbeHandler() = default;

us::yaml_config::Schema BrowserProbeHandler::GetStaticConfigSchema()
{
    return us::yaml_config::MergeSchemas<server::handlers::HttpHandlerBase>(R"(
type: object
description: Monitor-only browser probe handler static config
additionalProperties: false
properties:
  request-timeout-ms:
    type: integer
    minimum: 1
    description: Upper bound for /tests/browser-probe handler in milliseconds
  devtools_startup_timeout_ms:
    type: integer
    minimum: 1
    description: How long to wait for Chromium to expose devtools socket and websocket path in milliseconds
  cdp_handshake_timeout_ms:
    type: integer
    minimum: 1
    description: Upper bound for devtools websocket handshake in milliseconds
  cdp_command_timeout_ms:
    type: integer
    minimum: 1
    description: Upper bound for a single CDP command round-trip in milliseconds
  devtools_poll_interval_ms:
    type: integer
    minimum: 1
    description: Polling interval for devtools socket/path discovery in milliseconds
  browser_stop_timeout_ms:
    type: integer
    minimum: 1
    description: Grace timeout after SIGTERM before SIGKILL for the Chromium process in milliseconds
  cdp_max_remote_payload_bytes:
    type: integer
    minimum: 1
    description: Maximum accepted remote CDP websocket message size in bytes
  local_fixture_rewrite:
    type: boolean
    description: Rewrite local test fixture hosts (test-target) to 127.0.0.1:18080/18443 for the browser probe
)");
}

std::string BrowserProbeHandler::HandleRequestThrow(
    const server::http::HttpRequest &request, server::request::RequestContext &
) const
{
    using server::http::HttpMethod::kPost;
    using enum server::http::HttpStatus;

    auto &response = request.GetHttpResponse();
    if (request.GetMethod() != kPost)
        return httpu::respondError(response, kMethodNotAllowed, "method not allowed"_t);

    const auto body = String::fromBytes(request.RequestBody());
    if (!body)
        return httpu::respondError(response, kBadRequest, "invalid request body"_t);

    const auto probeRequest = parseProbeRequest(body->view());
    if (!probeRequest)
        return httpu::respondError(response, kBadRequest, probeRequest.error());

    const auto timeoutBudget = std::min(
        impl->probeConfig.requestTimeout, probeRequest->timeout_ms * 1ms
    );
    auto finalDeadline = computeHandlerDeadline(request, timeoutBudget);
    eng::current_task::SetDeadline(finalDeadline);

    const auto result = runProbe(*probeRequest, impl->probeConfig, finalDeadline);
    if (!result)
        return httpu::respondError(response, kInternalServerError, result.error());
    return httpu::respondJson(response, kOk, *result);
}

} // namespace v1
