#include "crawler/egress_proxy.hpp"
/**
 * @file
 * @brief In-process forward proxy used by the sandboxed Chromium crawler.
 *
 * Implements a per-run Unix-socket HTTP proxy (CONNECT + plain HTTP) that
 * bridges the network namespace boundary and counts downstream bytes
 * (proxy -> browser). This proxy does not perform TLS MITM.
 */

#include "integers.hpp"
#include "ip_utils.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <userver/clients/dns/exception.hpp>
#include <userver/clients/dns/resolver.hpp>
#include <userver/concurrent/variable.hpp>
#include <userver/crypto/base64.hpp>
#include <userver/crypto/exception.hpp>
#include <userver/engine/async.hpp>
#include <userver/engine/deadline.hpp>
#include <userver/engine/io/socket.hpp>
#include <userver/engine/task/task_with_result.hpp>
#include <userver/engine/wait_any.hpp>
#include <userver/utils/assert.hpp>
#include <userver/utils/traceful_exception.hpp>

#include <absl/strings/ascii.h>
#include <absl/strings/match.h>
#include <absl/strings/strip.h>

namespace v1::crawler {
using namespace text::literals;
namespace concurrent = us::concurrent;
namespace crypto = us::crypto;
namespace dns = us::clients::dns;
namespace engine = eng;
namespace utils = us::utils;

namespace {

constexpr auto kMaxHeaderBytes = 64_uz * 1024_uz;
constexpr size_t kIoBufferBytes = 16UL * 1024UL;
constexpr auto kMaxContentLengthBytes = 1024_i64 * 1024_i64 * 1024_i64;

constexpr std::string_view kLocalHostA = "test-target";
constexpr std::string_view kLocalHostB = "asset.test-target";
constexpr auto kLocalHttpPort = 18080_u16;
constexpr auto kLocalHttpsPort = 18443_u16;
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

struct [[nodiscard]] HeaderLine final {
    std::string nameLower;
    std::string_view name;
    std::string_view value;
};

[[nodiscard]] std::optional<std::string_view>
findHeaderValue(const std::vector<HeaderLine> &headers, std::string_view nameLower) noexcept
{
    for (const auto &h : headers) {
        if (h.nameLower == nameLower)
            return h.value;
    }
    return {};
}

[[nodiscard]] std::optional<std::string_view> takeCrlfLine(std::string_view &remaining) noexcept
{
    const auto next = remaining.find("\r\n");
    if (next == std::string_view::npos)
        return {};

    const auto line = remaining.substr(0, next);
    remaining.remove_prefix(next + 2);
    return line;
}

struct [[nodiscard]] ParsedRequest final {
    std::string_view method;
    std::string_view target;
    std::string_view version;
    std::vector<HeaderLine> headers;
    usize headerBytes{0};
};

[[nodiscard]] std::optional<ParsedRequest> parseHeaderBlock(std::string_view bytes)
{
    const auto endPos = bytes.find("\r\n\r\n");
    if (endPos == std::string_view::npos)
        return {};

    const auto headerPart = bytes.substr(0, endPos + 4);
    auto remaining = headerPart;
    const auto requestLine = takeCrlfLine(remaining);
    if (!requestLine)
        return {};

    ParsedRequest out;
    out.headerBytes = usz(headerPart);
    {
        const auto firstSp = requestLine->find(' ');
        if (firstSp == std::string_view::npos)
            return {};
        const auto secondSp = requestLine->find(' ', firstSp + 1);
        if (secondSp == std::string_view::npos)
            return {};
        out.method = requestLine->substr(0, firstSp);
        out.target = requestLine->substr(firstSp + 1, secondSp - firstSp - 1);
        out.version = requestLine->substr(secondSp + 1);
        if (out.method.empty() || out.target.empty() || out.version.empty())
            return {};
    }

    while (!remaining.empty()) {
        const auto line = takeCrlfLine(remaining);
        if (!line)
            return {};
        if (line->empty())
            break;

        const auto colon = line->find(':');
        if (colon == std::string_view::npos)
            continue;
        const auto name = line->substr(0, colon);
        auto value = line->substr(colon + 1);
        value = absl::StripAsciiWhitespace(value);
        if (name.empty())
            continue;

        auto lowerName = std::string(name);
        absl::AsciiStrToLower(&lowerName);
        HeaderLine h{
            .nameLower = std::move(lowerName),
            .name = name,
            .value = value,
        };
        out.headers.push_back(std::move(h));
    }
    return out;
}

[[nodiscard]] std::optional<std::pair<std::string, std::string>>
parseBasicAuthUser(std::string_view headerValue)
{
    auto value = headerValue;
    const auto sp = value.find(' ');
    if (sp == std::string_view::npos)
        return {};
    const auto scheme = value.substr(0, sp);
    if (!absl::EqualsIgnoreCase(scheme, "basic"))
        return {};
    value.remove_prefix(sp + 1);
    value = absl::StripLeadingAsciiWhitespace(value);
    if (value.empty())
        return {};

    try {
        auto decoded = crypto::base64::Base64Decode(std::string(value));
        auto pos = decoded.find(':');
        if (pos == std::string::npos)
            return {};
        auto username = decoded.substr(0, pos);
        auto password = decoded.substr(pos + 1);
        return std::pair{std::move(username), std::move(password)};
    } catch (const crypto::CryptoException &) {
        return {};
    }
}

[[nodiscard]] std::optional<u16> parsePort(std::string_view portText) noexcept
{
    if (portText.empty())
        return {};

    unsigned int port = 0;
    const auto *const begin = portText.data();
    const auto *const end = begin + portText.size();
    const auto result = std::from_chars(begin, end, port);
    if (result.ec != std::errc{} || result.ptr != end)
        return {};
    if (port == 0 || port > raw(65535_u16))
        return {};
    return u16(port);
}

enum class PortMode : bool { kOptional, kRequired };

struct [[nodiscard]] Authority final {
    std::string_view host;
    u16 port;
};

[[nodiscard]] std::optional<Authority>
parseAuthority(std::string_view authority, u16 defaultPort, PortMode portMode) noexcept
{
    authority = absl::StripAsciiWhitespace(authority);
    if (authority.empty())
        return {};

    auto host = std::string_view{};
    auto remainder = std::string_view{};

    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos)
            return {};
        host = authority.substr(1, close - 1);
        remainder = authority.substr(close + 1);
    } else {
        const auto firstColon = authority.find(':');
        const auto lastColon = authority.rfind(':');
        if (firstColon != lastColon)
            return {};
        if (lastColon == std::string_view::npos) {
            if (portMode == PortMode::kRequired)
                return {};
            return Authority{.host = authority, .port = defaultPort};
        }
        host = authority.substr(0, lastColon);
        remainder = authority.substr(lastColon);
    }

