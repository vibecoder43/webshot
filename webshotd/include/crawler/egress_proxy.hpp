#pragma once

#include "expected.hpp"
#include "integers.hpp"
#include "text.hpp"
#include "userver_namespaces.hpp"

#include <memory>
#include <optional>
#include <string>

#include <userver/clients/dns/resolver_fwd.hpp>

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
    std::unique_ptr<Impl> impl;
};

} // namespace v1::crawler
