#pragma once

#include <chrono>

#include <userver/engine/deadline.hpp>
#include <userver/server/http/http_request.hpp>
#include <userver/server/request/task_inherited_data.hpp>

namespace v1 {

[[nodiscard]] inline userver::engine::Deadline computeHandlerDeadline(
    const userver::server::http::HttpRequest &request, std::chrono::milliseconds handlerTimeout
)
{
    using userver::engine::Deadline;

    const auto configDeadline = Deadline::FromTimePoint(request.GetStartTime() + handlerTimeout);
    const auto inheritedDeadline = userver::server::request::GetTaskInheritedDeadline();

    if (!inheritedDeadline.IsReachable())
        return configDeadline;
    if (!configDeadline.IsReachable())
        return inheritedDeadline;

    const auto inheritedLeft = inheritedDeadline.TimeLeft();
    const auto configLeft = configDeadline.TimeLeft();
    if (inheritedLeft < configLeft)
        return inheritedDeadline;
    return configDeadline;
}

} // namespace v1
