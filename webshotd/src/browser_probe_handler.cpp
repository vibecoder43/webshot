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

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
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
namespace json = us::formats::json;

namespace v1 {
namespace {

using crawler::describeCdpFailure;

struct [[nodiscard]] ProbeConfig final {
    const Config &svcConfig;
    us::clients::dns::Resolver &dnsResolver;
    us::engine::subprocess::ProcessStarter &processStarter;
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
    try {
        auto request = json::FromString(body).As<dto::BrowserProbeRequest>();
        if (request.url.empty() || request.wait_expression.empty() || request.timeout_ms <= 0)
            return std::unexpected("invalid request body"_t);
        return request;
    } catch (const json::Exception &) {
        return std::unexpected("invalid request body"_t);
    }
}

[[nodiscard]] Expected<json::Value, String>
evaluateExpression(crawler::CdpSession &cdpSession, const String &expression)
{
    dto::RuntimeEvaluateParams params;
    params.expression = std::string(expression.view());
    params.returnByValue = true;
    params.awaitPromise = true;

    const auto result = cdpSession.send<json::Value>("Runtime.evaluate"_t, params);
    if (!result)
        return std::unexpected(describeCdpFailure("Runtime.evaluate failed"_t, result.error()));

    const auto exception = (*result)["exceptionDetails"];
    if (!exception.IsMissing())
        return std::unexpected(
            text::format("Runtime.evaluate threw: {}", json::ToString(exception))
        );

    const auto resultValue = (*result)["result"];
    if (resultValue.IsMissing())
        return std::unexpected("Runtime.evaluate missing result"_t);

    const auto value = resultValue["value"];
    if (!value.IsMissing())
        return value;

    return json::FromString("null");
}

template <typename T>
[[nodiscard]] Expected<T, String>
evaluateExpressionAs(crawler::CdpSession &cdpSession, const String &expression)
{
    const auto value = evaluateExpression(cdpSession, expression);
    if (!value)
        return std::unexpected(value.error());
    try {
        return value->As<T>();
    } catch (const json::Exception &) {
        return std::unexpected("expression returned invalid shape"_t);
    }
}

[[nodiscard]] Expected<bool, String>
evaluateBoolExpression(crawler::CdpSession &cdpSession, const String &expression)
{
    const auto value = evaluateExpressionAs<bool>(cdpSession, expression);
    if (!value)
        return std::unexpected(value.error());
    return *value;
}

[[nodiscard]] Expected<std::optional<dto::BrowserProbeFrameState>, String>
evaluateFrameExpression(crawler::CdpSession &cdpSession, const String &expression)
{
    const auto value = evaluateExpression(cdpSession, expression);
    if (!value)
        return std::unexpected(value.error());
    if (json::ToString(*value) == "null")
        return std::optional<dto::BrowserProbeFrameState>{};

    try {
        return value->As<dto::BrowserProbeFrameState>();
    } catch (const json::Exception &) {
        return std::unexpected("expression returned invalid shape"_t);
    }
}

[[nodiscard]] Expected<void, String> handleProbeEvent(
    const crawler::CdpEvent &event, std::vector<std::string> &console,
    std::vector<std::string> &pageErrors
)
{
    if (!event.params)
        return {};
    if (event.method == "Runtime.consoleAPICalled"_t) {
        console.push_back(json::ToString(event.params->extra));
        return {};
    }
    if (event.method == "Runtime.exceptionThrown"_t) {
        pageErrors.push_back(json::ToString(event.params->extra));
        return {};
    }
    return {};
}

[[nodiscard]] Expected<void, String> drainProbeEvents(
    crawler::CdpSession &cdpSession, std::vector<std::string> &console,
    std::vector<std::string> &pageErrors
)
{
    for (const auto &event : cdpSession.drainAvailableEvents()) {
        const auto handled = handleProbeEvent(event, console, pageErrors);
        if (!handled)
            return std::unexpected(handled.error());
    }
    return {};
}

[[nodiscard]] Expected<void, String> waitForExpression(
    crawler::CdpSession &cdpSession, const String &expression, us::engine::Deadline deadline,
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
        return std::unexpected(matched.error());
    }

    while (!deadline.IsReached()) {
        if (const auto drained = drainProbeEvents(cdpSession, console, pageErrors); !drained) {
            return std::unexpected(drained.error());
        }
        if (const auto matched = updateMatchState(); matched) {
            if (*matched)
                return {};
        } else {
            return std::unexpected(matched.error());
        }

        const auto eventDeadline = us::engine::Deadline::FromDuration(
            std::min(
                chrono::duration_cast<chrono::milliseconds>(deadline.TimeLeft()), recheckInterval
            )
        );
        auto event = cdpSession.waitEvent(eventDeadline, "timed out waiting for expression"_t);
        if (!event) {
            if (event.error().code == crawler::CdpError::kTimeout)
                continue;
            return std::unexpected(
                describeCdpFailure("wait for cdp event failed"_t, event.error())
            );
        }
        if (const auto handled = handleProbeEvent(*event, console, pageErrors); !handled) {
            return std::unexpected(handled.error());
        }
    }

