#pragma once

#include "expected.hpp"
#include "integers.hpp"
#include "text.hpp"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <userver/clients/dns/resolver_fwd.hpp>

namespace ws::crawler {

namespace us = userver;
namespace eng = us::engine;
struct [[nodiscard]] EgressProxyConfig final {
    EgressProxyConfig(
        std::string socket_path, std::string run_id, usize url_bytes_max, i64 down_bytes_max,
        bool require_auth, bool enable_local_fixture_rewrite,
        std::vector<u16> testsuite_loopback_ports
    )
        : socket_path_(std::move(socket_path)), run_id(std::move(run_id)),
          url_bytes_max(url_bytes_max), down_bytes_max(down_bytes_max), require_auth(require_auth),
          enable_local_fixture_rewrite(enable_local_fixture_rewrite),
          testsuite_loopback_ports(std::move(testsuite_loopback_ports))
    {
    }

    std::string socket_path_;
    std::string run_id;
    usize url_bytes_max;
    i64 down_bytes_max;
    bool require_auth;
    bool enable_local_fixture_rewrite;
    std::vector<u16> testsuite_loopback_ports;
};

class [[nodiscard]] EgressProxy final {
public:
    [[nodiscard]] static Expected<std::unique_ptr<EgressProxy>, String>
    Create(EgressProxyConfig config, us::clients::dns::Resolver &resolver, eng::Deadline deadline);
    ~EgressProxy() noexcept;

    EgressProxy(const EgressProxy &) = delete;
    EgressProxy(EgressProxy &&) = delete;
    EgressProxy &operator=(const EgressProxy &) = delete;
    EgressProxy &operator=(EgressProxy &&) = delete;

    void Stop() noexcept;

    [[nodiscard]] i64 DownBytes() const noexcept;
    [[nodiscard]] std::optional<String> ErrorReason() const noexcept;

private:
    explicit EgressProxy(EgressProxyConfig config);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ws::crawler
