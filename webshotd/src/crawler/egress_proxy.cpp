#include "egress_proxy.hpp"
/**
 * @file
 * @brief In-process forward proxy used by the sandboxed Chromium crawler.
 *
 * Implements a per-run Unix-socket HTTP proxy (CONNECT + plain HTTP) that
 * bridges the network namespace boundary and counts downstream bytes
 * (proxy -> browser). This proxy does not perform TLS MITM.
 */

#include "crypto.hpp"
#include "grab_value.hpp"
#include "integers.hpp"
#include "invariant.hpp"
#include "ip.hpp"
#include "try.hpp"

#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <exception>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <userver/clients/dns/exception.hpp>
#include <userver/clients/dns/resolver.hpp>
#include <userver/concurrent/variable.hpp>
#include <userver/engine/async.hpp>
#include <userver/engine/deadline.hpp>
#include <userver/engine/io/socket.hpp>
#include <userver/engine/task/task_with_result.hpp>
#include <userver/engine/wait_all_checked.hpp>
#include <userver/logging/log.hpp>
#include <userver/utils/assert.hpp>
#include <userver/utils/text_light.hpp>

namespace ws::crawler {
namespace us = userver;
namespace eng = us::engine;
namespace concurrent = us::concurrent;
namespace dns = us::clients::dns;
namespace utext = us::utils::text;
using namespace text::literals;
namespace {

constexpr auto kMaxHeaderBytes = 64_uz * 1024_uz;
constexpr size_t kIoBufferBytes = 16UL * 1024UL;
constexpr auto kMaxContentLengthBytes = 1024_i64 * 1024_i64 * 1024_i64;

const std::array<String, 3> kLocalFixtureHosts = {
    "test-target"_t,
    "asset.test-target"_t,
    "untrusted.test-target"_t,
};
const std::array<String, 3> kTestsuiteLoopbackHosts = {
    "127.0.0.1"_t,
    "localhost"_t,
    "::1"_t,
};
constexpr auto kLocalHttpPort = 18080_u16;
constexpr auto kLocalHttpsPort = 18443_u16;
constexpr auto kTestsuiteServicePort = 8080_u16;
constexpr auto kTestsuitePort = 8333_u16;
constexpr std::string_view kHttpScheme = "http://";
constexpr std::string_view kSlashPath = "/";
constexpr std::string_view kUnsupportedRequestTarget = "unsupported request target";
constexpr std::string_view kMissingHostHeader = "missing Host header";
constexpr std::string_view kInvalidHostHeader = "invalid Host header";
constexpr std::string_view kConnectEstablishedResponse =
    "HTTP/1.1 200 Connection Established\r\n\r\n";
constexpr std::string_view kProxyAuthRequiredResponse =
    "HTTP/1.1 407 Proxy Authentication Required\r\n"
    "Content-Type: text/plain; charset=utf-8\r\n"
    "Proxy-Authenticate: Basic realm=\"webshot\"\r\n"
    "Connection: close\r\n"
    "Content-Length: 30\r\n"
    "\r\n"
    "Proxy authentication required\n";

[[nodiscard]] bool ICaseEqual(std::string_view lhs, std::string_view rhs) noexcept
{
    return lhs.size() == rhs.size() && utext::ICaseStartsWith(lhs, rhs);
}

struct [[nodiscard]] HeaderLine final {
    std::string_view name;
    std::string_view value;
};

[[nodiscard]] std::optional<std::string_view>
FindHeaderValue(const std::vector<HeaderLine> &headers, std::string_view name) noexcept
{
    for (const auto &h : headers) {
        if (ICaseEqual(h.name, name))
            return h.value;
    }
    return {};
}

[[nodiscard]] std::optional<std::string_view> TakeCrlfLine(std::string_view &remaining) noexcept
{
    auto next = remaining.find("\r\n");
    if (next == std::string_view::npos)
        return {};

    auto line = remaining.substr(0, next);
    remaining.remove_prefix(next + 2);
    return line;
}

struct [[nodiscard]] ParsedRequest final {
    std::string_view method;
    std::string_view target;
    std::string_view version;
    std::vector<HeaderLine> headers;
    usize header_bytes{0};
};

[[nodiscard]] std::optional<ParsedRequest> ParseHeaderBlock(std::string_view bytes)
{
    const auto end_pos = bytes.find("\r\n\r\n");
    if (end_pos == std::string_view::npos)
        return {};

    const auto header_part = bytes.substr(0, end_pos + 4);
    auto remaining = header_part;
    const auto request_line = TakeCrlfLine(remaining);
    if (!request_line)
        return {};

    ParsedRequest out;
    out.header_bytes = unsize(header_part);
    {
        auto first_sp = request_line->find(' ');
        if (first_sp == std::string_view::npos)
            return {};
        auto second_sp = request_line->find(' ', first_sp + 1);
        if (second_sp == std::string_view::npos)
            return {};
        out.method = request_line->substr(0, first_sp);
        out.target = request_line->substr(first_sp + 1, second_sp - first_sp - 1);
        out.version = request_line->substr(second_sp + 1);
        if (out.method.empty() || out.target.empty() || out.version.empty())
            return {};
    }

    while (!remaining.empty()) {
        auto line = TakeCrlfLine(remaining);
        if (!line)
            return {};
        if (line->empty())
            break;

        auto colon = line->find(':');
        if (colon == std::string_view::npos)
            continue;
        auto name = line->substr(0, colon);
        auto value = line->substr(colon + 1);
        value = utext::TrimView(value);
        if (name.empty())
            continue;

        HeaderLine h{
            .name = name,
            .value = value,
        };
        out.headers.push_back(std::move(h));
    }
    return out;
}

[[nodiscard]] std::optional<std::pair<std::string, std::string>>
ParseBasicAuthUser(std::string_view header_value)
{
    auto value = header_value;
    auto sp = value.find(' ');
    if (sp == std::string_view::npos)
        return {};
    auto scheme = value.substr(0, sp);
    if (!ICaseEqual(scheme, "basic"))
        return {};
    value.remove_prefix(sp + 1);
    value = utext::TrimView(value);
    if (value.empty())
        return {};

    auto decoded = ws::crypto::Base64Decode(value, false);
    if (!decoded)
        return {};
    auto pos = decoded->find(':');
    if (pos == std::string::npos)
        return {};
    auto username = decoded->substr(0, pos);
    auto password = decoded->substr(pos + 1);
    return std::pair{std::move(username), std::move(password)};
}

[[nodiscard]] std::optional<u16> ParsePort(std::string_view port_text) noexcept
{
    if (port_text.empty())
        return {};

    auto port = integers::Parse<u16>(port_text);
    if (!port || *port == 0_u16)
        return {};
    return port;
}

enum class PortMode : bool { kOptional, kRequired };

struct [[nodiscard]] Authority final {
    std::string_view host;
    u16 port{0};
};

struct [[nodiscard]] ResolvedTcpAddress final {
    eng::io::Sockaddr sockaddr;
    Ip ip;
    String label;
};

[[nodiscard]] std::optional<Authority>
ParseAuthority(std::string_view authority, u16 default_port, PortMode port_mode) noexcept
{
    authority = utext::TrimView(authority);
    if (authority.empty())
        return {};

    std::string_view host{};
    std::string_view remainder{};

    if (authority.front() == '[') {
        auto close = authority.find(']');
        if (close == std::string_view::npos)
            return {};
        host = authority.substr(1, close - 1);
        remainder = authority.substr(close + 1);
    } else {
        auto first_colon = authority.find(':');
        auto last_colon = authority.rfind(':');
        if (first_colon != last_colon)
            return {};
        if (last_colon == std::string_view::npos) {
            if (port_mode == PortMode::kRequired)
                return {};
            return Authority{.host = authority, .port = default_port};
        }
        host = authority.substr(0, last_colon);
        remainder = authority.substr(last_colon);
    }

    if (host.empty())
        return {};
    if (remainder.empty()) {
        if (port_mode == PortMode::kRequired)
            return {};
        return Authority{.host = host, .port = default_port};
    }
    if (!remainder.starts_with(':'))
        return {};

    const auto port = ParsePort(remainder.substr(1));
    if (!port)
        return {};
    return Authority{.host = host, .port = *port};
}

[[nodiscard]] eng::io::Sockaddr SockaddrFromIp4(const Ip4 &ip, u16 port)
{
    sockaddr_in addr4{};
    addr4.sin_family = AF_INET;
    addr4.sin_port = htons(Raw(port));
    const auto &bytes = ip.GetBytes();
    std::memcpy(&addr4.sin_addr.s_addr, bytes.data(), bytes.size());
    return eng::io::Sockaddr(&addr4);
}

[[nodiscard]] eng::io::Sockaddr SockaddrFromIp6(const Ip6 &ip, u16 port)
{
    sockaddr_in6 addr6{};
    addr6.sin6_family = AF_INET6;
    addr6.sin6_port = htons(Raw(port));
    const auto &bytes = ip.GetBytes();
    std::memcpy(addr6.sin6_addr.s6_addr, bytes.data(), bytes.size());
    return eng::io::Sockaddr(&addr6);
}

[[nodiscard]] eng::io::Sockaddr SockaddrFromIp(const Ip &ip, u16 port)
{
    if (std::holds_alternative<Ip4>(ip))
        return SockaddrFromIp4(std::get<Ip4>(ip), port);
    return SockaddrFromIp6(std::get<Ip6>(ip), port);
}

[[nodiscard]] std::optional<Ip> IpFromSockaddr(const eng::io::Sockaddr &addr) noexcept
{
    if (addr.Domain() == eng::io::AddrDomain::kInet) {
        Ip4::BytesType bytes{};
        std::memcpy(bytes.data(), &addr.As<sockaddr_in>()->sin_addr.s_addr, bytes.size());
        return Ip4{bytes};
    }
    if (addr.Domain() == eng::io::AddrDomain::kInet6) {
        Ip6::BytesType bytes{};
        std::memcpy(bytes.data(), addr.As<sockaddr_in6>()->sin6_addr.s6_addr, bytes.size());
        return Ip6{bytes};
    }
    return {};
}

[[nodiscard]] String DescribeSockaddr(const eng::io::Sockaddr &addr)
{
    if (!addr.HasPort())
        return text::Format("{}", addr.PrimaryAddressString());
    if (addr.Domain() == eng::io::AddrDomain::kInet6)
        return text::Format("[{}]:{}", addr.PrimaryAddressString(), addr.Port());
    return text::Format("{}:{}", addr.PrimaryAddressString(), addr.Port());
}

[[nodiscard]] Expected<std::vector<ResolvedTcpAddress>, String> ResolveTcp(
    dns::Resolver &resolver, const String &host, u16 port, eng::Deadline deadline,
    bool allow_non_public_ip
)
{
    if (auto ip = ParseIp(host)) {
        if (!allow_non_public_ip && !IsPublicRoutable(*ip))
            return Unex(text::Format("ip address not public-routable for {}", host));

        auto addr = SockaddrFromIp(*ip, port);
        return {{
            {
                .sockaddr = addr,
                .ip = *ip,
                .label = DescribeSockaddr(addr),
            },
        }};
    }

    try {
        auto addrs = resolver.Resolve(host.ToBytes(), deadline);
        if (addrs.empty())
            return Unex(text::Format("dns resolve returned no addresses for {}", host));

        std::vector<ResolvedTcpAddress> resolved;
        resolved.reserve(addrs.size());
        for (auto &addr : addrs) {
            addr.SetPort(Raw(port));
            auto ip = IpFromSockaddr(addr);
            Invariant(ip, "dns resolver returned non-IP address"_t);
            if (!allow_non_public_ip && !IsPublicRoutable(*ip))
                continue;
            resolved.push_back(
                ResolvedTcpAddress{
                    .sockaddr = addr,
                    .ip = *ip,
                    .label = DescribeSockaddr(addr),
                }
            );
        }
        if (resolved.empty())
            return Unex(
                text::Format("dns resolve returned no public-routable addresses for {}", host)
            );
        return resolved;
    } catch (const dns::ResolverException &) {
        return Unex(text::Format("dns resolve failed for {}", host));
    }
}

struct [[nodiscard]] UpstreamTarget final {
    String connect_host;
    u16 connect_port{0};
    bool allow_non_public_ip{false};
};

[[nodiscard]] UpstreamTarget
RewriteLocalFixtureIfNeeded(const EgressProxyConfig &cfg, const String &host, u16 port)
{
    if (!cfg.enable_local_fixture_rewrite)
        return {.connect_host = host, .connect_port = port};

    const bool is_testsuite_loopback_host = std::ranges::contains(kTestsuiteLoopbackHosts, host);
    const bool is_testsuite_loopback_port =
        port == kTestsuiteServicePort || port == kTestsuitePort ||
        std::ranges::contains(cfg.testsuite_loopback_ports, port);
    if (is_testsuite_loopback_host && is_testsuite_loopback_port) {
        return {
            .connect_host = host,
            .connect_port = port,
            .allow_non_public_ip = true,
        };
    }

    auto is_local_host = std::ranges::contains(kLocalFixtureHosts, host);
    if (!is_local_host)
        return {.connect_host = host, .connect_port = port};

    if (port == 80_u16)
        return {
            .connect_host = "127.0.0.1"_t,
            .connect_port = kLocalHttpPort,
            .allow_non_public_ip = true,
        };
    if (port == 443_u16)
        return {
            .connect_host = "127.0.0.1"_t,
            .connect_port = kLocalHttpsPort,
            .allow_non_public_ip = true,
        };
    if (port == kLocalHttpPort || port == kLocalHttpsPort)
        return {
            .connect_host = "127.0.0.1"_t,
            .connect_port = port,
            .allow_non_public_ip = true,
        };
    return {.connect_host = host, .connect_port = port};
}

[[nodiscard]] std::string MakeErrorResponse(std::string_view status, std::string_view message)
{
    auto body = std::string(message) + "\n";
    return std::format(
        "{}\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "Connection: close\r\n"
        "Content-Length: {}\r\n"
        "\r\n"
        "{}",
        status, body.size(), body
    );
}

[[nodiscard]] std::string Make400Response(std::string_view message)
{
    return MakeErrorResponse("HTTP/1.1 400 Bad Request", message);
}

[[nodiscard]] std::string Make502Response(std::string_view message)
{
    return MakeErrorResponse("HTTP/1.1 502 Bad Gateway", message);
}

[[nodiscard]] bool ShouldForwardHeader(std::string_view name) noexcept
{
    return !ICaseEqual(name, "proxy-authorization") && !ICaseEqual(name, "proxy-connection") &&
           !ICaseEqual(name, "connection");
}

void AppendHeaderLine(std::string &out, std::string_view name, std::string_view value)
{
    out.append(name);
    out.append(": ");
    out.append(value);
    out.append("\r\n");
}

struct [[nodiscard]] HttpRequestTarget final {
    String host;
    u16 port{0};
    std::string_view path;
};

[[nodiscard]] Expected<HttpRequestTarget, std::string_view>
ParseHttpRequestTarget(const ParsedRequest &req)
{
    if (req.target.starts_with(kHttpScheme)) {
        auto rest = req.target.substr(kHttpScheme.size());
        auto slash = rest.find('/');
        auto authority = slash == std::string_view::npos ? rest : rest.substr(0, slash);
        auto parsed_authority = ParseAuthority(authority, 80_u16, PortMode::kOptional);
        if (!parsed_authority)
            return Unex(kUnsupportedRequestTarget);

        auto host_text = TRY_MAP_ERR(String::FromBytes(parsed_authority->host), [&](auto &&) {
            return kUnsupportedRequestTarget;
        });

        return HttpRequestTarget{
            .host = std::move(host_text),
            .port = parsed_authority->port,
            .path = slash == std::string_view::npos ? kSlashPath : rest.substr(slash),
        };
    }
    if (!req.target.starts_with('/'))
        return Unex(kUnsupportedRequestTarget);
    auto host_header = FindHeaderValue(req.headers, "host");
    if (!host_header)
        return Unex(kMissingHostHeader);

    auto parsed_host = ParseAuthority(*host_header, 0_u16, PortMode::kOptional);
    if (!parsed_host)
        return Unex(kInvalidHostHeader);

    auto host_text = TRY_MAP_ERR(String::FromBytes(parsed_host->host), [&](auto &&) {
        return kInvalidHostHeader;
    });

    return HttpRequestTarget{
        .host = std::move(host_text),
        .port = parsed_host->port == 0_u16 ? 80_u16 : parsed_host->port,
        .path = req.target,
    };
}

[[nodiscard]] Expected<i64, String> ParseContentLength(std::string_view header_value)
{
    auto digits = utext::TrimView(header_value);
    if (digits.empty())
        return Unex("invalid Content-Length"_t);

    auto content_length = integers::Parse<int64_t>(digits);
    if (!content_length)
        return Unex("invalid Content-Length"_t);
    if (*content_length < 0 || *content_length > Raw(kMaxContentLengthBytes))
        return Unex("invalid Content-Length"_t);

    return i64{*content_length};
}

[[nodiscard]] std::string MakeForwardRequest(const ParsedRequest &req, std::string_view path)
{
    std::string out;
    out.reserve(NumericCast<size_t>(req.header_bytes + 64_uz + unsize(path)));
    out.append(req.method);
    out.push_back(' ');
    out.append(path);
    out.push_back(' ');
    out.append(req.version);
    out.append("\r\n");

    for (const auto &header : req.headers) {
        if (!ShouldForwardHeader(header.name))
            continue;
        AppendHeaderLine(out, header.name, header.value);
    }

    out.append("Connection: close\r\n\r\n");
    return out;
}

} // namespace

struct EgressProxy::Impl final {
    explicit Impl(EgressProxyConfig cfg) : config(std::move(cfg)) {}

