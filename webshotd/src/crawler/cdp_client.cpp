#include "crawler/cdp_client.hpp"

#include "schema/cdp.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <absl/strings/ascii.h>
#include <absl/strings/match.h>

#include <userver/crypto/base64.hpp>
#include <userver/crypto/hash.hpp>
#include <userver/crypto/random.hpp>
#include <userver/engine/deadline.hpp>
#include <userver/engine/io/sockaddr.hpp>
#include <userver/engine/io/socket.hpp>
#include <userver/engine/sleep.hpp>
#include <userver/formats/json.hpp>
#include <userver/formats/json/value_builder.hpp>
#include <userver/logging/log.hpp>
#include <userver/utils/assert.hpp>
#include <userver/utils/datetime.hpp>
#include <userver/websocket/connection.hpp>

namespace us = userver;
namespace json = us::formats::json;
namespace chrono = std::chrono;

namespace v1::crawler {
using namespace text::literals;

namespace {

constexpr auto kHandshakeTimeout = chrono::seconds(5);
constexpr size_t kMaxHandshakeResponseBytes = 16UL * 1024UL;
constexpr size_t kMaxHandshakeDumpBytes = 1024UL;
constexpr std::string_view kWebsocketGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

[[nodiscard]] std::string currentTraceTimestamp()
{
    return us::utils::datetime::UtcTimestring(
        us::utils::datetime::Now(), us::utils::datetime::kRfc3339Format
    );
}

struct HandshakeResponse final {
    std::string statusLine;
    std::unordered_map<std::string, std::string> headers;
    std::string rawHeaders;
};

[[nodiscard]] String getErrorMessage(const json::Value &error)
{
    UINVARIANT(error.IsObject(), "cdp error payload must be object");
    try {
        return String::fromBytesThrow(error.As<dto::CdpError>().message);
    } catch (const std::exception &e) {
        UINVARIANT(
            false, fmt::format("cdp error payload does not match dto::CdpError ({})", e.what())
        );
    }
    return "cdp command failed"_t;
}

[[nodiscard]] bool containsHeaderToken(std::string_view value, std::string_view token)
{
    size_t start = 0;
    while (start <= value.size()) {
        const auto commaPos = value.find(',', start);
        const auto part = commaPos == std::string_view::npos
                              ? value.substr(start)
                              : value.substr(start, commaPos - start);
        const auto trimmedPart = absl::StripAsciiWhitespace(part);
        if (absl::EqualsIgnoreCase(trimmedPart, token))
            return true;
        if (commaPos == std::string_view::npos)
            break;
        start = commaPos + 1;
    }

    return false;
}

[[nodiscard]] std::string
readHandshakeResponse(us::engine::io::Socket &socket, us::engine::Deadline deadline)
{
    std::string response;
    response.reserve(1024);

    while (response.find("\r\n\r\n") == std::string::npos) {
        if (response.size() >= kMaxHandshakeResponseBytes)
            throw std::runtime_error("websocket handshake response headers are too large");

        char ch = '\0';
        const auto bytesRead = socket.RecvSome(&ch, 1, deadline);
        if (bytesRead == 0)
            throw std::runtime_error("websocket handshake response ended before headers completed");
        response.push_back(ch);
    }

    return response;
}

[[nodiscard]] std::unordered_map<std::string, std::string>
parseHandshakeHeaders(std::string_view headersBlock)
{
    std::unordered_map<std::string, std::string> headers;

    size_t lineStart = 0;
    while (lineStart < headersBlock.size()) {
        const auto lineEnd = headersBlock.find("\r\n", lineStart);
        const auto line = lineEnd == std::string::npos
                              ? headersBlock.substr(lineStart)
                              : headersBlock.substr(lineStart, lineEnd - lineStart);
        if (line.empty())
            break;
        const auto colonPos = line.find(':');
        if (colonPos != std::string::npos) {
            const auto trimmed = absl::StripAsciiWhitespace(line.substr(colonPos + 1));
            headers.emplace(absl::AsciiStrToLower(line.substr(0, colonPos)), std::string(trimmed));
        }

        if (lineEnd == std::string::npos)
            break;
        lineStart = lineEnd + 2;
    }

    return headers;
}

[[nodiscard]] std::string makeHeaderDump(std::string_view rawHeaders)
{
    std::string dump;
    dump.reserve(std::min(rawHeaders.size(), kMaxHandshakeDumpBytes) + 16);

    for (const auto ch : rawHeaders) {
        if (dump.size() >= kMaxHandshakeDumpBytes)
            break;

        if (ch == '\r') {
            dump += "\\r";
        } else if (ch == '\n') {
            dump += "\\n";
        } else if (std::isprint(static_cast<unsigned char>(ch))) {
            dump.push_back(ch);
        } else {
            dump += fmt::format("\\x{:02x}", static_cast<unsigned char>(ch));
        }
    }

    if (rawHeaders.size() > kMaxHandshakeDumpBytes)
        dump += "...";

    return dump;
}

[[nodiscard]] std::string
makeHeaderSummary(const std::unordered_map<std::string, std::string> &headers)
{
    std::vector<std::pair<std::string, std::string>> items;
    items.reserve(headers.size());
    for (const auto &[name, value] : headers)
        items.emplace_back(name, value);

    std::sort(std::begin(items), std::end(items), [](const auto &lhs, const auto &rhs) {
        return lhs.first < rhs.first;
    });

    std::string summary;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i != 0)
            summary += ", ";
        summary += items[i].first;
        summary += '=';
        summary += items[i].second;
    }

