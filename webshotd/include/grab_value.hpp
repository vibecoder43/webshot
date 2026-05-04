#pragma once

#include "expected.hpp"

#include <optional>
#include <utility>

namespace ws {

template <typename T> inline T GrabValueOf(std::optional<T> &opt)
{
    T x = std::move(*opt);
    opt.reset();
    return x;
}

template <typename T> inline T GrabValueOf(std::optional<T> &&opt) { return GrabValueOf(opt); }

template <typename T, typename E> inline T GrabValueOf(Expected<T, E> &expected)
{
    return std::move(*expected);
}

template <typename T, typename E> inline T GrabValueOf(Expected<T, E> &&expected)
{
    return std::move(*expected);
}

} // namespace ws