    EgressProxyConfig config;
    concurrent::Variable<i64> down_bytes_{0};
    std::atomic<bool> stopped{false};
    concurrent::Variable<std::optional<String>> error;
    eng::io::Socket listener;
    std::optional<eng::TaskWithResult<void>> accept_task;
    concurrent::Variable<std::vector<eng::TaskWithResult<void>>> client_tasks;

    [[nodiscard]] bool IsStopped() const noexcept { return stopped.load(); }

    void NoteError(String reason) noexcept
    {
        auto lock = error.Lock();
        if (*lock)
            return;
        *lock = std::move(reason);
    }

    void RequestCancelAllClientTasksNoWait() noexcept
    {
        if (accept_task)
            accept_task->RequestCancel();
        auto lock = client_tasks.Lock();
        for (auto &task : *lock)
            task.RequestCancel();
    }

    [[nodiscard]] usize
    SendBudgeted(eng::io::Socket &sock, std::span<const char> bytes, eng::Deadline deadline)
    {
        if (bytes.empty())
            return 0_uz;
        if (IsStopped())
            return 0_uz;

        const auto max_claim = std::min<i64>(ssize(bytes), config.down_bytes_max);
        if (max_claim <= 0_i64)
            return 0_uz;
        {
            auto counter = down_bytes_.Lock();
            if (*counter > config.down_bytes_max - max_claim) {
                NoteError(
                    text::Format(
                        "net_limit: proxy downstream bytes {} exceeded limit {}", *counter,
                        config.down_bytes_max
                    )
                );
                stopped.store(true);
                RequestCancelAllClientTasksNoWait();
                return 0_uz;
            }
            *counter += max_claim;
        }

        try {
            auto sent = sock.SendAll(bytes.data(), NumericCast<size_t>(max_claim), deadline);
            auto unused = max_claim - i64{sent};
            if (unused > 0_i64) {
                auto counter = down_bytes_.Lock();
                *counter -= unused;
            }
            return {sent};
        } catch (const std::exception &) {
            auto counter = down_bytes_.Lock();
            *counter -= max_claim;
            return 0_uz;
        }
    }