    if (host.empty())
        return {};
    if (remainder.empty()) {
        if (portMode == PortMode::kRequired)
            return {};
        return Authority{.host = host, .port = defaultPort};
    }
    if (!remainder.starts_with(':'))
        return {};

    const auto port = parsePort(remainder.substr(1));
    if (!port)
        return {};
    return Authority{.host = host, .port = *port};
}

[[nodiscard]] engine::io::Sockaddr sockaddrFromIpv4(std::string_view host, u16 port)
{
    sockaddr_in addr4{};
    addr4.sin_family = AF_INET;
    addr4.sin_port = htons(raw(port));
    const auto hostText = std::string(host);
    UINVARIANT(inet_pton(AF_INET, hostText.c_str(), &addr4.sin_addr) == 1, "invalid ipv4 addr");
    return engine::io::Sockaddr(&addr4);
}

[[nodiscard]] engine::io::Sockaddr sockaddrFromIpv6(std::string_view host, u16 port)
{
    auto candidate = host;
    if (!candidate.empty() && candidate.front() == '[' && candidate.back() == ']')
        candidate = candidate.substr(1, candidate.size() - 2);
    sockaddr_in6 addr6{};
    addr6.sin6_family = AF_INET6;
    addr6.sin6_port = htons(raw(port));
    const auto hostText = std::string(candidate);
    UINVARIANT(inet_pton(AF_INET6, hostText.c_str(), &addr6.sin6_addr) == 1, "invalid ipv6 addr");
    return engine::io::Sockaddr(&addr6);
}