    if (summary.empty())
        return "empty";
    return summary;
}

[[nodiscard]] std::string describeHandshakeResponse(const HandshakeResponse &response)
{
    return fmt::format(
        "status={}, headers={}, raw_headers={}",
        response.statusLine.empty() ? "missing" : response.statusLine,
        makeHeaderSummary(response.headers), makeHeaderDump(response.rawHeaders)
    );
}

[[nodiscard]] HandshakeResponse parseHandshakeResponse(std::string_view response)
{
    const auto headersEnd = response.find("\r\n\r\n");
    UINVARIANT(
        headersEnd != std::string::npos, "websocket handshake response missing header terminator"
    );

    HandshakeResponse parsed;
    parsed.rawHeaders = std::string(response.substr(0, headersEnd + 4));

    const auto statusLineEnd = response.find("\r\n");
    if (statusLineEnd == std::string::npos)
        throw std::runtime_error(
            fmt::format(
                "invalid websocket handshake response: {}", describeHandshakeResponse(parsed)
            )
        );

    parsed.statusLine = std::string(response.substr(0, statusLineEnd));
    parsed.headers = parseHandshakeHeaders(
        response.substr(statusLineEnd + 2, headersEnd - (statusLineEnd + 2))
    );
    return parsed;
}

void validateHandshakeResponse(const HandshakeResponse &response, std::string_view secWebsocketKey)
{
    if (response.statusLine.rfind("HTTP/1.1 101 ", 0) != 0 &&
        response.statusLine != "HTTP/1.1 101") {
        throw std::runtime_error(
            fmt::format(
                "websocket upgrade rejected: {} ({})", response.statusLine,
                describeHandshakeResponse(response)
            )
        );
    }

    const auto upgradeIt = response.headers.find("upgrade");
    if (upgradeIt == std::end(response.headers) ||
        !absl::EqualsIgnoreCase(std::string_view(upgradeIt->second), "websocket")) {
        throw std::runtime_error(
            fmt::format(
                "websocket handshake missing header: upgrade ({})",
                describeHandshakeResponse(response)
            )
        );
    }

    const auto connectionIt = response.headers.find("connection");
    if (connectionIt == std::end(response.headers) ||
        !containsHeaderToken(connectionIt->second, "upgrade")) {
        throw std::runtime_error(
            fmt::format(
                "websocket handshake missing header: connection=upgrade ({})",
                describeHandshakeResponse(response)
            )
        );
    }

    const auto acceptIt = response.headers.find("sec-websocket-accept");
    if (acceptIt == std::end(response.headers)) {
        throw std::runtime_error(
            fmt::format(
                "websocket handshake missing header: sec-websocket-accept ({})",
                describeHandshakeResponse(response)
            )
        );
    }

    const auto expectedAccept = us::crypto::base64::Base64Encode(
        us::crypto::hash::Sha1(
            {secWebsocketKey, kWebsocketGuid}, us::crypto::hash::OutputEncoding::kBinary
        )
    );
    if (acceptIt->second != expectedAccept)
        throw std::runtime_error(
            fmt::format(
                "websocket handshake accept mismatch: got={}, expected={} ({})", acceptIt->second,
                expectedAccept, describeHandshakeResponse(response)
            )
        );
}

} // namespace