    [[nodiscard]] bool
    SendAll(eng::io::Socket &sock, std::span<const char> bytes, eng::Deadline deadline) noexcept
    {
        if (bytes.empty())
            return true;

        try {
            static_cast<void>(sock.SendAll(bytes.data(), bytes.size(), deadline));
            return true;
        } catch (const std::exception &e) {
            LOG_WARNING() << std::format("SendAll failed: {}", e.what());
            return false;
        }
    }

    void Send407(eng::io::Socket &client, eng::Deadline deadline)
    {
        static_cast<void>(SendBudgeted(client, kProxyAuthRequiredResponse, deadline));
    }

    void Send400(eng::io::Socket &client, std::string_view message, eng::Deadline deadline)
    {
        auto response = Make400Response(message);
        static_cast<void>(SendBudgeted(client, response, deadline));
    }

    void Send502(eng::io::Socket &client, std::string_view message, eng::Deadline deadline)
    {
        auto response = Make502Response(message);
        static_cast<void>(SendBudgeted(client, response, deadline));
    }

    void CloseSocketsQuietly(eng::io::Socket &a, eng::io::Socket &b) noexcept
    {
        try {
            a.Close();
        } catch (const std::exception &e) {
            LOG_WARNING() << std::format("CloseSocketsQuietly: close failed: {}", e.what());
        }
        try {
            b.Close();
        } catch (const std::exception &e) {
            LOG_WARNING() << std::format("CloseSocketsQuietly: close failed: {}", e.what());
        }
    }