    if (lastError)
        return std::unexpected(text::format("timed out waiting for expression: {}", *lastError));
    return std::unexpected("timed out waiting for expression"_t);
}

[[nodiscard]] Expected<void, String> settleProbeEvents(
    crawler::CdpSession &cdpSession, us::engine::Deadline deadline,
    chrono::milliseconds recheckInterval, std::vector<std::string> &console,
    std::vector<std::string> &pageErrors
)
{
    const auto settleWindow = std::max(recheckInterval * 2, 250ms);
    const auto settleDeadline = pickEarlierDeadline(
        deadline, us::engine::Deadline::FromDuration(settleWindow)
    );

    while (!settleDeadline.IsReached()) {
        if (const auto drained = drainProbeEvents(cdpSession, console, pageErrors); !drained) {
            return std::unexpected(drained.error());
        }

        const auto eventDeadline = us::engine::Deadline::FromDuration(
            std::min(
                chrono::duration_cast<chrono::milliseconds>(settleDeadline.TimeLeft()),
                recheckInterval
            )
        );
        auto event = cdpSession.waitEvent(eventDeadline, "timed out waiting for probe settle"_t);
        if (!event) {
            if (event.error().code == crawler::CdpError::kTimeout)
                continue;
            return std::unexpected(
                describeCdpFailure("wait for cdp event failed"_t, event.error())
            );
        }
        if (const auto handled = handleProbeEvent(*event, console, pageErrors); !handled) {
            return std::unexpected(handled.error());
        }
    }

    return {};
}

[[nodiscard]] Expected<dto::BrowserProbeFrameState, String> waitForFrameExpression(
    crawler::CdpSession &cdpSession, const String &expression, us::engine::Deadline deadline,
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
        return std::unexpected(frame.error());
    }

    while (!deadline.IsReached()) {
        if (const auto drained = drainProbeEvents(cdpSession, console, pageErrors); !drained) {
            return std::unexpected(drained.error());
        }
        if (auto frame = updateFrameState(); frame) {
            if (*frame)
                return **frame;
        } else {
            return std::unexpected(frame.error());
        }

        const auto eventDeadline = us::engine::Deadline::FromDuration(
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
            return std::unexpected(
                describeCdpFailure("wait for cdp event failed"_t, event.error())
            );
        }
        if (const auto handled = handleProbeEvent(*event, console, pageErrors); !handled) {
            return std::unexpected(handled.error());
        }
    }

    if (lastError) {
        return std::unexpected(
            text::format("timed out waiting for frame expression: {}", *lastError)
        );
    }
    return std::unexpected("timed out waiting for frame expression"_t);
}

