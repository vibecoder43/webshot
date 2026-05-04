#pragma once

#include "expected.hpp"

#include <functional>
#include <string>
#include <type_traits>
#include <utility>

#include <userver/storages/postgres/cluster.hpp>
#include <userver/storages/postgres/exceptions.hpp>
#include <userver/storages/postgres/transaction.hpp>

namespace ws {

namespace us = userver;
namespace pg = us::storages::postgres;
struct [[nodiscard]] PgError final {
    std::string what;
};

namespace pgx {

namespace detail {

template <typename F> using InvokeResult = std::invoke_result_t<F>;
template <typename F> using ExpectedValue = std::remove_cvref_t<InvokeResult<F>>;

template <typename F> [[nodiscard]] auto CatchPg(F &&f) -> Expected<ExpectedValue<F>, PgError>
{
    try {
        if constexpr (std::is_void_v<InvokeResult<F>>) {
            std::invoke(std::forward<F>(f));
            return {};
        } else {
            return std::invoke(std::forward<F>(f));
        }
    } catch (const pg::Error &e) {
        return Unex(PgError{.what = std::string(e.what())});
    }
}

} // namespace detail

template <pg::ClusterHostType Host, typename F, typename... Ts>
[[nodiscard]] auto Execute(const pg::ClusterPtr &cluster, F &&f, Ts &&...args)
{
    using Res = decltype(cluster->Execute(Host, std::forward<Ts>(args)...));
    using R = std::remove_cvref_t<std::invoke_result_t<F, Res &>>;

    return detail::CatchPg([&]() -> R {
        auto res = cluster->Execute(Host, std::forward<Ts>(args)...);
        if constexpr (std::is_void_v<R>) {
            std::invoke(std::forward<F>(f), res);
        } else {
            return std::invoke(std::forward<F>(f), res);
        }
    });
}

template <typename F> [[nodiscard]] auto ReadwriteTransaction(const pg::ClusterPtr &cluster, F &&f)
{
    using Trx = decltype(cluster->Begin(pg::ClusterHostType::kMaster, pg::Transaction::RW));
    using R = std::remove_cvref_t<std::invoke_result_t<F, Trx &>>;

    return detail::CatchPg([&]() -> R {
        auto trx = cluster->Begin(pg::ClusterHostType::kMaster, pg::Transaction::RW);
        if constexpr (std::is_void_v<R>) {
            std::invoke(std::forward<F>(f), trx);
        } else {
            return std::invoke(std::forward<F>(f), trx);
        }
    });
}

} // namespace pgx

} // namespace ws