    void ShutdownWriteQuietly(eng::io::Socket &sock) noexcept
    {
        if (!sock.IsValid())
            return;
        static_cast<void>(::shutdown(sock.Fd(), SHUT_WR));
    }

    [[nodiscard]] Expected<eng::io::Socket, String>
    ConnectUpstream(dns::Resolver &resolver, const String &host, u16 port, eng::Deadline deadline)
    {
        const auto upstream = RewriteLocalFixtureIfNeeded(config, host, port);
        auto addrs = TRY(ResolveTcp(
            resolver, upstream.connect_host, upstream.connect_port, deadline,
            upstream.allow_non_public_ip
        ));

        std::vector<String> errors;
        errors.reserve(addrs.size());
        for (const auto &addr : addrs) {
            eng::io::Socket socket{addr.sockaddr.Domain(), eng::io::SocketType::kStream};
            try {
                socket.Connect(addr.sockaddr, deadline);
                return socket;
            } catch (const std::exception &e) {
                errors.push_back(text::Format("{} ({})", addr.label, e.what()));
            }
        }

        String details;
        for (const auto &cur : errors) {
            if (!details.Empty())
                details += "; "_t;
            details += cur;
        }
        return Unex(
            text::Format(
                "connect upstream failed for {}:{} after {} attempt(s): {}", upstream.connect_host,
                upstream.connect_port, errors.size(), details
            )
        );
    }