[[nodiscard]] Expected<engine::io::Sockaddr, String>
resolveTcp(dns::Resolver &resolver, std::string_view host, u16 port, engine::Deadline deadline)
{
    const auto hostText = std::string(host);
    if (isIpv4Address(hostText))
        return sockaddrFromIpv4(hostText, port);
    if (isIpv6Address(hostText))
        return sockaddrFromIpv6(hostText, port);

    try {
        auto addrs = resolver.Resolve(hostText, deadline);
        if (addrs.empty())
            return std::unexpected(
                text::format("dns resolve returned no addresses for {}", hostText)
            );
        addrs.front().SetPort(raw(port));
        return addrs.front();
    } catch (const dns::NotResolvedException &) {
        return std::unexpected(text::format("dns resolve failed for {}", hostText));
    }
}

struct [[nodiscard]] UpstreamTarget final {
    std::string connectHost;
    u16 connectPort;
};

[[nodiscard]] UpstreamTarget
rewriteLocalFixtureIfNeeded(const EgressProxyConfig &cfg, std::string_view host, u16 port)
{
    if (!cfg.enableLocalFixtureRewrite)
        return UpstreamTarget{.connectHost = std::string(host), .connectPort = port};

    const bool isLocalHost = host == kLocalHostA || host == kLocalHostB;
    if (!isLocalHost)
        return UpstreamTarget{.connectHost = std::string(host), .connectPort = port};

    if (port == 80_u16)
        return UpstreamTarget{.connectHost = "127.0.0.1", .connectPort = kLocalHttpPort};
    if (port == 443_u16)
        return UpstreamTarget{.connectHost = "127.0.0.1", .connectPort = kLocalHttpsPort};
    return UpstreamTarget{.connectHost = std::string(host), .connectPort = port};
}

[[nodiscard]] std::string makeErrorResponse(std::string_view status, std::string_view message)
{
    const auto body = std::string(message) + "\n";
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

[[nodiscard]] std::string make400Response(std::string_view message)
{
    return makeErrorResponse("HTTP/1.1 400 Bad Request", message);
}

[[nodiscard]] std::string make502Response(std::string_view message)
{
    return makeErrorResponse("HTTP/1.1 502 Bad Gateway", message);
}

[[nodiscard]] bool shouldForwardHeader(std::string_view nameLower) noexcept
{
    return nameLower != "proxy-authorization" && nameLower != "proxy-connection" &&
           nameLower != "connection";
}

void appendHeaderLine(std::string &out, std::string_view name, std::string_view value)
{
    out.append(name);
    out.append(": ");
    out.append(value);
    out.append("\r\n");
}

struct [[nodiscard]] HttpRequestTarget final {
    std::string_view host;
    u16 port;
    std::string_view path;
};

[[nodiscard]] Expected<HttpRequestTarget, std::string_view>
parseHttpRequestTarget(const ParsedRequest &req)
{
    if (req.target.starts_with("http://")) {
        auto rest = req.target.substr(std::string_view{"http://"}.size());
        const auto slash = rest.find('/');
        const auto authority = slash == std::string_view::npos ? rest : rest.substr(0, slash);
        const auto parsedAuthority = parseAuthority(authority, 80_u16, PortMode::kOptional);
        if (!parsedAuthority)
            return std::unexpected(std::string_view{"unsupported request target"});

        return HttpRequestTarget{
            .host = parsedAuthority->host,
            .port = parsedAuthority->port,
            .path = slash == std::string_view::npos ? std::string_view{"/"} : rest.substr(slash),
        };
    }

    if (!req.target.starts_with('/'))
        return std::unexpected(std::string_view{"unsupported request target"});

    const auto hostHeader = findHeaderValue(req.headers, "host");
    if (!hostHeader)
        return std::unexpected(std::string_view{"missing Host header"});

    const auto parsedHost = parseAuthority(hostHeader.value(), 0_u16, PortMode::kOptional);
    if (!parsedHost)
        return std::unexpected(std::string_view{"invalid Host header"});

    return HttpRequestTarget{
        .host = parsedHost->host,
        .port = parsedHost->port == 0_u16 ? 80_u16 : parsedHost->port,
        .path = req.target,
    };
}

[[nodiscard]] Expected<i64, String> parseContentLength(std::string_view headerValue)
{
    const auto digits = absl::StripAsciiWhitespace(headerValue);
    if (digits.empty())
        return std::unexpected("invalid Content-Length"_t);

    int64_t contentLength = 0;
    const auto *const begin = digits.data();
    const auto *const end = begin + digits.size();
    const auto result = std::from_chars(begin, end, contentLength);
    if (result.ec != std::errc{} || result.ptr != end)
        return std::unexpected("invalid Content-Length"_t);
    if (contentLength < 0 || contentLength > raw(kMaxContentLengthBytes))
        return std::unexpected("invalid Content-Length"_t);

    return i64(contentLength);
}

[[nodiscard]] std::string buildForwardRequest(const ParsedRequest &req, std::string_view path)
{
    std::string out;
    out.reserve(numericCast<size_t>(req.headerBytes + 64_uz + usz(path)));
    out.append(req.method);
    out.push_back(' ');
    out.append(path);
    out.push_back(' ');
    out.append(req.version);
    out.append("\r\n");

    for (const auto &header : req.headers) {
        if (!shouldForwardHeader(header.nameLower))
            continue;
        appendHeaderLine(out, header.name, header.value);
    }

    out.append("Connection: close\r\n\r\n");
    return out;
}

} // namespace

