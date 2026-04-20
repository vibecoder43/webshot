#pragma once

/**
 * @file
 * @brief Project-wide userver namespace aliases.
 *
 * userver uses versioned namespaces; forward-declaring nested `userver::...`
 * namespaces can introduce ambiguity (e.g. `userver::server` vs
 * `userver::v2_xx::server`). To keep aliasing robust, this header includes a
 * small set of userver headers that ensure the aliased namespaces exist.
 */

// Minimal includes to make the aliased namespaces visible as `userver::...`.
#include <concepts>
#include <string_view>
#include <type_traits>
#include <utility>

#include <userver/clients/http/client.hpp>
#include <userver/engine/deadline.hpp>
#include <userver/formats/json_fwd.hpp>
#include <userver/server/request/task_inherited_data.hpp>
#include <userver/utils/assert.hpp>
#include <userver/utils/datetime.hpp>

namespace us = userver;
namespace server = us::server;
namespace eng = us::engine;
namespace json = us::formats::json;
namespace datetime = us::utils::datetime;
namespace httpc = us::clients::http;

namespace v1::detail {

[[noreturn]] inline void abortInvariant(std::string_view message) noexcept
{
    us::utils::AbortWithStacktrace(message);
}

template <typename Message>
    requires requires(const Message &message) {
        { message.view() } -> std::convertible_to<std::string_view>;
    }
[[noreturn]] inline void abortInvariant(const Message &message) noexcept
{
    us::utils::AbortWithStacktrace(message.view());
}

} // namespace v1::detail

namespace v1 {

template <typename Condition, typename Message>
inline void invariant(const Condition &condition, const Message &message) noexcept
{
    if (condition)
        return;
    detail::abortInvariant(message);
}

} // namespace v1