    void CopyClientToUpstream(
        eng::io::Socket &client, eng::io::Socket &upstream, eng::Deadline deadline
    ) noexcept
    {
        std::array<char, kIoBufferBytes> storage{};
        std::span<char> buffer{storage};
        try {
            while (!IsStopped()) {
                const usize received{client.RecvSome(buffer.data(), buffer.size(), deadline)};
                if (received == 0_uz) {
                    ShutdownWriteQuietly(upstream);
                    return;
                }
                if (!SendAll(
                        upstream, std::span<const char>{buffer.data(), Raw(received)}, deadline
                    )) {
                    return;
                }
            }
        } catch (const std::exception &e) {
            LOG_WARNING() << std::format("CopyClientToUpstream: {}", e.what());
            return;
        }
    }

    void CopyUpstreamToClientBudgeted(
        eng::io::Socket &upstream, eng::io::Socket &client, eng::Deadline deadline
    )
    {
        std::array<char, kIoBufferBytes> storage{};
        std::span<char> buffer{storage};
        try {
            while (!IsStopped()) {
                const usize received{upstream.RecvSome(buffer.data(), buffer.size(), deadline)};
                if (received == 0_uz) {
                    ShutdownWriteQuietly(client);
                    return;
                }

                std::span<const char> pending{buffer.data(), Raw(received)};
                while (!pending.empty() && !IsStopped()) {
                    auto sent = SendBudgeted(client, pending, deadline);
                    if (sent == 0_uz)
                        return;
                    pending = pending.subspan(Raw(sent));
                }
            }
        } catch (const std::exception &e) {
            LOG_WARNING() << std::format("CopyUpstreamToClientBudgeted: {}", e.what());
            return;
        }
    }