CdpClient::CdpClient(std::string socketPathIn, String websocketPathIn, std::string tracePathIn)
    : socketPath(std::move(socketPathIn)), websocketPath(std::move(websocketPathIn)),
      tracePath(std::move(tracePathIn))
{
    auto socket = us::engine::io::Socket{
        us::engine::io::AddrDomain::kUnix, us::engine::io::SocketType::kStream
    };
    const auto deadline = us::engine::Deadline::FromDuration(kHandshakeTimeout);
    auto address = us::engine::io::Sockaddr::MakeUnixSocketAddress(socketPath);
    socket.Connect(address, deadline);

    auto randomKey = std::array<char, 16>{};
    us::crypto::GenerateRandomBlock(us::utils::span(randomKey));
    const auto secWebsocketKey = us::crypto::base64::Base64Encode(
        std::string_view(randomKey.data(), randomKey.size())
    );

    const auto request = fmt::format(
        "GET {} HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: Upgrade\r\n"
        "Upgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: {}\r\n"
        "\r\n",
        makeEndpointPath(), secWebsocketKey
    );
    static_cast<void>(socket.SendAll(request.data(), request.size(), deadline));

    const auto response = readHandshakeResponse(socket, deadline);
    const auto parsedResponse = parseHandshakeResponse(response);
    validateHandshakeResponse(parsedResponse, secWebsocketKey);

    auto connectionSocket = std::make_unique<us::engine::io::Socket>(std::move(socket));
    connection = us::websocket::MakeClientWebSocketConnection(
        std::move(connectionSocket), std::move(address), {}
    );

    UINVARIANT(!tracePath.empty(), "cdp trace path must not be empty");
    try {
        traceFile = us::fs::blocking::FileDescriptor::Open(
            tracePath, us::fs::blocking::OpenMode{
                           us::fs::blocking::OpenFlag::kWrite,
                           us::fs::blocking::OpenFlag::kCreateIfNotExists,
                           us::fs::blocking::OpenFlag::kAppend,
                       }
        );
    } catch (const std::exception &e) {
        throw std::runtime_error(
            fmt::format("failed to open cdp trace file: {} ({})", tracePath, e.what())
        );
    }
}

CdpClient::~CdpClient() noexcept { closeNoThrow(); }

json::Value CdpClient::sendRaw(
    std::string_view method, const json::Value &params, const std::optional<String> &sessionId
)
{
    if (closed)
        throw std::runtime_error("cdp socket is closed");

    const auto id = nextRequestId++;
    dto::CdpCommandRequest request;
    request.id = id;
    request.method = std::string(method);
    if (!params.IsMissing())
        request.params = dto::CdpCommandRequest::Params{params};
    if (sessionId)
        request.sessionId = std::string(sessionId->view());
    pendingRequests.emplace(
        id, PendingRequestTrace{
                std::string(method),
                sessionId ? std::make_optional(String::fromBytesThrow(sessionId->view()))
                          : std::optional<String>{},
            }
    );
    traceCommand(id, method, sessionId);
    try {
        connection->SendText(json::ToString(json::ValueBuilder(request).ExtractValue()));
    } catch (const std::exception &e) {
        pendingRequests.erase(id);
        traceTransportError("send", e.what());
        throw;
    }

    try {
        waitUntil(
            [this, id]() { return pendingResults.contains(id); },
            us::engine::Deadline::FromDuration(chrono::seconds(10)),
            "timed out waiting for cdp response"
        );
    } catch (const std::exception &e) {
        if (pendingRequests.contains(id))
            traceTransportError("wait", e.what());
        pendingRequests.erase(id);
        throw;
    }

    auto it = pendingResults.find(id);
    UINVARIANT(
        it != std::end(pendingResults), fmt::format("missing cdp response for request id {}", id)
    );
    auto result = it->second;
    pendingResults.erase(it);
    return result;
}

