#include "ui_replay_handler.hpp"
/**
 * @file
 * @brief Handler that renders the replay UI for a specific capture UUID.
 */
#include "config.hpp"
#include "crud.hpp"
#include "handler_request_support.hpp"
#include "storage_url.hpp"
#include "text.hpp"
#include "try.hpp"
#include "uuid_format.hpp"
#include "uuid_utils.hpp"

#include <chrono>
#include <format>
#include <optional>

#include <ada/character_sets-inl.h>
#include <ada/unicode.h>
#include <userver/components/component.hpp>
#include <userver/engine/task/current_task.hpp>
#include <userver/http/common_headers.hpp>
#include <userver/server/http/http_request.hpp>
#include <userver/server/http/http_response.hpp>
#include <userver/server/http/http_status.hpp>
#include <userver/yaml_config/merge_schemas.hpp>

using namespace ws;
using namespace text::literals;
using namespace std::chrono_literals;

namespace {

[[nodiscard]] std::string EscapeHtml(std::string_view text)
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

[[nodiscard]] Expected<std::string, StorageUrlError> RenderReplayLocation(
    const CaptureRecord &capture, const Config &config, const std::optional<String> &request_host
)
{
    const auto download_url = TRY(BuildCaptureDownloadUrl(
        capture.uuid, config.S3Mode(), config.S3PublicBaseUrl(), request_host
    ));

    std::string out = "/vendor/replaywebpage/index.html?source=";
    out += ada::unicode::percent_encode(
        download_url.Href().View(), ada::character_sets::WWW_FORM_URLENCODED_PERCENT_ENCODE
    );
    out += "#url=";
    out += ada::unicode::percent_encode(
        capture.replay_url.Href().View(), ada::character_sets::WWW_FORM_URLENCODED_PERCENT_ENCODE
    );
    return out;
}

[[nodiscard]] std::string RenderErrorPage(String message)
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
    out += EscapeHtml(message.View());
    out += R"(</pre>
  </body>
</html>
)";
    return out;
}

} // namespace

namespace ws {

namespace us = userver;
namespace server = us::server;
UiReplayHandler::UiReplayHandler(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : HttpHandlerBase(config, context), crud_(context.FindComponent<Crud>()),
      config_(context.FindComponent<Config>()),
      request_timeout(config["request-timeout-ms"].As<int64_t>() * 1ms)
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
    HandlerRequestSupport request_support{crud_, config_};
    request_support.ApplyRequestDeadline(request, request_timeout);
    response.SetContentType("text/html; charset=utf-8");

    const auto uuid = request_support.ParseUuidPathArg(request, "uuid"_t);
    if (!uuid) {
        response.SetStatus(kBadRequest);
        return RenderErrorPage(text::Format("{}: {}", uuid.Error().name, uuid.Error().message));
    }

    const auto cooldown = request_support.CheckClientIpCooldown(request);
    if (!cooldown) {
        if (cooldown.Error() == ClientRequestError::kInvalidClientIp) {
            response.SetStatus(kBadRequest);
            return RenderErrorPage("invalid client ip"_t);
        }

        response.SetStatus(kInternalServerError);
        return RenderErrorPage("internal server error"_t);
    }
    if (*cooldown) {
        const auto retry_after_seconds = std::chrono::ceil<std::chrono::seconds>(
            (*cooldown)->retry_after
        );
        response.SetStatus(kTooManyRequests);
        response.SetHeader(
            us::http::headers::kRetryAfter, std::to_string(retry_after_seconds.count())
        );
        return RenderErrorPage("client IP in cooldown"_t);
    }

    auto capture = crud_.FindCapture(*uuid);
    if (!capture) {
        response.SetStatus(kInternalServerError);
        return RenderErrorPage("internal server error"_t);
    }
    if (!*capture) {
        response.SetStatus(kNotFound);
        return RenderErrorPage("capture not found"_t);
    }
    response.SetStatus(kFound);
    auto replay_location = RenderReplayLocation(
        **capture, config_, request_support.RequestHost(request)
    );
    if (!replay_location) {
        LOG_ERROR() << std::format(
            "Failed to build replay storage URL: {}",
            StorageUrlErrorMessage(replay_location.Error())
        );
        response.SetStatus(kInternalServerError);
        return RenderErrorPage("internal server error"_t);
    }
    response.SetHeader(us::http::headers::kLocation, *replay_location);
    return {};
}

} // namespace ws