    void RelayConnect(eng::io::Socket &client, eng::io::Socket &upstream, eng::Deadline deadline)
    {
        auto client_to_upstream = eng::AsyncNoSpan([&]() {
            CopyClientToUpstream(client, upstream, deadline);
        });
        auto upstream_to_client = eng::AsyncNoSpan([&]() {
            CopyUpstreamToClientBudgeted(upstream, client, deadline);
        });

        auto status = eng::WaitAllCheckedUntil(deadline, client_to_upstream, upstream_to_client);
        if (status != eng::FutureStatus::kReady) {
            client_to_upstream.RequestCancel();
            upstream_to_client.RequestCancel();
        }
        static_cast<void>(client_to_upstream.WaitNothrow());
        static_cast<void>(upstream_to_client.WaitNothrow());
    }

    void ForwardHttpRequestBody(
        eng::io::Socket &client, eng::io::Socket &upstream, std::string_view buffered_body,
        i64 remaining_body, eng::Deadline deadline
    )
    {
        auto already_buffered = buffered_body.substr(
            0, NumericCast<size_t>(std::max(0_i64, std::min(remaining_body, ssize(buffered_body))))
        );
        if (!SendAll(upstream, already_buffered, deadline))
            return;
        remaining_body -= ssize(already_buffered);

        std::array<char, kIoBufferBytes> storage{};
        std::span<char> buffer{storage};
        while (remaining_body > 0_i64 && !IsStopped()) {
            auto want = NumericCast<size_t>(std::min(remaining_body, ssize(buffer)));
            auto received = 0_uz;
            try {
                received = usize{client.RecvSome(buffer.data(), want, deadline)};
            } catch (const std::exception &) {
                break;
            }
            if (received == 0_uz)
                break;
            remaining_body -= i64(received);
            if (!SendAll(upstream, std::span<const char>{buffer.data(), Raw(received)}, deadline))
                return;
        }
    }