CdpClient::ListenerId CdpClient::addListener(EventListener listener)
{
    const auto id = nextListenerId++;
    listeners.emplace(id, std::move(listener));
    return id;
}

void CdpClient::removeListener(ListenerId id) { listeners.erase(id); }

bool CdpClient::tryPumpOnce()
{
    if (closed)
        return false;

    us::websocket::Message message;
    try {
        if (!connection->TryRecv(message))
            return false;
    } catch (const std::exception &e) {
        traceTransportError("try_recv", e.what());
        throw;
    }
    if (message.close_status) {
        closed = true;
        traceClose("in", numericCast<int>(message.close_status.value()));
        throw std::runtime_error(
            fmt::format("cdp socket closed ({})", numericCast<int>(message.close_status.value()))
        );
    }
    handleMessage(message.data);
    return true;
}

void CdpClient::waitUntil(
    const std::function<bool()> &predicate, us::engine::Deadline deadline,
    std::string_view timeoutMessage
)
{
    while (!predicate()) {
        if (tryPumpOnce())
            continue;
        if (deadline.IsReachable() && deadline.IsReached())
            throw std::runtime_error(std::string(timeoutMessage));
        us::engine::SleepFor(std::chrono::milliseconds(10));
    }
}

void CdpClient::close()
{
    if (closed)
        return;
    traceClose("out", numericCast<int>(us::websocket::CloseStatus::kNormal));
    closed = true;
    try {
        if (connection)
            connection->Close(us::websocket::CloseStatus::kNormal);
    } catch (const std::exception &e) {
        traceTransportError("close", e.what());
        connection.reset();
        throw;
    }
    connection.reset();
}

void CdpClient::pumpOne()
{
    us::websocket::Message message;
    try {
        connection->Recv(message);
    } catch (const std::exception &e) {
        traceTransportError("recv", e.what());
        throw;
    }
    if (message.close_status) {
        closed = true;
        traceClose("in", numericCast<int>(message.close_status.value()));
        throw std::runtime_error(
            fmt::format("cdp socket closed ({})", numericCast<int>(message.close_status.value()))
        );
    }
    handleMessage(message.data);
}

void CdpClient::closeNoThrow() noexcept
{
    try {
        close();
    } catch (const std::exception &e) {
        closed = true;
        connection.reset();
        LOG_WARNING() << "Suppressing CDP cleanup failure after disconnect: " << e.what();
    }
}

void CdpClient::dispatchEvent(CdpEvent event)
{
    std::vector<EventListener> listenerSnapshot;
    listenerSnapshot.reserve(listeners.size());
    for (const auto &[id, listener] : listeners) {
        static_cast<void>(id);
        listenerSnapshot.push_back(listener);
    }

    for (const auto &listener : listenerSnapshot)
        listener(event);
}

