#pragma once

#include "text.hpp"

#include <userver/utils/assert.hpp>

namespace ws {
namespace us = userver;

[[noreturn]] inline void Invariant(const String &message) noexcept
{
    us::utils::AbortWithStacktrace(message.View());
}

template <typename Condition>
inline void Invariant(const Condition &condition, const String &message) noexcept
{
    if (condition)
        return;
    Invariant(message);
}

} // namespace ws
