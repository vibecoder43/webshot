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
#include "json.hpp"
#include "schema/browser_probe.hpp"
#include "schema/cdp.hpp"
#include "text.hpp"
#include "try.hpp"

#include <algorithm>
#include <chrono>
#include <iterator>
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
#include <userver/engine/task/task_processor_fwd.hpp>
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
using text::ToBytes;

namespace chrono = std::chrono;
namespace ujson = userver::formats::json;

namespace ws {
namespace us = userver;
namespace server = us::server;
namespace eng = us::engine;
namespace {

using crawler::DescribeCdpFailure;

constexpr chrono::milliseconds kMinProbeSettleWindow = 250ms;

struct [[nodiscard]] ProbeConfig final {
    const Config &svc_config;
    us::clients::dns::Resolver &dns_resolver_;
    eng::subprocess::ProcessStarter &process_starter_;
    eng::TaskProcessor &fs_task_processor_;
    chrono::milliseconds request_timeout;
    chrono::milliseconds devtools_startup_timeout;
    chrono::milliseconds cdp_handshake_timeout;
    chrono::milliseconds cdp_command_timeout;
    chrono::milliseconds devtools_poll_interval;
    chrono::milliseconds browser_stop_timeout;
    i64 cdp_max_remote_payload_bytes;
    bool local_fixture_rewrite;
    std::vector<u16> testsuite_loopback_ports;
};

[[nodiscard]] Expected<dto::BrowserProbeRequest, String> ParseProbeRequest(const String &body)
{
    const auto request = TRY(
        ws::json::Parse<dto::BrowserProbeRequest>(body, "invalid request body"_t)
    );
    ENSURE(
        !request.url.empty() && !request.wait_expression.empty() && request.timeout_ms > 0,
        "invalid request body"_t
    );
    return request;
}

[[nodiscard]] std::vector<u16>
ParseTestsuiteLoopbackPorts(const us::components::ComponentConfig &config)
{
    std::vector<u16> ports;
    for (const auto port : config["testsuite_loopback_ports"].As<std::vector<int64_t>>()) {
        ports.emplace_back(NumericCast<uint16_t>(port));
    }
    return ports;
}

[[nodiscard]] Expected<ujson::Value, String>
EvaluateExpression(crawler::CdpSession &cdp_session, const String &expression)
{
    dto::RuntimeEvaluateParams params{
        .expression = ToBytes(expression),
        .returnByValue = true,
        .awaitPromise = true,
    };

    const auto result = TRY_MAP_ERR(
        cdp_session.Send<ujson::Value>("Runtime.evaluate"_t, params), [](auto failure) {
            return DescribeCdpFailure("Runtime.evaluate failed"_t, std::move(failure));
        }
    );

    const auto exception = result["exceptionDetails"];
    if (!exception.IsMissing()) {
        return Unex(
            text::Format(
                "Runtime.evaluate threw: {}",
                TRY(ws::json::Stringify(exception, "Runtime.evaluate returned invalid exception"_t))
            )
        );
    }

    const auto result_value = result["result"];
    ENSURE(!result_value.IsMissing(), "Runtime.evaluate missing result"_t);

    const auto value = result_value["value"];
    if (!value.IsMissing())
        return value;

    return ws::json::Parse<ujson::Value>("null"_t, "Runtime.evaluate missing value"_t);
}

template <typename T>
[[nodiscard]] Expected<T, String>
EvaluateExpressionAs(crawler::CdpSession &cdp_session, const String &expression)
{
    return ws::json::As<T>(
        TRY(EvaluateExpression(cdp_session, expression)), "expression returned invalid shape"_t
    );
}

[[nodiscard]] Expected<bool, String>
EvaluateBoolExpression(crawler::CdpSession &cdp_session, const String &expression)
{
    return TRY(EvaluateExpressionAs<bool>(cdp_session, expression));
}

[[nodiscard]] Expected<std::optional<dto::BrowserProbeFrameState>, String>
EvaluateFrameExpression(crawler::CdpSession &cdp_session, const String &expression)
{
    return EvaluateExpression(cdp_session, expression)
        .AndThen(
            [](const auto &value) -> Expected<std::optional<dto::BrowserProbeFrameState>, String> {
                return ws::json::Stringify(value, "expression returned invalid shape"_t)
                    .AndThen(
                        [&](
                            const auto &rendered
                        ) -> Expected<std::optional<dto::BrowserProbeFrameState>, String> {
                            if (rendered == "null"_t)
                                return {};

                            return ws::json::As<dto::BrowserProbeFrameState>(
                                       value, "expression returned invalid shape"_t
                            )
                                .Transform([](auto parsed) {
                                    return std::optional<dto::BrowserProbeFrameState>{
                                        std::move(parsed)
                                    };
                                });
                        }
                    );
            }
        );
}

void CleanupProbeSession(
    crawler::BrowserSession &browser, std::unique_ptr<crawler::BrowserPageSession> &page_session,
    std::unique_ptr<crawler::CdpClient> &cdp
)
{
    if (page_session) {
        if (const auto closed_page = page_session->Close(); !closed_page) {
            LOG_WARNING() << std::format(
                "Suppressing page session close failure during probe cleanup: {}",
                closed_page.Error()
            );
        }
        page_session.reset();
    }
    if (cdp) {
        if (const auto closed = cdp->Close(); !closed) {
            LOG_WARNING() << std::format(
                "Suppressing CDP close failure during probe cleanup: code={}{}",
                NumericCast<int>(closed.Error().code),
                closed.Error().detail ? std::format(", detail={}", *closed.Error().detail)
                                      : std::string{}
            );
        }
        cdp.reset();
    }
    browser.Close();
}

[[nodiscard]] Expected<void, String> HandleProbeEvent(
    const crawler::CdpEvent &event, std::vector<String> &console, std::vector<String> &page_errors
)
{
    if (!event.params)
        return {};
    if (event.method == "Runtime.consoleAPICalled"_t) {
        console.push_back(
            TRY(ws::json::Stringify(event.params->extra, "invalid console payload"_t))
        );
        return {};
    }
    if (event.method == "Runtime.exceptionThrown"_t) {
        page_errors.push_back(
            TRY(ws::json::Stringify(event.params->extra, "invalid exception payload"_t))
        );
        return {};
    }
    return {};
}

[[nodiscard]] Expected<void, String> DrainProbeEvents(
    crawler::CdpSession &cdp_session, std::vector<String> &console, std::vector<String> &page_errors
)
{
    for (const auto &event : cdp_session.DrainAvailableEvents())
        TRY(HandleProbeEvent(event, console, page_errors));
    return {};
}

[[nodiscard]] Expected<void, String> WaitForExpression(
    crawler::CdpSession &cdp_session, const String &expression, eng::Deadline deadline,
    chrono::milliseconds recheck_interval, std::vector<String> &console,
    std::vector<String> &page_errors
)
{
    std::optional<String> last_error;

    const auto update_match_state = [&]() -> Expected<bool, String> {
        const auto matched = EvaluateBoolExpression(cdp_session, expression);
        if (matched)
            return *matched;
        last_error = matched.Error();
        return false;
    };

    if (TRY(update_match_state()))
        return {};

    while (!deadline.IsReached()) {
        TRY(DrainProbeEvents(cdp_session, console, page_errors));
        if (TRY(update_match_state()))
            return {};

        const auto event_deadline = eng::Deadline::FromDuration(
            std::min(
                chrono::duration_cast<chrono::milliseconds>(deadline.TimeLeft()), recheck_interval
            )
        );
        auto event = cdp_session.WaitEvent(event_deadline, "timed out waiting for expression"_t);
        if (!event) {
            if (event.Error().code == crawler::CdpError::kTimeout)
                continue;
            return Unex(DescribeCdpFailure("wait for cdp event failed"_t, event.Error()));
        }
        TRY(HandleProbeEvent(*event, console, page_errors));
    }

    if (last_error)
        return Unex(text::Format("timed out waiting for expression: {}", *last_error));
    return Unex("timed out waiting for expression"_t);
}

[[nodiscard]] Expected<void, String> SettleProbeEvents(
    crawler::CdpSession &cdp_session, eng::Deadline deadline, chrono::milliseconds recheck_interval,
    std::vector<String> &console, std::vector<String> &page_errors
)
{
    const auto settle_window = std::max(recheck_interval * 2, kMinProbeSettleWindow);
    const auto settle_deadline = PickEarlierDeadline(
        deadline, eng::Deadline::FromDuration(settle_window)
    );

    while (!settle_deadline.IsReached()) {
        TRY(DrainProbeEvents(cdp_session, console, page_errors));

        const auto event_deadline = eng::Deadline::FromDuration(
            std::min(
                chrono::duration_cast<chrono::milliseconds>(settle_deadline.TimeLeft()),
                recheck_interval
            )
        );
        auto event = cdp_session.WaitEvent(event_deadline, "timed out waiting for probe settle"_t);
        if (!event) {
            if (event.Error().code == crawler::CdpError::kTimeout)
                continue;
            return Unex(DescribeCdpFailure("wait for cdp event failed"_t, event.Error()));
        }
        TRY(HandleProbeEvent(*event, console, page_errors));
    }

    return {};
}

[[nodiscard]] Expected<dto::BrowserProbeFrameState, String> WaitForFrameExpression(
    crawler::CdpSession &cdp_session, const String &expression, eng::Deadline deadline,
    chrono::milliseconds recheck_interval, std::vector<String> &console,
    std::vector<String> &page_errors
)
{
    std::optional<String> last_error;

    const auto update_frame_state =
        [&]() -> Expected<std::optional<dto::BrowserProbeFrameState>, String> {
        auto frame = EvaluateFrameExpression(cdp_session, expression);
        if (frame)
            return frame;
        last_error = frame.Error();
        return {};
    };

    if (auto frame = TRY(update_frame_state()))
        return *frame;

    while (!deadline.IsReached()) {
        TRY(DrainProbeEvents(cdp_session, console, page_errors));
        if (auto frame = TRY(update_frame_state()))
            return *frame;

        const auto event_deadline = eng::Deadline::FromDuration(
            std::min(
                chrono::duration_cast<chrono::milliseconds>(deadline.TimeLeft()), recheck_interval
            )
        );
        auto event = cdp_session.WaitEvent(
            event_deadline, "timed out waiting for frame expression"_t
        );
        if (!event) {
            if (event.Error().code == crawler::CdpError::kTimeout)
                continue;
            return Unex(DescribeCdpFailure("wait for cdp event failed"_t, event.Error()));
        }
        TRY(HandleProbeEvent(*event, console, page_errors));
    }

    if (last_error) {
        return Unex(text::Format("timed out waiting for frame expression: {}", *last_error));
    }
    return Unex("timed out waiting for frame expression"_t);
}

[[nodiscard]] Expected<dto::BrowserProbeResponse, String>
RunProbe(const dto::BrowserProbeRequest &request, const ProbeConfig &config, eng::Deadline deadline)
{
    crawler::BrowserSession browser{
        config.dns_resolver_, config.process_starter_, config.fs_task_processor_,
        crawler::BrowserSessionConfig{
            .url_bytes_max = config.svc_config.UrlBytesMax(),
            .proxy_down_bytes_max = config.cdp_max_remote_payload_bytes * 4_i64,
            .browser_runs_root_ =
                crawler::BuildBrowserRunsRoot(std::string(config.svc_config.StateDir())),
            .cgroup_root_path_ = crawler::ResolveDelegatedCgroupRootPath(config.fs_task_processor_),
            .cgroup_limits_ = {},
            .local_fixture_trust_db_source_path =
                crawler::LocalFixtureTrustDbSourcePath(config.svc_config.StateDir()),
            .devtools_startup_timeout = config.devtools_startup_timeout,
            .cdp_handshake_timeout = config.cdp_handshake_timeout,
            .cdp_command_timeout = config.cdp_command_timeout,
            .devtools_poll_interval = config.devtools_poll_interval,
            .browser_stop_timeout = config.browser_stop_timeout,
            .cdp_max_remote_payload_bytes = config.cdp_max_remote_payload_bytes,
            .proxy_require_auth = false,
            .enable_local_fixture_rewrite = config.local_fixture_rewrite,
            .testsuite_loopback_ports = config.testsuite_loopback_ports,
            .cgroup_name_prefix = "webshotd_browser_probe",
        }
    };
    std::unique_ptr<crawler::CdpClient> cdp;
    std::unique_ptr<crawler::BrowserPageSession> page_session;
    std::vector<String> console;
    std::vector<String> page_errors;

    const auto decorate_failure = [&browser](auto detail) {
        return browser.BuildFailureDetail(std::move(detail));
    };
    const auto result = [&]() -> Expected<dto::BrowserProbeResponse, String> {
        TRY(browser.Launch());

        const auto mark_phase = [&browser](std::string_view phase) { browser.MarkPhase(phase); };

        browser.MarkPhase("connect_cdp");
        cdp = TRY(browser.ConnectCdp(deadline));
        page_session = std::make_unique<crawler::BrowserPageSession>(*cdp);

        TRY(page_session->AttachFreshTarget(mark_phase));
        auto &cdp_session = page_session->GetSession();
        TRY(page_session->EnableBaseDomains(mark_phase));
        TRY(DrainProbeEvents(cdp_session, console, page_errors));

        browser.MarkPhase("navigate");
        dto::PageNavigateParams navigate_params;
        navigate_params.url = request.url;
        const auto navigate_result = TRY_MAP_ERR(
            cdp_session.Send<dto::PageNavigateResult>("Page.navigate"_t, navigate_params),
            [](auto failure) {
                return DescribeCdpFailure("Page.navigate failed"_t, std::move(failure));
            }
        );
        ENSURE(!navigate_result.errorText, *String::FromBytes(*navigate_result.errorText));
        TRY(DrainProbeEvents(cdp_session, console, page_errors));

        browser.MarkPhase("wait_expression");
        const auto wait_expression = *String::FromBytes(request.wait_expression);
        TRY(WaitForExpression(
            cdp_session, wait_expression, deadline, config.devtools_poll_interval, console,
            page_errors
        ));
        TRY(DrainProbeEvents(cdp_session, console, page_errors));
        TRY(SettleProbeEvents(
            cdp_session, deadline, config.devtools_poll_interval, console, page_errors
        ));

        dto::BrowserProbeResponse probe_result{};

        const auto state = TRY(
            EvaluateExpressionAs<dto::BrowserProbePageState>(
                cdp_session,
                R"JS((() => ({ final_url: location.href, title: document.title || '', text: (document.body ? document.body.innerText : '') }))())JS"_t
            )
        );
        probe_result.final_url = state.final_url;
        probe_result.title = state.title;
        probe_result.text = state.text;

        if (request.frame_expression) {
            const auto frame_expression = *String::FromBytes(*request.frame_expression);
            probe_result.frame = TRY_MAP_ERR(
                WaitForFrameExpression(
                    cdp_session, frame_expression, deadline, config.devtools_poll_interval, console,
                    page_errors
                ),
                [](auto detail) { return text::Format("frame_expression failed: {}", detail); }
            );
        }
        TRY(DrainProbeEvents(cdp_session, console, page_errors));
        std::ranges::transform(
            console, std::back_inserter(probe_result.console),
            [](const auto &s) { return ToBytes(s); }
        );
        std::ranges::transform(
            page_errors, std::back_inserter(probe_result.page_errors),
            [](const auto &s) { return ToBytes(s); }
        );
        return probe_result;
    }()
                                     .TransformError(decorate_failure);

    CleanupProbeSession(browser, page_session, cdp);
    return result;
}

} // namespace

struct BrowserProbeHandler::Impl final {
    ProbeConfig probe_config;
};

BrowserProbeHandler::BrowserProbeHandler(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : HttpHandlerBase(config, context),
      impl_(
          std::make_unique<Impl>(Impl{
              .probe_config = ProbeConfig{
                  .svc_config = context.FindComponent<Config>(),
                  .dns_resolver_ =
                      context.FindComponent<us::clients::dns::Component>().GetResolver(),
                  .process_starter_ = context.FindComponent<us::components::ProcessStarter>().Get(),
                  .fs_task_processor_ = context.GetTaskProcessor("fs-task-processor"),
                  .request_timeout = config["request-timeout-ms"].As<int64_t>() * 1ms,
                  .devtools_startup_timeout = config["devtools_startup_timeout_ms"].As<int64_t>() *
                                              1ms,
                  .cdp_handshake_timeout = config["cdp_handshake_timeout_ms"].As<int64_t>() * 1ms,
                  .cdp_command_timeout = config["cdp_command_timeout_ms"].As<int64_t>() * 1ms,
                  .devtools_poll_interval = config["devtools_poll_interval_ms"].As<int64_t>() * 1ms,
                  .browser_stop_timeout = config["browser_stop_timeout_ms"].As<int64_t>() * 1ms,
                  .cdp_max_remote_payload_bytes =
                      i64(config["cdp_max_remote_payload_bytes"].As<int64_t>()),
                  .local_fixture_rewrite = config["local_fixture_rewrite"].As<bool>(),
                  .testsuite_loopback_ports = ParseTestsuiteLoopbackPorts(config),
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
    description: Upper bound for /tests/browser_probe handler in milliseconds
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
  testsuite_loopback_ports:
    type: array
    description: Extra loopback ports that the test-only browser probe may reach through the crawler proxy
    items:
      type: integer
      description: Loopback TCP port number
      minimum: 1
      maximum: 65535
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
        return httpu::RespondError(response, kMethodNotAllowed, "method not allowed"_t);

    const auto body = String::FromBytes(request.RequestBody());
    if (!body)
        return httpu::RespondError(response, kBadRequest, "invalid request body"_t);

    const auto probe_request = ParseProbeRequest(*body);
    if (!probe_request)
        return httpu::RespondError(response, kBadRequest, probe_request.Error());

    const auto timeout_budget = std::min(
        impl_->probe_config.request_timeout, probe_request->timeout_ms * 1ms
    );
    auto final_deadline = ComputeHandlerDeadline(request, timeout_budget);
    eng::current_task::SetDeadline(final_deadline);

    const auto result = RunProbe(*probe_request, impl_->probe_config, final_deadline);
    if (!result)
        return httpu::RespondError(response, kInternalServerError, result.Error());
    return httpu::RespondJson(response, kOk, *result);
}

} // namespace ws
