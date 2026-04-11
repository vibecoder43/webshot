#include "crawler/cdp_client.hpp"

#include "grab_value.hpp"
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

#include <format>

#include <absl/strings/ascii.h>
#include <absl/strings/match.h>

#include <userver/crypto/base64.hpp>
#include <userver/crypto/hash.hpp>
#include <userver/crypto/random.hpp>
#include <userver/engine/deadline.hpp>
#include <userver/engine/exception.hpp>
#include <userver/engine/io/sockaddr.hpp>
#include <userver/engine/io/socket.hpp>
#include <userver/engine/sleep.hpp>
#include <userver/formats/json.hpp>
#include <userver/formats/json/value_builder.hpp>
#include <userver/logging/log.hpp>
#include <userver/utils/assert.hpp>
#include <userver/utils/datetime.hpp>
#include <userver/utils/traceful_exception.hpp>
#include <userver/websocket/connection.hpp>

namespace us = userver;
namespace json = us::formats::json;
namespace chrono = std::chrono;

namespace v1::crawler {
using namespace text::literals;
using v1::Expected;

namespace {

constexpr size_t kMaxHandshakeResponseBytes = 16UL * 1024UL;
constexpr std::string_view kWebsocketGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

[[nodiscard]] us::engine::Deadline
pickEarlierReachableDeadline(us::engine::Deadline a, us::engine::Deadline b)
{
    UINVARIANT(a.IsReachable(), "deadline must be reachable");
    UINVARIANT(b.IsReachable(), "deadline must be reachable");
    return a.TimeLeft() <= b.TimeLeft() ? a : b;
}

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

[[nodiscard]] Expected<String, CdpFailure> getErrorMessage(const json::Value &error)
{
    using enum CdpError;
    UINVARIANT(error.IsObject(), "cdp error payload must be object");
    try {
        auto message = String::fromBytes(error.As<dto::CdpError>().message);
        if (!message)
            return std::unexpected(CdpFailure{.code = kProtocol, .detail = {}});
        return std::move(message).value();
    } catch (const json::Exception &e) {
        UINVARIANT(
            false, std::format("cdp error payload does not match dto::CdpError ({})", e.what())
        );
    }
    return std::unexpected(CdpFailure{.code = kProtocol, .detail = {}});
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

[[nodiscard]] Expected<std::string, CdpFailure>
readHandshakeResponse(us::engine::io::Socket &socket, us::engine::Deadline deadline)
{
    using enum CdpError;
    std::string response;
    response.reserve(1024);

    while (response.find("\r\n\r\n") == std::string::npos) {
        if (response.size() >= kMaxHandshakeResponseBytes)
            return std::unexpected(CdpFailure{.code = kHandshakeResponseTooLarge, .detail = {}});

        char ch = '\0';
        const auto bytesRead = socket.RecvSome(&ch, 1, deadline);
        if (bytesRead == 0)
            return std::unexpected(CdpFailure{.code = kHandshakeUnexpectedEof, .detail = {}});
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

[[nodiscard]] Expected<HandshakeResponse, CdpFailure>
parseHandshakeResponse(std::string_view response)
{
    const auto headersEnd = response.find("\r\n\r\n");
    if (headersEnd == std::string::npos)
        return std::unexpected(
            CdpFailure{.code = CdpError::kHandshakeMalformedResponse, .detail = {}}
        );

    HandshakeResponse parsed;
    parsed.rawHeaders = std::string(response.substr(0, headersEnd + 4));

    const auto statusLineEnd = response.find("\r\n");
    if (statusLineEnd == std::string::npos)
        return std::unexpected(
            CdpFailure{.code = CdpError::kHandshakeMalformedResponse, .detail = {}}
        );

    parsed.statusLine = std::string(response.substr(0, statusLineEnd));
    parsed.headers = parseHandshakeHeaders(
        response.substr(statusLineEnd + 2, headersEnd - (statusLineEnd + 2))
    );
    return parsed;
}

Expected<void, CdpFailure>
validateHandshakeResponse(const HandshakeResponse &response, std::string_view secWebsocketKey)
{
    if (response.statusLine.rfind("HTTP/1.1 101 ", 0) != 0 &&
        response.statusLine != "HTTP/1.1 101") {
        return std::unexpected(CdpFailure{.code = CdpError::kHandshakeRejected, .detail = {}});
    }

    const auto upgradeIt = response.headers.find("upgrade");
    if (upgradeIt == std::end(response.headers) ||
        !absl::EqualsIgnoreCase(std::string_view(upgradeIt->second), "websocket")) {
        return std::unexpected(CdpFailure{.code = CdpError::kHandshakeMissingHeader, .detail = {}});
    }

    const auto connectionIt = response.headers.find("connection");
    if (connectionIt == std::end(response.headers) ||
        !containsHeaderToken(connectionIt->second, "upgrade")) {
        return std::unexpected(CdpFailure{.code = CdpError::kHandshakeMissingHeader, .detail = {}});
    }

    const auto acceptIt = response.headers.find("sec-websocket-accept");
    if (acceptIt == std::end(response.headers)) {
        return std::unexpected(CdpFailure{.code = CdpError::kHandshakeMissingHeader, .detail = {}});
    }

    const auto expectedAccept = us::crypto::base64::Base64Encode(
        us::crypto::hash::Sha1(
            {secWebsocketKey, kWebsocketGuid}, us::crypto::hash::OutputEncoding::kBinary
        )
    );
    if (acceptIt->second != expectedAccept)
        return std::unexpected(CdpFailure{.code = CdpError::kHandshakeRejected, .detail = {}});
    return {};
}

} // namespace

CdpClient::CdpClient(
    std::string socketPathIn, String websocketPathIn,
    std::shared_ptr<us::websocket::WebSocketConnection> connectionIn, std::string tracePathIn,
    us::fs::blocking::FileDescriptor traceFileIn, us::engine::Deadline overallDeadlineIn,
    chrono::seconds commandTimeoutIn, chrono::milliseconds waitPollIntervalIn
)
    : socketPath(std::move(socketPathIn)), websocketPath(std::move(websocketPathIn)),
      connection(std::move(connectionIn)), tracePath(std::move(tracePathIn)),
      traceFile(std::move(traceFileIn)), overallDeadline(overallDeadlineIn),
      commandTimeout(commandTimeoutIn), waitPollInterval(waitPollIntervalIn)
{
}

Expected<std::unique_ptr<CdpClient>, CdpFailure> CdpClient::connect(
    std::string socketPath, String websocketPath, std::string tracePath,
    us::engine::Deadline overallDeadline, chrono::seconds handshakeTimeout,
    chrono::seconds commandTimeout, chrono::milliseconds waitPollInterval
)
{
    using enum CdpError;

    UINVARIANT(!tracePath.empty(), "cdp trace path must not be empty");
    UINVARIANT(overallDeadline.IsReachable(), "cdp overall deadline must be reachable");

    us::fs::blocking::FileDescriptor traceFd;
    try {
        traceFd = us::fs::blocking::FileDescriptor::Open(
            tracePath, us::fs::blocking::OpenMode{
                           us::fs::blocking::OpenFlag::kWrite,
                           us::fs::blocking::OpenFlag::kCreateIfNotExists,
                           us::fs::blocking::OpenFlag::kAppend,
                       }
        );
    } catch (const std::runtime_error &) {
        return std::unexpected(CdpFailure{.code = kTraceFileOpenFailed, .detail = {}});
    }

    auto socket = us::engine::io::Socket{
        us::engine::io::AddrDomain::kUnix, us::engine::io::SocketType::kStream
    };
    const auto handshakeDeadline = pickEarlierReachableDeadline(
        overallDeadline, us::engine::Deadline::FromDuration(handshakeTimeout)
    );
    auto address = us::engine::io::Sockaddr::MakeUnixSocketAddress(socketPath);
    try {
        socket.Connect(address, handshakeDeadline);
    } catch (const us::utils::TracefulException &) {
        return std::unexpected(CdpFailure{.code = kSocketConnectFailed, .detail = {}});
    }

    auto randomKey = std::array<char, 16>{};
    us::crypto::GenerateRandomBlock(us::utils::span(randomKey));
    const auto secWebsocketKey = us::crypto::base64::Base64Encode(
        std::string_view(randomKey.data(), randomKey.size())
    );

    const auto request = std::format(
        "GET {} HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: Upgrade\r\n"
        "Upgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: {}\r\n"
        "\r\n",
        websocketPath.empty()
            ? "/"
            : (websocketPath.startsWith('/') ? websocketPath.view()
                                             : ("/" + std::string(websocketPath.view()))),
        secWebsocketKey
    );
    try {
        static_cast<void>(socket.SendAll(request.data(), request.size(), handshakeDeadline));
    } catch (const us::utils::TracefulException &) {
        return std::unexpected(CdpFailure{.code = kTransport, .detail = {}});
    }

    auto response = readHandshakeResponse(socket, handshakeDeadline);
    if (!response)
        return std::unexpected(response.error());
    auto parsedResponse = parseHandshakeResponse(grabValueOf(response));
    if (!parsedResponse)
        return std::unexpected(parsedResponse.error());
    auto validated = validateHandshakeResponse(grabValueOf(parsedResponse), secWebsocketKey);
    if (!validated)
        return std::unexpected(validated.error());

    std::shared_ptr<us::websocket::WebSocketConnection> ws;
    try {
        auto connectionSocket = std::make_unique<us::engine::io::Socket>(std::move(socket));
        ws = us::websocket::MakeClientWebSocketConnection(
            std::move(connectionSocket), std::move(address), {}
        );
    } catch (const us::utils::TracefulException &) {
        return std::unexpected(CdpFailure{.code = kTransport, .detail = {}});
    }

    return std::unique_ptr<CdpClient>(new CdpClient(
        std::move(socketPath), std::move(websocketPath), std::move(ws), std::move(tracePath),
        std::move(traceFd), overallDeadline, commandTimeout, waitPollInterval
    ));
}

CdpClient::~CdpClient() noexcept { closeQuietly(); }

Expected<json::Value, CdpFailure> CdpClient::sendRaw(
    std::string_view method, const json::Value &params, const std::optional<String> &sessionId
)
{
    using enum CdpError;
    if (closed)
        return std::unexpected(CdpFailure{.code = kSocketClosed, .detail = {}});

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
                .method = std::string{method},
                .sessionId = sessionId,
            }
    );
    traceCommand(id, method, sessionId);
    try {
        connection->SendText(json::ToString(json::ValueBuilder(request).ExtractValue()));
    } catch (const us::utils::TracefulException &e) {
        pendingRequests.erase(id);
        traceTransportError("send", e.what());
        return std::unexpected(CdpFailure{.code = kTransport, .detail = {}});
    }

    const auto deadline = pickEarlierReachableDeadline(
        overallDeadline, us::engine::Deadline::FromDuration(commandTimeout)
    );
    auto waited = waitUntil(
        [this, id]() { return pendingResults.contains(id); }, deadline,
        "timed out waiting for cdp response"
    );
    if (!waited) {
        pendingRequests.erase(id);
        return std::unexpected(waited.error());
    }

    auto it = pendingResults.find(id);
    UINVARIANT(
        it != std::end(pendingResults), std::format("missing cdp response for request id {}", id)
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

Expected<bool, CdpFailure> CdpClient::tryPumpOnce()
{
    using enum CdpError;
    if (closed)
        return false;

    us::websocket::Message message;
    try {
        if (!connection->TryRecv(message))
            return false;
    } catch (const us::utils::TracefulException &e) {
        traceTransportError("try_recv", e.what());
        return std::unexpected(CdpFailure{.code = kTransport, .detail = {}});
    }
    if (message.close_status) {
        closed = true;
        traceClose("in", numericCast<int>(message.close_status.value()));
        return std::unexpected(CdpFailure{.code = kSocketClosed, .detail = {}});
    }
    auto ok = handleMessage(message.data);
    if (!ok)
        return std::unexpected(ok.error());
    return true;
}

Expected<void, CdpFailure> CdpClient::close()
{
    using enum CdpError;
    if (closed)
        return {};
    traceClose("out", numericCast<int>(us::websocket::CloseStatus::kNormal));
    closed = true;
    try {
        if (connection)
            connection->Close(us::websocket::CloseStatus::kNormal);
    } catch (const us::utils::TracefulException &e) {
        traceTransportError("close", e.what());
        connection.reset();
        return std::unexpected(CdpFailure{.code = kTransport, .detail = {}});
    }
    connection.reset();
    return {};
}

Expected<void, CdpFailure> CdpClient::pumpOne()
{
    using enum CdpError;
    us::websocket::Message message;
    try {
        connection->Recv(message);
    } catch (const us::utils::TracefulException &e) {
        traceTransportError("recv", e.what());
        return std::unexpected(CdpFailure{.code = kTransport, .detail = {}});
    }
    if (message.close_status) {
        closed = true;
        traceClose("in", numericCast<int>(message.close_status.value()));
        return std::unexpected(CdpFailure{.code = kSocketClosed, .detail = {}});
    }
    return handleMessage(message.data);
}

void CdpClient::closeQuietly() noexcept
{
    closed = true;
    connection.reset();
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

Expected<void, CdpFailure> CdpClient::handleMessage(const std::string &payload)
{
    using enum CdpError;
    json::Value value;
    try {
        value = json::FromString(payload);
    } catch (const json::Exception &) {
        return std::unexpected(CdpFailure{.code = kJsonParseFailed, .detail = {}});
    }
    const auto idValue = value["id"];
    if (!idValue.IsMissing()) {
        const auto id = idValue.As<int64_t>();
        const auto requestIt = pendingRequests.find(id);
        UINVARIANT(
            requestIt != std::end(pendingRequests),
            std::format("cdp response for unknown request id {}", id)
        );
        const auto *request = &requestIt->second;
        const auto errorValue = value["error"];
        if (!errorValue.IsMissing()) {
            auto errorMessage = getErrorMessage(errorValue);
            if (!errorMessage)
                return std::unexpected(errorMessage.error());
            traceResponse(id, request, errorMessage.value());
            pendingRequests.erase(requestIt);
            return std::unexpected(
                CdpFailure{.code = kCommandFailed, .detail = errorMessage.value()}
            );
        }
        traceResponse(id, request, {});
        pendingResults.emplace(id, value["result"]);
        pendingRequests.erase(requestIt);
        return {};
    }

    UINVARIANT(!value["method"].IsMissing(), "cdp message missing id and method");

    dto::CdpEventMessage eventMessage;
    try {
        eventMessage = value.As<dto::CdpEventMessage>();
    } catch (const json::Exception &) {
        return std::unexpected(CdpFailure{.code = kProtocol, .detail = {}});
    }
    const auto sessionId = eventMessage.sessionId.transform([](const auto &s) {
        return String::fromBytes(s).expect();
    });
    traceEvent(eventMessage.method, sessionId);
    dispatchEvent(
        CdpEvent{
            .method = String::fromBytes(eventMessage.method).expect(),
            .params = std::move(eventMessage.params),
            .sessionId = sessionId,
        }
    );
    return {};
}

std::string CdpClient::makeEndpointPath() const
{
    if (websocketPath.empty())
        return "/";
    const auto path = websocketPath.view();
    if (websocketPath.startsWith('/'))
        return std::string{path};
    return "/" + std::string{path};
}

Expected<void, CdpFailure> CdpClient::writeTraceLine(const json::Value &value)
{
    using enum CdpError;
    auto line = json::ToString(value);
    line.push_back('\n');
    try {
        traceFile.Write(line);
    } catch (const std::runtime_error &e) {
        static_cast<void>(e);
        return std::unexpected(CdpFailure{.code = kTraceWriteFailed, .detail = {}});
    }
    return {};
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
    writeTraceLine(entry.ExtractValue()).expect();
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
    writeTraceLine(entry.ExtractValue()).expect();
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
    writeTraceLine(entry.ExtractValue()).expect();
}

void CdpClient::traceClose(std::string_view direction, int closeCode)
{
    json::ValueBuilder entry;
    entry["ts"] = currentTraceTimestamp();
    entry["direction"] = std::string(direction);
    entry["kind"] = "close";
    entry["closeCode"] = closeCode;
    writeTraceLine(entry.ExtractValue()).expect();
}

void CdpClient::traceTransportError(std::string_view operation, std::string_view error)
{
    json::ValueBuilder entry;
    entry["ts"] = currentTraceTimestamp();
    entry["kind"] = "transport_error";
    entry["operation"] = std::string(operation);
    entry["error"] = std::string(error);
    writeTraceLine(entry.ExtractValue()).expect();
}

} // namespace v1::crawler