void CdpClient::handleMessage(const std::string &payload)
{
    const auto value = json::FromString(payload);
    const auto idValue = value["id"];
    if (!idValue.IsMissing()) {
        const auto id = idValue.As<int64_t>();
        const auto requestIt = pendingRequests.find(id);
        UINVARIANT(
            requestIt != std::end(pendingRequests),
            fmt::format("cdp response for unknown request id {}", id)
        );
        const auto *request = &requestIt->second;
        const auto errorValue = value["error"];
        if (!errorValue.IsMissing()) {
            const auto errorMessage = getErrorMessage(errorValue);
            traceResponse(id, request, errorMessage);
            pendingRequests.erase(requestIt);
            throw std::runtime_error(std::string(errorMessage.view()));
        }
        traceResponse(id, request, {});
        pendingResults.emplace(id, value["result"]);
        pendingRequests.erase(requestIt);
        return;
    }

    UINVARIANT(!value["method"].IsMissing(), "cdp message missing id and method");

    auto eventMessage = value.As<dto::CdpEventMessage>();
    traceEvent(
        eventMessage.method,
        eventMessage.sessionId
            ? std::make_optional(String::fromBytesThrow(eventMessage.sessionId.value()))
            : std::optional<String>{}
    );
    dispatchEvent(
        CdpEvent{
            String::fromBytesThrow(eventMessage.method),
            std::move(eventMessage.params),
            eventMessage.sessionId
                ? std::make_optional(String::fromBytesThrow(eventMessage.sessionId.value()))
                : std::optional<String>{},
        }
    );
}

std::string CdpClient::makeEndpointPath() const
{
    if (websocketPath.empty())
        return "/";
    const auto path = websocketPath.view();
    if (websocketPath.startsWith('/'))
        return std::string(path);
    return "/" + std::string(path);
}

void CdpClient::writeTraceLine(const json::Value &value)
{
    auto line = json::ToString(value);
    line.push_back('\n');
    try {
        traceFile.Write(line);
    } catch (const std::exception &e) {
        throw std::runtime_error(
            fmt::format("failed to write cdp trace to {} ({})", tracePath, e.what())
        );
    }
}

void CdpClient::traceCommand(
    int64_t id, std::string_view method, const std::optional<String> &sessionId
)
{
    json::ValueBuilder entry;
    entry["ts"] = currentTraceTimestamp();
    entry["direction"] = "out";
    entry["kind"] = "command";
    entry["id"] = id;
    entry["method"] = std::string(method);
    if (sessionId)
        entry["sessionId"] = std::string(sessionId->view());
    writeTraceLine(entry.ExtractValue());
}

void CdpClient::traceResponse(
    int64_t id, const PendingRequestTrace *request, const std::optional<String> &error
)
{
    json::ValueBuilder entry;
    entry["ts"] = currentTraceTimestamp();
    entry["direction"] = "in";
    entry["kind"] = error ? "error" : "response";
    entry["id"] = id;
    if (request) {
        entry["method"] = request->method;
        if (request->sessionId)
            entry["sessionId"] = std::string(request->sessionId->view());
    }
    if (error)
        entry["error"] = std::string(error->view());
    writeTraceLine(entry.ExtractValue());
}

void CdpClient::traceEvent(std::string_view method, const std::optional<String> &sessionId)
{
    json::ValueBuilder entry;
    entry["ts"] = currentTraceTimestamp();
    entry["direction"] = "in";
    entry["kind"] = "event";
    entry["method"] = std::string(method);
    if (sessionId)
        entry["sessionId"] = std::string(sessionId->view());
    writeTraceLine(entry.ExtractValue());
}

void CdpClient::traceClose(std::string_view direction, int closeCode)
{
    json::ValueBuilder entry;
    entry["ts"] = currentTraceTimestamp();
    entry["direction"] = std::string(direction);
    entry["kind"] = "close";
    entry["closeCode"] = closeCode;
    writeTraceLine(entry.ExtractValue());
}

void CdpClient::traceTransportError(std::string_view operation, std::string_view error)
{
    json::ValueBuilder entry;
    entry["ts"] = currentTraceTimestamp();
    entry["kind"] = "transport_error";
    entry["operation"] = std::string(operation);
    entry["error"] = std::string(error);
    writeTraceLine(entry.ExtractValue());
}

} // namespace v1::crawler