struct EgressProxy::Impl final {
    explicit Impl(EgressProxyConfig cfg) : config(std::move(cfg)) {}

    EgressProxyConfig config;
    std::atomic<int64_t> downBytes{};
    std::atomic<bool> closed{false};
    concurrent::Variable<std::optional<String>> failure;
    engine::io::Socket listener;
    std::optional<engine::TaskWithResult<void>> acceptTask;
    concurrent::Variable<std::vector<engine::TaskWithResult<void>>> clientTasks;

    [[nodiscard]] bool isClosed() const noexcept { return closed.load(std::memory_order_relaxed); }

    void noteFailure(String reason) noexcept
    {
        auto lock = failure.Lock();
        if (*lock)
            return;
        *lock = std::move(reason);
    }

    void requestCancelAllClientTasksNoWait() noexcept
    {
        if (acceptTask)
            acceptTask->RequestCancel();
        auto lock = clientTasks.Lock();
        for (auto &task : *lock)
            task.RequestCancel();
    }

    void accountDownBytes(i64 bytes) noexcept
    {
        if (bytes <= 0_i64)
            return;
        const auto delta = raw(bytes);
        const auto next = downBytes.fetch_add(delta, std::memory_order_relaxed) + delta;
        if (next <= raw(config.downBytesMax))
            return;
        noteFailure(
            text::format(
                "net_limit: proxy downstream bytes {} exceeded limit {}", next, config.downBytesMax
            )
        );
        closed.store(true, std::memory_order_relaxed);
        requestCancelAllClientTasksNoWait();
    }

    [[nodiscard]] usize sendBudgeted(
        engine::io::Socket &sock, std::span<const char> bytes, engine::Deadline deadline
    ) noexcept
    {
        if (isClosed() || bytes.empty())
            return 0_uz;
        const auto used = i64(downBytes.load(std::memory_order_relaxed));
        const auto remaining = config.downBytesMax - used;
        if (remaining <= 0_i64) {
            noteFailure(
                text::format(
                    "net_limit: proxy downstream bytes {} exceeded limit {}", used,
                    config.downBytesMax
                )
            );
            closed.store(true, std::memory_order_relaxed);
            return 0_uz;
        }

        const auto allowed = std::min(remaining, i64(bytes.size()));
        try {
            const auto sent = sock.SendAll(bytes.data(), numericCast<size_t>(allowed), deadline);
            accountDownBytes(i64{sent});
            return usize{sent};
        } catch (const utils::TracefulException &) {
            return 0_uz;
        }
    }