    void HandleConnect(
        dns::Resolver &resolver, eng::io::Socket &client, const ParsedRequest &req,
        eng::Deadline deadline
    )
    {
        auto authority = ParseAuthority(req.target, 0_u16, PortMode::kRequired);
        if (!authority) {
            Send400(client, "invalid CONNECT target", deadline);
            return;
        }

        auto host_text = String::FromBytes(authority->host);
        if (!host_text) {
            Send400(client, "invalid CONNECT target", deadline);
            return;
        }

        auto upstream = ConnectUpstream(resolver, *host_text, authority->port, deadline);
        if (!upstream) {
            NoteError(upstream.Error());
            Send502(client, upstream.Error().View(), deadline);
            return;
        }

        auto upstream_socket = GrabValueOf(upstream);
        static_cast<void>(SendBudgeted(client, kConnectEstablishedResponse, deadline));
        if (IsStopped()) {
            CloseSocketsQuietly(client, upstream_socket);
            return;
        }

        RelayConnect(client, upstream_socket, deadline);
        CloseSocketsQuietly(client, upstream_socket);
    }

    void HandleHttp(
        dns::Resolver &resolver, eng::io::Socket &client, const ParsedRequest &req,
        std::string_view header_bytes, eng::Deadline deadline
    )
    {
        auto target = ParseHttpRequestTarget(req);
        if (!target) {
            Send400(client, target.Error(), deadline);
            return;
        }

        i64 content_length{0};
        if (const auto header = FindHeaderValue(req.headers, "content-length")) {
            auto parsed_content_length = ParseContentLength(*header);
            if (!parsed_content_length) {
                Send400(client, parsed_content_length.Error().View(), deadline);
                return;
            }
            content_length = *parsed_content_length;
        }

        auto upstream = ConnectUpstream(resolver, target->host, target->port, deadline);
        if (!upstream) {
            NoteError(upstream.Error());
            Send502(client, upstream.Error().View(), deadline);
            return;
        }

        auto upstream_socket = GrabValueOf(upstream);
        const auto request = MakeForwardRequest(req, target->path);
        if (!SendAll(upstream_socket, request, deadline)) {
            NoteError("send upstream failed"_t);
            Send502(client, "send upstream failed", deadline);
            CloseSocketsQuietly(client, upstream_socket);
            return;
        }

        ForwardHttpRequestBody(
            client, upstream_socket, header_bytes.substr(NumericCast<size_t>(req.header_bytes)),
            content_length, deadline
        );
        CopyUpstreamToClientBudgeted(upstream_socket, client, deadline);
        CloseSocketsQuietly(client, upstream_socket);
    }

