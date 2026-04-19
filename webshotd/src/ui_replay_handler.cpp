#include "ui_replay_handler.hpp"
/**
 * @file
 * @brief Handler that renders the replay UI for a specific capture UUID.
 */
#include "client_ip.hpp"
#include "config.hpp"
#include "crud.hpp"
#include "deadline_utils.hpp"
#include "integers.hpp"
#include "text.hpp"
#include "uuid_format.hpp"
#include "uuid_utils.hpp"

#include <chrono>

#include <ada/character_sets-inl.h>
#include <ada/unicode.h>
#include <userver/components/component.hpp>
#include <userver/engine/task/current_task.hpp>
#include <userver/http/common_headers.hpp>
#include <userver/server/http/http_request.hpp>
#include <userver/server/http/http_response.hpp>
#include <userver/server/http/http_status.hpp>
#include <userver/yaml_config/merge_schemas.hpp>

using namespace v1;
using namespace text::literals;

namespace {

[[nodiscard]] std::string escapeHtml(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (const char ch : text) {
        switch (ch) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        case '\'':
            out += "&#39;";
            break;
        default:
            out.push_back(ch);
            break;
        }
    }
    return out;
}

[[nodiscard]] std::string renderReplayLocation(const CaptureRecord &capture, const Config &config)
{
    const auto downloadUrl = buildCaptureDownloadUrl(capture.uuid, config);
    std::string out = "/vendor/replaywebpage/index.html?source=";
    out += ada::unicode::percent_encode(
        downloadUrl.href().view(), ada::character_sets::WWW_FORM_URLENCODED_PERCENT_ENCODE
    );
    out += "#url=";
    out += ada::unicode::percent_encode(
        capture.replayUrl.href().view(), ada::character_sets::WWW_FORM_URLENCODED_PERCENT_ENCODE
    );
    return out;
}

[[nodiscard]] std::string renderErrorPage(std::string_view message)
{
    std::string out = R"(<!doctype html>
<html lang="en">
  <head>
    <meta charset="utf-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1" />
    <title>Webshot Replay</title>
    <style>
      body {
        margin: 0;
        padding: 16px;
        font: 14px/1.4 system-ui, -apple-system, Segoe UI, Roboto, Ubuntu, Cantarell, "Noto Sans", sans-serif;
      }
    </style>
  </head>
  <body>
    <pre>)";
    out += escapeHtml(message);
    out += R"(</pre>
  </body>
</html>
)";
    return out;
}

} // namespace

namespace v1 {

UiReplayHandler::UiReplayHandler(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : HttpHandlerBase(config, context), crud(context.FindComponent<Crud>()),
      config(context.FindComponent<Config>()),
      requestTimeoutMs(i64(config["request-timeout-ms"].As<int64_t>()))
{
}

us::yaml_config::Schema UiReplayHandler::GetStaticConfigSchema()
{
    return us::yaml_config::MergeSchemas<server::handlers::HttpHandlerBase>(R"(
type: object
description: Replay UI handler static config
additionalProperties: false
properties:
  request-timeout-ms:
    type: integer
    minimum: 1
    description: Upper bound for /vendor/replaywebpage/replay/{uuid} handler in milliseconds
)");
}

std::string UiReplayHandler::HandleRequestThrow(
    const server::http::HttpRequest &request, server::request::RequestContext &
) const
{
    using enum server::http::HttpStatus;

    auto &response = request.GetHttpResponse();
    const auto handlerTimeout = std::chrono::milliseconds{requestTimeoutMs};
    auto finalDeadline = computeHandlerDeadline(request, handlerTimeout);
    eng::current_task::SetDeadline(finalDeadline);
    response.SetContentType("text/html; charset=utf-8");

    const std::string arg = request.GetPathArg("uuid");
    if (arg.empty()) {
        response.SetStatus(kBadRequest);
        return renderErrorPage("uuid: missing parameter");
    }

    const auto uuidStr = String::fromBytes(arg);
    if (!uuidStr) {
        response.SetStatus(kBadRequest);
        return renderErrorPage("uuid: invalid parameter");
    }

    const auto uuidOpt = uuidu::parse(uuidStr->view());
    if (!uuidOpt) {
        response.SetStatus(kBadRequest);
        return renderErrorPage("uuid: invalid parameter");
    }

    auto clientIp = client_ip::resolve(request, config);
    if (!clientIp) {
        response.SetStatus(kBadRequest);
        return renderErrorPage("invalid client ip");
    }
    auto cooldown = *crud.acquireClientIpCooldown(std::move(*clientIp));
    if (cooldown) {
        const auto retryAfterSeconds = std::chrono::ceil<std::chrono::seconds>(
            cooldown->retryAfter
        );
        response.SetStatus(kTooManyRequests);
        response.SetHeader(
            us::http::headers::kRetryAfter, std::format("{}", retryAfterSeconds.count())
        );
        return renderErrorPage("client IP in cooldown");
    }

    auto capture = crud.findCapture(*uuidOpt);
    if (!capture) {
        response.SetStatus(kInternalServerError);
        return renderErrorPage("internal server error");
    }
    if (!*capture) {
        response.SetStatus(kNotFound);
        return renderErrorPage("capture not found");
    }
    response.SetStatus(kFound);
    response.SetHeader(us::http::headers::kLocation, renderReplayLocation(**capture, config));
    return {};
}

} // namespace v1