    [[nodiscard]] bool sendAll(
        engine::io::Socket &sock, std::span<const char> bytes, engine::Deadline deadline
    ) noexcept
    {
        if (bytes.empty())
            return true;

        try {
            static_cast<void>(sock.SendAll(bytes.data(), bytes.size(), deadline));
            return true;
        } catch (const utils::TracefulException &) {
            return false;
        }
    }

    void send407(engine::io::Socket &client, engine::Deadline deadline) noexcept
    {
        static_cast<void>(sendBudgeted(client, kProxyAuthRequiredResponse, deadline));
    }

    void send400(
        engine::io::Socket &client, std::string_view message, engine::Deadline deadline
    ) noexcept
    {
        const auto response = make400Response(message);
        static_cast<void>(sendBudgeted(client, response, deadline));
    }

    void send502(
        engine::io::Socket &client, std::string_view message, engine::Deadline deadline
    ) noexcept
    {
        const auto response = make502Response(message);
        static_cast<void>(sendBudgeted(client, response, deadline));
    }

    void closeSocketsQuietly(engine::io::Socket &a, engine::io::Socket &b) noexcept
    {
        try {
            a.Close();
        } catch (const utils::TracefulException &) {
        }
        try {
            b.Close();
        } catch (const utils::TracefulException &) {
        }
    }

    [[nodiscard]] Expected<engine::io::Socket, String> connectUpstream(
        dns::Resolver &resolver, std::string_view host, u16 port, engine::Deadline deadline
    ) noexcept
    {
        const auto upstream = rewriteLocalFixtureIfNeeded(config, host, port);
        auto addr = resolveTcp(resolver, upstream.connectHost, upstream.connectPort, deadline);
        if (!addr)
            return std::unexpected(std::move(addr).error());

        auto socket = engine::io::Socket{addr->Domain(), engine::io::SocketType::kStream};
        try {
            socket.Connect(addr.value(), deadline);
        } catch (const utils::TracefulException &) {
            return std::unexpected("connect upstream failed"_t);
        }
        return socket;
    }

    void copyClientToUpstream(
        engine::io::Socket &client, engine::io::Socket &upstream, engine::Deadline deadline
    ) noexcept
    {
        std::array<char, kIoBufferBytes> storage{};
        auto buffer = std::span<char>{storage};
        try {
            while (!isClosed()) {
                const auto received = usize{
                    client.RecvSome(buffer.data(), buffer.size(), deadline)
                };
                if (received == 0_uz)
                    return;
                if (!sendAll(
                        upstream, std::span<const char>{buffer.data(), raw(received)}, deadline
                    )) {
                    return;
                }
            }
        } catch (const utils::TracefulException &) {
            return;
        }
    }

    void copyUpstreamToClientBudgeted(
        engine::io::Socket &upstream, engine::io::Socket &client, engine::Deadline deadline
    ) noexcept
    {
        std::array<char, kIoBufferBytes> storage{};
        auto buffer = std::span<char>{storage};
        try {
            while (!isClosed()) {
                const auto received = usize{
                    upstream.RecvSome(buffer.data(), buffer.size(), deadline)
                };
                if (received == 0_uz)
                    return;

                auto pending = std::span<const char>{buffer.data(), raw(received)};
                while (!pending.empty() && !isClosed()) {
                    const auto sent = sendBudgeted(client, pending, deadline);
                    if (sent == 0_uz)
                        return;
                    pending = pending.subspan(raw(sent));
                }
            }
        } catch (const utils::TracefulException &) {
            return;
        }
    }

    void relayConnect(
        engine::io::Socket &client, engine::io::Socket &upstream, engine::Deadline deadline
    ) noexcept
    {
        auto clientToUpstream = engine::AsyncNoSpan([&]() noexcept {
            copyClientToUpstream(client, upstream, deadline);
        });
        auto upstreamToClient = engine::AsyncNoSpan([&]() noexcept {
            copyUpstreamToClientBudgeted(upstream, client, deadline);
        });

        auto waitAny = engine::MakeWaitAny(clientToUpstream, upstreamToClient);
        static_cast<void>(waitAny.WaitUntil(deadline));
        clientToUpstream.RequestCancel();
        upstreamToClient.RequestCancel();
        static_cast<void>(clientToUpstream.WaitNothrow());
        static_cast<void>(upstreamToClient.WaitNothrow());
    }