[[nodiscard]] Expected<dto::BrowserProbeResponse, String> runProbe(
    const dto::BrowserProbeRequest &request, const ProbeConfig &config,
    us::engine::Deadline deadline
)
{
    auto browser = crawler::BrowserSession{
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
        return std::unexpected(browser.buildFailureDetail(launched.error()));

    browser.markPhase("connect_cdp");
    auto connected = browser.connectCdp(deadline);
    if (!connected)
        return std::unexpected(browser.buildFailureDetail(connected.error()));
    auto cdp = std::move(*connected);
    auto pageSession = crawler::BrowserPageSession{*cdp};
    auto *cdpSession = static_cast<crawler::CdpSession *>(nullptr);

    std::vector<std::string> console;
    std::vector<std::string> pageErrors;

    auto cleanup = [&]() {
        if (const auto closedPage = pageSession.close(); !closedPage) {
            LOG_WARNING() << std::format(
                "Suppressing page session close failure during probe cleanup: {}",
                closedPage.error().view()
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

    browser.markPhase("create_browser_context");
    if (const auto created = pageSession.createBrowserContext(); !created) {
        const auto detail = browser.buildFailureDetail(created.error());
        cleanup();
        return std::unexpected(detail);
    }

    browser.markPhase("create_target");
    if (const auto created = pageSession.createBlankTarget(); !created) {
        const auto detail = browser.buildFailureDetail(created.error());
        cleanup();
        return std::unexpected(detail);
    }

    browser.markPhase("attach_target");
    if (const auto attached = pageSession.attachToTarget(); !attached) {
        const auto detail = browser.buildFailureDetail(attached.error());
        cleanup();
        return std::unexpected(detail);
    }
    cdpSession = &pageSession.cdpSession();

    if (const auto ok = cdpSession->sendVoid("Page.enable"_t); !ok) {
        const auto detail = browser.buildFailureDetail(
            describeCdpFailure("Page.enable failed"_t, ok.error())
        );
        cleanup();
        return std::unexpected(detail);
    }
    if (const auto ok = cdpSession->sendVoid("Runtime.enable"_t); !ok) {
        const auto detail = browser.buildFailureDetail(
            describeCdpFailure("Runtime.enable failed"_t, ok.error())
        );
        cleanup();
        return std::unexpected(detail);
    }
    browser.markPhase("enable_network");
    if (const auto ok = cdpSession->sendVoid("Network.enable"_t); !ok) {
        const auto detail = browser.buildFailureDetail(
            describeCdpFailure("Network.enable failed"_t, ok.error())
        );
        cleanup();
        return std::unexpected(detail);
    }
    browser.markPhase("enable_lifecycle_events");
    dto::PageSetLifecycleEventsEnabledParams lifecycleParams;
    lifecycleParams.enabled = true;
    if (const auto ok = cdpSession->sendVoid("Page.setLifecycleEventsEnabled"_t, lifecycleParams);
        !ok) {
        const auto detail = browser.buildFailureDetail(
            describeCdpFailure("Page.setLifecycleEventsEnabled failed"_t, ok.error())
        );
        cleanup();
        return std::unexpected(detail);
    }
    if (const auto drained = drainProbeEvents(*cdpSession, console, pageErrors); !drained) {
        const auto detail = browser.buildFailureDetail(drained.error());
        cleanup();
        return std::unexpected(detail);
    }

    browser.markPhase("navigate");
    dto::PageNavigateParams navigateParams;
    navigateParams.url = request.url;
    const auto navigateResult = cdpSession->send<dto::PageNavigateResult>(
        "Page.navigate"_t, navigateParams
    );
    if (!navigateResult) {
        const auto detail = browser.buildFailureDetail(
            describeCdpFailure("Page.navigate failed"_t, navigateResult.error())
        );
        cleanup();
        return std::unexpected(detail);
    }
    if (navigateResult->errorText) {
        const auto detail = browser.buildFailureDetail(
            String::fromBytes(*navigateResult->errorText).expect()
        );
        cleanup();
        return std::unexpected(detail);
    }
    if (const auto drained = drainProbeEvents(*cdpSession, console, pageErrors); !drained) {
        const auto detail = browser.buildFailureDetail(drained.error());
        cleanup();
        return std::unexpected(detail);
    }

    browser.markPhase("wait_expression");
    const auto waitExpression = String::fromBytes(request.wait_expression).expect();
    if (const auto waited = waitForExpression(
            *cdpSession, waitExpression, deadline, config.devtoolsPollInterval, console, pageErrors
        );
        !waited) {
        const auto detail = browser.buildFailureDetail(waited.error());
        cleanup();
        return std::unexpected(detail);
    }
    if (const auto drained = drainProbeEvents(*cdpSession, console, pageErrors); !drained) {
        const auto detail = browser.buildFailureDetail(drained.error());
        cleanup();
        return std::unexpected(detail);
    }
    if (const auto settled = settleProbeEvents(
            *cdpSession, deadline, config.devtoolsPollInterval, console, pageErrors
        );
        !settled) {
        const auto detail = browser.buildFailureDetail(settled.error());
        cleanup();
        return std::unexpected(detail);
    }

    auto result = dto::BrowserProbeResponse{};

    const auto state = evaluateExpressionAs<dto::BrowserProbePageState>(
        *cdpSession,
        R"JS((() => ({ final_url: location.href, title: document.title || '', text: (document.body ? document.body.innerText : '') }))())JS"_t
    );
    if (!state) {
        const auto detail = browser.buildFailureDetail(state.error());
        cleanup();
        return std::unexpected(detail);
    }
    result.final_url = state->final_url;
    result.title = state->title;
    result.text = state->text;

    if (request.frame_expression) {
        const auto frameExpression = String::fromBytes(*request.frame_expression).expect();
        auto frame = waitForFrameExpression(
            *cdpSession, frameExpression, deadline, config.devtoolsPollInterval, console, pageErrors
        );
        if (!frame) {
            const auto detail = browser.buildFailureDetail(
                text::format("frame_expression failed: {}", frame.error())
            );
            cleanup();
            return std::unexpected(detail);
        }
        result.frame = grabValueOf(frame);
    }
    if (const auto drained = drainProbeEvents(*cdpSession, console, pageErrors); !drained) {
        const auto detail = browser.buildFailureDetail(drained.error());
        cleanup();
        return std::unexpected(detail);
    }
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