    void HandleClient(dns::Resolver &resolver, eng::io::Socket client, eng::Deadline deadline)
    {
        if (IsStopped())
            return;

        std::string header;
        header.reserve(2048);
        std::array<char, kIoBufferBytes> storage{};
        std::span<char> buffer{storage};
        try {
            while (!header.contains("\r\n\r\n")) {
                if (unsize(header) > kMaxHeaderBytes) {
                    Send400(client, "header too large", deadline);
                    client.Close();
                    return;
                }
                const usize received{client.RecvSome(buffer.data(), buffer.size(), deadline)};
                if (received == 0_uz)
                    return;
                header.append(buffer.data(), Raw(received));
            }
        } catch (const std::exception &) {
            return;
        }

        const auto parsed = ParseHeaderBlock(header);
        if (!parsed) {
            Send400(client, "invalid request", deadline);
            return;
        }
        if (unsize(parsed->target) > config.url_bytes_max) {
            Send400(client, "target too long", deadline);
            return;
        }

        if (config.require_auth) {
            auto auth = FindHeaderValue(parsed->headers, "proxy-authorization");
            auto user = auth ? ParseBasicAuthUser(*auth)
                             : std::optional<std::pair<std::string, std::string>>{};
            if (!user || user->first != config.run_id) {
                Send407(client, deadline);
                return;
            }
        }

        if (ICaseEqual(parsed->method, "CONNECT")) {
            HandleConnect(resolver, client, *parsed, deadline);
            return;
        }
        HandleHttp(resolver, client, *parsed, header, deadline);
    }

    void AcceptLoop(dns::Resolver &resolver, eng::Deadline deadline)
    {
        while (!IsStopped()) {
            eng::io::Socket client;
            try {
                client = listener.Accept(deadline);
            } catch (const std::exception &e) {
                LOG_WARNING() << std::format("AcceptLoop accept error: {}", e.what());
                return;
            }
            if (IsStopped())
                return;
            auto task = eng::AsyncNoSpan([this, &resolver, deadline,
                                          sock = std::move(client)]() mutable {
                HandleClient(resolver, std::move(sock), deadline);
            });
            auto lock = client_tasks.Lock();
            lock->push_back(std::move(task));
        }
    }

    void StopAll() noexcept
    {
        stopped.store(true);
        if (accept_task) {
            accept_task->RequestCancel();
            static_cast<void>(accept_task->WaitNothrow());
            accept_task.reset();
        }
        std::vector<eng::TaskWithResult<void>> tasks;
        {
            auto lock = client_tasks.Lock();
            tasks = std::move(*lock);
            lock->clear();
        }
        for (auto &t : tasks) {
            t.RequestCancel();
        }
        for (auto &t : tasks) {
            static_cast<void>(t.WaitNothrow());
        }
        try {
            if (listener.IsValid())
                listener.Close();
        } catch (const std::exception &e) {
            LOG_WARNING() << std::format("StopAll listener close failed: {}", e.what());
        }
    }
};

EgressProxy::EgressProxy(EgressProxyConfig config)
    : impl_{std::make_unique<Impl>(std::move(config))}
{
    Invariant(!impl_->config.socket_path_.empty(), "proxy socket path must not be empty"_t);
    Invariant(!impl_->config.run_id.empty(), "proxy runId must not be empty"_t);
}

EgressProxy::~EgressProxy() noexcept { Stop(); }

Expected<std::unique_ptr<EgressProxy>, String>
EgressProxy::Make(EgressProxyConfig config, dns::Resolver &resolver, eng::Deadline deadline)
{
    Invariant(deadline.IsReachable(), "proxy start deadline must be reachable"_t);

    auto proxy = std::unique_ptr<EgressProxy>{new EgressProxy(std::move(config))};
    auto &impl = *proxy->impl_;

    impl.listener = eng::io::Socket(eng::io::AddrDomain::kUnix, eng::io::SocketType::kStream);
    try {
        auto addr = eng::io::Sockaddr::MakeUnixSocketAddress(impl.config.socket_path_);
        impl.listener.Bind(addr);
        impl.listener.Listen();
    } catch (const std::exception &e) {
        return Unex(text::Format("proxy bind then listen failed: {}", e.what()));
    }

    impl.accept_task = eng::AsyncNoSpan([&impl, &resolver, deadline]() {
        impl.AcceptLoop(resolver, deadline);
    });
    return proxy;
}

void EgressProxy::Stop() noexcept { impl_->StopAll(); }

i64 EgressProxy::DownBytes() const noexcept
{
    auto locked = impl_->down_bytes_.Lock();
    return {*locked};
}

std::optional<String> EgressProxy::ErrorReason() const noexcept
{
    auto lock = impl_->error.Lock();
    return *lock;
}

} // namespace ws::crawler