    void forwardHttpRequestBody(
        engine::io::Socket &client, engine::io::Socket &upstream, std::string_view bufferedBody,
        i64 remainingBody, engine::Deadline deadline
    ) noexcept
    {
        const auto alreadyBuffered = bufferedBody.substr(
            0, numericCast<size_t>(std::max(0_i64, std::min(remainingBody, ssize(bufferedBody))))
        );
        if (!sendAll(upstream, alreadyBuffered, deadline))
            return;
        remainingBody -= ssize(alreadyBuffered);

        std::array<char, kIoBufferBytes> storage{};
        auto buffer = std::span<char>{storage};
        while (remainingBody > 0_i64 && !isClosed()) {
            const auto want = numericCast<size_t>(std::min(remainingBody, ssize(buffer)));
            auto received = 0_uz;
            try {
                received = usize{client.RecvSome(buffer.data(), want, deadline)};
            } catch (const utils::TracefulException &) {
                break;
            }
            if (received == 0_uz)
                break;
            remainingBody -= i64(received);
            if (!sendAll(upstream, std::span<const char>{buffer.data(), raw(received)}, deadline))
                return;
        }
    }

    void handleConnect(
        dns::Resolver &resolver, engine::io::Socket &client, const ParsedRequest &req,
        engine::Deadline deadline
    ) noexcept
    {
        const auto authority = parseAuthority(req.target, 0_u16, PortMode::kRequired);
        if (!authority) {
            send400(client, "invalid CONNECT target", deadline);
            return;
        }

        auto upstream = connectUpstream(resolver, authority->host, authority->port, deadline);
        if (!upstream) {
            send502(client, upstream.error().view(), deadline);
            return;
        }

        auto upstreamSocket = std::move(upstream).value();
        static_cast<void>(sendBudgeted(client, kConnectEstablishedResponse, deadline));
        if (isClosed()) {
            closeSocketsQuietly(client, upstreamSocket);
            return;
        }

        relayConnect(client, upstreamSocket, deadline);
        closeSocketsQuietly(client, upstreamSocket);
    }

    void handleHttp(
        dns::Resolver &resolver, engine::io::Socket &client, const ParsedRequest &req,
        std::string_view headerBytes, engine::Deadline deadline
    ) noexcept
    {
        auto target = parseHttpRequestTarget(req);
        if (!target) {
            send400(client, target.error(), deadline);
            return;
        }

        i64 contentLength = 0_i64;
        if (const auto header = findHeaderValue(req.headers, "content-length")) {
            auto parsedContentLength = parseContentLength(header.value());
            if (!parsedContentLength) {
                send400(client, parsedContentLength.error().view(), deadline);
                return;
            }
            contentLength = parsedContentLength.value();
        }

        auto upstream = connectUpstream(resolver, target->host, target->port, deadline);
        if (!upstream) {
            send502(client, upstream.error().view(), deadline);
            return;
        }

        auto upstreamSocket = std::move(upstream).value();
        const auto request = buildForwardRequest(req, target->path);
        if (!sendAll(upstreamSocket, request, deadline)) {
            send502(client, "send upstream failed", deadline);
            closeSocketsQuietly(client, upstreamSocket);
            return;
        }

        forwardHttpRequestBody(
            client, upstreamSocket, headerBytes.substr(numericCast<size_t>(req.headerBytes)),
            contentLength, deadline
        );
        copyUpstreamToClientBudgeted(upstreamSocket, client, deadline);
        closeSocketsQuietly(client, upstreamSocket);
    }

