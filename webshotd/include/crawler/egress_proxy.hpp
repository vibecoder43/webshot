#pragma once

#include "expected.hpp"
#include "integers.hpp"
#include "text.hpp"
#include "userver_namespaces.hpp"

#include <cstddef>
#include <optional>
#include <string>

#include <userver/clients/dns/resolver_fwd.hpp>
#include <userver/utils/fast_pimpl.hpp>

namespace v1::crawler {

struct [[nodiscard]] EgressProxyConfig final {
    std::string socketPath;
    std::string runId;
    usize urlBytesMax;
    i64 downBytesMax;
    bool enableLocalFixtureRewrite;
};

class [[nodiscard]] EgressProxy final {
public:
    explicit EgressProxy(EgressProxyConfig config);
    ~EgressProxy() noexcept;

    EgressProxy(const EgressProxy &) = delete;
    EgressProxy(EgressProxy &&) = delete;
    EgressProxy &operator=(const EgressProxy &) = delete;
    EgressProxy &operator=(EgressProxy &&) = delete;

    [[nodiscard]] Expected<void, String>
    start(us::clients::dns::Resolver &resolver, eng::Deadline deadline);
    void close() noexcept;

    [[nodiscard]] i64 downBytes() const noexcept;
    [[nodiscard]] std::optional<String> failureReason() const noexcept;

private:
    struct Impl;
    static constexpr size_t kImplSize = 1024UL;
    static constexpr size_t kImplAlignment = 16UL;
    us::utils::FastPimpl<Impl, kImplSize, kImplAlignment> impl;
};

} // namespace v1::crawler