    void handleClient(
        dns::Resolver &resolver, engine::io::Socket client, engine::Deadline deadline
    ) noexcept
    {
        if (isClosed())
            return;

        std::string header;
        header.reserve(2048);
        std::array<char, kIoBufferBytes> storage{};
        auto buffer = std::span<char>{storage};
        try {
            while (header.find("\r\n\r\n") == std::string::npos) {
                if (usz(header) > kMaxHeaderBytes) {
                    send400(client, "header too large", deadline);
                    client.Close();
                    return;
                }
                const auto received = usize{
                    client.RecvSome(buffer.data(), buffer.size(), deadline)
                };
                if (received == 0_uz)
                    return;
                header.append(buffer.data(), raw(received));
            }
        } catch (const utils::TracefulException &) {
            return;
        }

        const auto parsed = parseHeaderBlock(header);
        if (!parsed) {
            send400(client, "invalid request", deadline);
            return;
        }
        if (usz(parsed->target) > config.urlBytesMax) {
            send400(client, "target too long", deadline);
            return;
        }

        const auto auth = findHeaderValue(parsed->headers, "proxy-authorization");
        const auto user = auth ? parseBasicAuthUser(auth.value())
                               : std::optional<std::pair<std::string, std::string>>{};
        if (!user || user->first != config.runId) {
            send407(client, deadline);
            return;
        }

        if (absl::EqualsIgnoreCase(parsed->method, "CONNECT")) {
            handleConnect(resolver, client, parsed.value(), deadline);
            return;
        }
        handleHttp(resolver, client, parsed.value(), header, deadline);
    }

    void acceptLoop(dns::Resolver &resolver, engine::Deadline deadline) noexcept
    {
        while (!isClosed()) {
            engine::io::Socket client;
            try {
                client = listener.Accept(deadline);
            } catch (const utils::TracefulException &) {
                return;
            }
            if (isClosed())
                return;
            auto task = engine::AsyncNoSpan([this, &resolver, deadline,
                                             sock = std::move(client)]() mutable noexcept {
                handleClient(resolver, std::move(sock), deadline);
            });
            auto lock = clientTasks.Lock();
            lock->push_back(std::move(task));
        }
    }

    void closeAll() noexcept
    {
        closed.store(true, std::memory_order_relaxed);
        if (acceptTask) {
            acceptTask->RequestCancel();
            static_cast<void>(acceptTask->WaitNothrow());
            acceptTask.reset();
        }
        std::vector<engine::TaskWithResult<void>> tasks;
        {
            auto lock = clientTasks.Lock();
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
        } catch (const utils::TracefulException &) {
        }
    }
};

EgressProxy::EgressProxy(EgressProxyConfig config) : impl(std::move(config))
{
    UINVARIANT(!impl->config.socketPath.empty(), "proxy socket path must not be empty");
    UINVARIANT(!impl->config.runId.empty(), "proxy runId must not be empty");
    UINVARIANT(impl->config.urlBytesMax > 0_uz, "proxy urlBytesMax must be positive");
    UINVARIANT(impl->config.downBytesMax > 0_i64, "proxy downBytesMax must be positive");
}

EgressProxy::~EgressProxy() noexcept { close(); }

Expected<void, String> EgressProxy::start(dns::Resolver &resolver, engine::Deadline deadline)
{
    UINVARIANT(deadline.IsReachable(), "proxy start deadline must be reachable");
    if (impl->acceptTask)
        return std::unexpected("proxy already started"_t);

    impl->listener = engine::io::Socket(
        engine::io::AddrDomain::kUnix, engine::io::SocketType::kStream
    );
    try {
        auto addr = engine::io::Sockaddr::MakeUnixSocketAddress(impl->config.socketPath);
        impl->listener.Bind(addr);
        impl->listener.Listen();
    } catch (const utils::TracefulException &e) {
        return std::unexpected(text::format("proxy bind/listen failed: {}", e.what()));
    }

    impl->acceptTask = engine::AsyncNoSpan([this, &resolver, deadline]() noexcept {
        impl->acceptLoop(resolver, deadline);
    });
    return {};
}

void EgressProxy::close() noexcept { impl->closeAll(); }

i64 EgressProxy::downBytes() const noexcept { return i64(impl->downBytes.load()); }

std::optional<String> EgressProxy::failureReason() const noexcept
{
    auto lock = impl->failure.Lock();
    return *lock;
}

} // namespace v1::crawler
