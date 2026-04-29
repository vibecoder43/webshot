#include "crawler/cdp_client.hpp"

#include "deadline_utils.hpp"
#include "grab_value.hpp"
#include "invariant.hpp"
#include "json.hpp"
#include "schema/cdp.hpp"
#include "try.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <exception>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <string>
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
#include <userver/engine/async.hpp>
#include <userver/engine/deadline.hpp>
#include <userver/engine/io/sockaddr.hpp>
#include <userver/engine/io/socket.hpp>
#include <userver/engine/task/cancel.hpp>
#include <userver/formats/json.hpp>
#include <userver/formats/json/value_builder.hpp>
#include <userver/logging/log.hpp>
#include <userver/utils/assert.hpp>
#include <userver/utils/datetime.hpp>
#include <userver/utils/underlying_value.hpp>
#include <userver/websocket/connection.hpp>
namespace chrono = std::chrono;

namespace v1::crawler {
using namespace text::literals;
using text::toBytes;
using v1::Expected;

namespace {

constexpr auto kMaxHandshakeResponseBytes = 16_i64 * 1024_i64;
constexpr std::string_view kWebsocketGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

[[nodiscard]] bool isCancelRequested() { return eng::current_task::IsCancelRequested(); }

[[nodiscard]] std::string currentTraceTimestamp()
{
    return datetime::UtcTimestring(datetime::Now(), datetime::kRfc3339Format);
}

struct HandshakeResponse final {
    std::string statusLine;
    std::unordered_map<std::string, std::string> headers;
    std::string rawHeaders;
};

[[nodiscard]] Expected<us::fs::blocking::FileDescriptor, CdpFailure>
openTraceFile(eng::TaskProcessor &fsTaskProcessor, const std::string &tracePath)
{
    using enum CdpError;
    try {
        return eng::AsyncNoSpan(
                   fsTaskProcessor, [&tracePath] {
                       return us::fs::blocking::FileDescriptor::Open(
                           tracePath, us::fs::blocking::OpenMode{
                                          us::fs::blocking::OpenFlag::kWrite,
                                          us::fs::blocking::OpenFlag::kCreateIfNotExists,
                                          us::fs::blocking::OpenFlag::kAppend,
                                      }
                       );
                   }
        ).Get();
    } catch (const std::runtime_error &) {
        return Unex(CdpFailure{.code = kTraceFileOpenFailed, .detail = {}});
    }
}

[[nodiscard]] Expected<void, CdpFailure> writeTraceBytes(
    eng::TaskProcessor &fsTaskProcessor, us::fs::blocking::FileDescriptor &traceFile,
    const std::string &line
)
{
    using enum CdpError;
    try {
        eng::AsyncNoSpan(fsTaskProcessor, [&traceFile, &line] { traceFile.Write(line); }).Get();
        return {};
    } catch (const std::runtime_error &) {
        return Unex(CdpFailure{.code = kTraceWriteFailed, .detail = {}});
    }
}

[[nodiscard]] Expected<void, CdpFailure>
connectCdpSocket(eng::io::Socket &socket, const eng::io::Sockaddr &address, eng::Deadline deadline)
{
    using enum CdpError;
    try {
        socket.Connect(address, deadline);
        return {};
    } catch (const std::exception &) {
        if (isCancelRequested())
            throw;
        return Unex(CdpFailure{.code = kSocketConnectFailed, .detail = {}});
    }
}

[[nodiscard]] Expected<void, CdpFailure>
sendHandshakeRequest(eng::io::Socket &socket, const std::string &request, eng::Deadline deadline)
{
    using enum CdpError;
    try {
        static_cast<void>(socket.SendAll(request.data(), request.size(), deadline));
        return {};
    } catch (const std::exception &) {
        if (isCancelRequested())
            throw;
        return Unex(CdpFailure{.code = kTransport, .detail = {}});
    }
}

[[nodiscard]] Expected<size_t, CdpFailure>
readHandshakeByte(eng::io::Socket &socket, char &ch, eng::Deadline deadline)
{
    using enum CdpError;
    try {
        return socket.RecvSome(&ch, 1, deadline);
    } catch (const std::exception &) {
        if (isCancelRequested())
            throw;
        return Unex(CdpFailure{.code = kTransport, .detail = {}});
    }
}

[[nodiscard]] Expected<std::shared_ptr<us::websocket::WebSocketConnection>, CdpFailure>
makeCdpWebSocketConnection(
    eng::io::Socket socket, eng::io::Sockaddr address, i64 maxRemotePayloadBytes
)
{
    using enum CdpError;
    try {
        auto connectionSocket = std::make_unique<eng::io::Socket>(std::move(socket));
        us::websocket::Config wsConfig;
        wsConfig.max_remote_payload = numericCast<decltype(wsConfig.max_remote_payload)>(
            maxRemotePayloadBytes
        );
        return us::websocket::MakeClientWebSocketConnection(
            std::move(connectionSocket), std::move(address), wsConfig
        );
    } catch (const std::exception &) {
        if (isCancelRequested())
            throw;
        return Unex(CdpFailure{.code = kTransport, .detail = {}});
    }
}

[[nodiscard]] Expected<void, std::string>
sendWebSocketText(us::websocket::WebSocketConnection &connection, const std::string &requestBytes)
{
    try {
        connection.SendText(requestBytes);
        return {};
    } catch (const std::exception &e) {
        if (isCancelRequested())
            throw;
        return Unex(std::string(e.what()));
    }
}

[[nodiscard]] Expected<void, std::string>
closeWebSocket(us::websocket::WebSocketConnection &connection)
{
    try {
        connection.Close(us::websocket::CloseStatus::kNormal);
        return {};
    } catch (const std::exception &e) {
        if (isCancelRequested())
            throw;
        return Unex(std::string(e.what()));
    }
}

[[nodiscard]] Expected<String, CdpFailure> getErrorMessage(const json::Value &error)
{
    using enum CdpError;
    invariant(error.IsObject(), "cdp error payload must be object"_t);
    const auto parsed = TRY(
        exu::json::as<dto::CdpError, CdpFailure>(error, [](const json::Exception &e) -> CdpFailure {
            invariant(
                text::format("cdp error payload does not match dto::CdpError ({})", e.what())
            );
        })
    );
    auto message = String::fromBytes(parsed.message);
    if (!message)
        return Unex(CdpFailure{.code = kProtocol, .detail = {}});
    return std::move(*message);
}

[[nodiscard]] bool containsHeaderToken(std::string_view value, std::string_view token)
{
    auto remaining = value;
    while (true) {
        const auto commaPos = remaining.find(',');
        const auto part = commaPos == std::string_view::npos ? remaining
                                                             : remaining.substr(0, commaPos);
        const auto trimmedPart = absl::StripAsciiWhitespace(part);
        if (absl::EqualsIgnoreCase(trimmedPart, token))
            return true;
        if (commaPos == std::string_view::npos)
            break;
        remaining.remove_prefix(commaPos + 1);
    }

    return false;
}

[[nodiscard]] Expected<std::string, CdpFailure>
readHandshakeResponse(eng::io::Socket &socket, eng::Deadline deadline)
{
    using enum CdpError;
    std::string response;
    response.reserve(1024);

    while (!response.contains("\r\n\r\n")) {
        if (ssize(response) >= kMaxHandshakeResponseBytes)
            return Unex(CdpFailure{.code = kHandshakeResponseTooLarge, .detail = {}});

        char ch = '\0';
        const auto bytesRead = TRY(readHandshakeByte(socket, ch, deadline));
        if (bytesRead == 0)
            return Unex(CdpFailure{.code = kHandshakeUnexpectedEof, .detail = {}});
        response.push_back(ch);
    }

    return response;
}

[[nodiscard]] std::unordered_map<std::string, std::string>
parseHandshakeHeaders(std::string_view headersBlock)
{
    std::unordered_map<std::string, std::string> headers;

    auto remaining = headersBlock;
    while (!remaining.empty()) {
        const auto lineEnd = remaining.find("\r\n");
        const auto line = lineEnd == std::string::npos ? remaining : remaining.substr(0, lineEnd);
        if (line.empty())
            break;
        const auto colonPos = line.find(':');
        if (colonPos != std::string::npos) {
            const auto trimmed = absl::StripAsciiWhitespace(line.substr(colonPos + 1));
            headers.emplace(absl::AsciiStrToLower(line.substr(0, colonPos)), std::string(trimmed));
        }

        if (lineEnd == std::string::npos)
            break;
        remaining.remove_prefix(lineEnd + 2);
    }

    return headers;
}

[[nodiscard]] Expected<HandshakeResponse, CdpFailure>
parseHandshakeResponse(std::string_view response)
{
    const auto headersEnd = response.find("\r\n\r\n");
    if (headersEnd == std::string::npos)
        return Unex(CdpFailure{.code = CdpError::kHandshakeMalformedResponse, .detail = {}});

    HandshakeResponse parsed;
    parsed.rawHeaders = std::string(response.substr(0, headersEnd + 4));

    const auto statusLineEnd = response.find("\r\n");
    if (statusLineEnd == std::string::npos)
        return Unex(CdpFailure{.code = CdpError::kHandshakeMalformedResponse, .detail = {}});

    parsed.statusLine = std::string(response.substr(0, statusLineEnd));
    parsed.headers = parseHandshakeHeaders(
        response.substr(statusLineEnd + 2, headersEnd - (statusLineEnd + 2))
    );
    return parsed;
}

Expected<void, CdpFailure>
validateHandshakeResponse(const HandshakeResponse &response, std::string_view secWebsocketKey)
{
    if (!response.statusLine.starts_with("HTTP/1.1 101 ") &&
        response.statusLine != "HTTP/1.1 101") {
        return Unex(CdpFailure{.code = CdpError::kHandshakeRejected, .detail = {}});
    }

    const auto upgradeIt = response.headers.find("upgrade");
    if (upgradeIt == std::end(response.headers) ||
        !absl::EqualsIgnoreCase(std::string_view{upgradeIt->second}, "websocket")) {
        return Unex(CdpFailure{.code = CdpError::kHandshakeMissingHeader, .detail = {}});
    }

    const auto connectionIt = response.headers.find("connection");
    if (connectionIt == std::end(response.headers) ||
        !containsHeaderToken(connectionIt->second, "upgrade")) {
        return Unex(CdpFailure{.code = CdpError::kHandshakeMissingHeader, .detail = {}});
    }

    const auto acceptIt = response.headers.find("sec-websocket-accept");
    if (acceptIt == std::end(response.headers)) {
        return Unex(CdpFailure{.code = CdpError::kHandshakeMissingHeader, .detail = {}});
    }

    const auto expectedAccept = us::crypto::base64::Base64Encode(
        us::crypto::hash::Sha1(
            {secWebsocketKey, kWebsocketGuid}, us::crypto::hash::OutputEncoding::kBinary
        )
    );
    if (acceptIt->second != expectedAccept)
        return Unex(CdpFailure{.code = CdpError::kHandshakeRejected, .detail = {}});
    return {};
}

} // namespace

String describeCdpFailure(const String &action, const CdpFailure &failure)
{
    auto message = action;
    if (failure.detail)
        message = text::format("{}: {}", message, *failure.detail);
    return message;
}

struct CdpSessionState final {
    struct Data final {
        eng::ConditionVariable cv;
        std::deque<CdpEvent> events;
        std::optional<CdpFailure> failure;
        bool closed{false};
    };

    us::concurrent::Variable<Data> data;
};

struct CdpClient::PendingCommandWaiter final {
    struct Data final {
        eng::ConditionVariable cv;
        std::optional<json::Value> result;
        std::optional<CdpFailure> failure;
        bool done{false};
    };

    us::concurrent::Variable<Data> data;
};

namespace {

[[nodiscard]] String parsePrintableText(std::string_view value)
{
    return String::fromBytes(value).expect();
}

[[nodiscard]] CdpFailure makeTimeoutFailure(const String &timeoutMessage)
{
    return {
        .code = CdpError::kTimeout,
        .detail = std::optional<String>{timeoutMessage},
    };
}

[[nodiscard]] CdpFailure makeSocketClosedFailure(const String &detail)
{
    return {
        .code = CdpError::kSocketClosed,
        .detail = std::optional<String>{detail},
    };
}

[[nodiscard]] std::optional<String>
extractRoutingSessionId(const dto::CdpEventMessage &eventMessage)
{
    if (eventMessage.sessionId)
        return text::optionalString(eventMessage.sessionId).expect();
    if (!eventMessage.params)
        return {};
    const auto sessionIdValue = eventMessage.params->extra["sessionId"];
    if (sessionIdValue.IsMissing())
        return {};
    return TRY(String::fromBytes(sessionIdValue.As<std::string>()));
}

[[nodiscard]] std::optional<String> extractRoutingTargetId(const dto::CdpEventMessage &eventMessage)
{
    if (!eventMessage.params)
        return {};
    const auto targetIdValue = eventMessage.params->extra["targetId"];
    if (targetIdValue.IsMissing())
        return {};
    return TRY(String::fromBytes(targetIdValue.As<std::string>()));
}

} // namespace

CdpClient::CdpClient(
    std::string socketPath, String websocketPath,
    std::shared_ptr<us::websocket::WebSocketConnection> connection, std::string tracePath,
    us::fs::blocking::FileDescriptor traceFile, eng::TaskProcessor &fsTaskProcessor,
    eng::Deadline overallDeadline, chrono::milliseconds commandTimeout
)
    : socketPath(std::move(socketPath)), websocketPath(std::move(websocketPath)),
      connection(std::move(connection)), tracePath(std::move(tracePath)),
      traceFile(std::move(traceFile)), fsTaskProcessor(fsTaskProcessor),
      overallDeadline(overallDeadline), commandTimeout(commandTimeout)
{
}

Expected<std::unique_ptr<CdpClient>, CdpFailure> CdpClient::connect(
    std::string socketPath, String websocketPath, std::string tracePath,
    eng::TaskProcessor &fsTaskProcessor, eng::Deadline overallDeadline,
    chrono::milliseconds handshakeTimeout, chrono::milliseconds commandTimeout,
    i64 maxRemotePayloadBytes
)
{
    using enum CdpError;

    invariant(!tracePath.empty(), "cdp trace path must not be empty"_t);
    invariant(overallDeadline.IsReachable(), "cdp overall deadline must be reachable"_t);

    auto traceFd = TRY(openTraceFile(fsTaskProcessor, tracePath));

    eng::io::Socket socket{eng::io::AddrDomain::kUnix, eng::io::SocketType::kStream};
    const auto handshakeDeadline = pickEarlierDeadline(
        overallDeadline, eng::Deadline::FromDuration(handshakeTimeout)
    );
    auto address = eng::io::Sockaddr::MakeUnixSocketAddress(socketPath);
    TRY(connectCdpSocket(socket, address, handshakeDeadline));

    std::array<char, 16> randomKey{};
    us::crypto::GenerateRandomBlock(us::utils::span(randomKey));
    const auto secWebsocketKey = us::crypto::base64::Base64Encode(
        std::string_view(randomKey.data(), randomKey.size())
    );

    const auto requestPath = websocketPath.empty()
                                 ? "/"_t
                                 : (websocketPath.startsWith('/') ? websocketPath
                                                                  : ("/"_t + websocketPath));
    const auto request = std::format(
        "GET {} HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: Upgrade\r\n"
        "Upgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: {}\r\n"
        "\r\n",
        requestPath, secWebsocketKey
    );
    TRY(sendHandshakeRequest(socket, request, handshakeDeadline));

    auto response = TRY(readHandshakeResponse(socket, handshakeDeadline));
    auto parsedResponse = TRY(parseHandshakeResponse(response));
    TRY(validateHandshakeResponse(parsedResponse, secWebsocketKey));

    auto ws = TRY(
        makeCdpWebSocketConnection(std::move(socket), std::move(address), maxRemotePayloadBytes)
    );

    auto client = std::unique_ptr<CdpClient>(new CdpClient(
        std::move(socketPath), std::move(websocketPath), std::move(ws), std::move(tracePath),
        std::move(traceFd), fsTaskProcessor, overallDeadline, commandTimeout
    ));
    client->startReaderTask();
    return client;
}

CdpClient::~CdpClient() noexcept { closeQuietly(); }

void CdpClient::startReaderTask()
{
    readerTask = std::move(eng::CriticalAsyncNoSpan([this]() { readerLoop(); })).AsTask();
}

Expected<json::Value, CdpFailure> CdpClient::sendRaw(
    const String &method, const json::Value &params, const std::optional<String> &sessionId
)
{
    using enum CdpError;
    auto waiter = std::make_shared<PendingCommandWaiter>();
    i64 id{0};
    {
        auto state = sharedState.Lock();
        if (state->terminalFailure)
            return Unex(*state->terminalFailure);
        if (state->closing || state->closed)
            return Unex(makeSocketClosedFailure("cdp socket is closed"_t));
        id = state->nextRequestId++;
        state->pendingRequests.insertWaiting(id, method, sessionId);
        state->pendingWaiters.emplace(id, waiter);
    }
    dto::CdpCommandRequest request{
        .id = raw(id),
        .method = toBytes(method),
    };
    if (!params.IsMissing())
        request.params = dto::CdpCommandRequest::Params{params};
    if (sessionId)
        request.sessionId = toBytes(*sessionId);
    traceCommand(id, method, sessionId);
    const auto requestBytes = TRY(
        exu::json::stringifyBytes(request, CdpFailure{.code = kProtocol, .detail = {}})
    );
    Expected<void, std::string> sent;
    {
        const auto sendLock = sendState.Lock();
        static_cast<void>(sendLock);
        sent = sendWebSocketText(*connection, requestBytes);
    }
    if (!sent) {
        traceTransportError("send"_t, parsePrintableText(sent.error()));
        CdpFailure failure{.code = kTransport, .detail = {}};
        failTerminal(failure);
        return Unex(failure);
    }

    const auto deadline = pickEarlierDeadline(
        overallDeadline, eng::Deadline::FromDuration(commandTimeout)
    );
    auto waiterState = waiter->data.UniqueLock();
    const auto ready = [&waiterState]() { return waiterState->done; };
    if (!ready() && !waiterState->cv.WaitUntil(waiterState.GetLock(), deadline, ready)) {
        waiterState.GetLock().unlock();
        {
            auto state = sharedState.Lock();
            if (auto *requestInFlight = state->pendingRequests.find(id);
                requestInFlight != nullptr) {
                requestInFlight->ignoreResponse = true;
                state->pendingWaiters.erase(id);
                return Unex(makeTimeoutFailure("timed out waiting for cdp response"_t));
            }
        }
        waiterState.GetLock().lock();
    }
    if (!waiterState->done)
        return Unex(makeTimeoutFailure("timed out waiting for cdp response"_t));
    if (waiterState->failure)
        return Unex(*waiterState->failure);
    invariant(waiterState->result, "cdp waiter completed without result"_t);
    return *waiterState->result;
}

Expected<void, CdpFailure> CdpClient::close()
{
    auto closeAlreadyResolved = false;
    {
        auto state = sharedState.Lock();
        if (state->closed || state->terminalFailure) {
            state->closing = true;
            state->closed = true;
            closeAlreadyResolved = true;
        } else {
            state->closing = true;
        }
    }
    if (closeAlreadyResolved) {
        stopReaderTask();
        connection.reset();
        return {};
    }

    traceClose("out"_t, numericCast<int>(us::websocket::CloseStatus::kNormal));
    Expected<void, std::string> closedConnection;
    {
        const auto sendLock = sendState.Lock();
        static_cast<void>(sendLock);
        closedConnection = connection ? closeWebSocket(*connection) : Expected<void, std::string>{};
    }
    if (!closedConnection) {
        traceTransportError("close"_t, parsePrintableText(closedConnection.error()));
        CdpFailure failure{.code = CdpError::kTransport, .detail = {}};
        failTerminal(failure);
        stopReaderTask();
        connection.reset();
        return Unex(failure);
    }

    stopReaderTask();
    connection.reset();
    return {};
}

Expected<std::unique_ptr<CdpSession>, CdpFailure>
CdpClient::createSession(String sessionId, String targetId)
{
    auto sessionState = std::make_shared<CdpSessionState>();
    {
        auto state = sharedState.Lock();
        if (state->terminalFailure)
            return Unex(*state->terminalFailure);
        if (state->closing || state->closed)
            return Unex(makeSocketClosedFailure("cdp socket is closed"_t));
        state->sessionsById.emplace(sessionId, sessionState);
        state->sessionsByTargetId.emplace(targetId, sessionState);
    }
    return std::unique_ptr<CdpSession>(
        new CdpSession(*this, std::move(sessionId), std::move(targetId), std::move(sessionState))
    );
}

void CdpClient::readerLoop()
{
    while (true) {
        us::websocket::Message message;
        try {
            connection->Recv(message);
        } catch (const std::exception &e) {
            if (isCancelRequested()) {
                failTerminal(makeSocketClosedFailure("websocket close requested"_t));
                return;
            }
            traceTransportError("recv"_t, parsePrintableText(e.what()));
            failTerminal(CdpFailure{.code = CdpError::kTransport, .detail = {}});
            return;
        }
        if (message.close_status) {
            const auto closeCode = us::utils::UnderlyingValue(*message.close_status);
            traceClose("in"_t, closeCode);
            failTerminal(
                CdpFailure{
                    .code = CdpError::kSocketClosed,
                    .detail = text::format("websocket close status {}", closeCode),
                }
            );
            return;
        }
        auto ok = handleMessage(message.data);
        if (!ok) {
            failTerminal(ok.error());
            return;
        }
    }
}

CdpSession::~CdpSession()
{
    if (client == nullptr || sessionState == nullptr)
        return;
    client->unregisterSession(sessionIdValue, targetIdValue, sessionState);
}

Expected<void, CdpFailure> CdpClient::handleMessage(const std::string &payload)
{
    using enum CdpError;
    const auto payloadText = String::fromBytes(payload);
    if (!payloadText)
        return Unex(CdpFailure{.code = kJsonParseFailed, .detail = {}});
    auto value = TRY(
        exu::json::parse<json::Value>(
            *payloadText, CdpFailure{.code = kJsonParseFailed, .detail = {}}
        )
    );
    const auto idValue = value["id"];
    if (!idValue.IsMissing()) {
        const auto id = i64(idValue.As<int64_t>());
        std::shared_ptr<PendingCommandWaiter> waiter;
        std::optional<String> errorMessage;
        CdpPendingRequest requestCopy;
        {
            auto state = sharedState.Lock();
            auto *request = state->pendingRequests.find(id);
            if (request == nullptr) {
                if (state->closing || state->closed || state->terminalFailure)
                    return {};
                return Unex(
                    CdpFailure{
                        .code = kProtocol,
                        .detail = text::format("cdp response for unknown request id {}", id),
                    }
                );
            }
            requestCopy = *request;
            const auto errorValue = value["error"];
            if (!errorValue.IsMissing()) {
                errorMessage = TRY(getErrorMessage(errorValue));
            }
            if (request->ignoreResponse) {
                state->pendingRequests.erase(id);
                state->pendingWaiters.erase(id);
                traceResponse(id, &requestCopy, errorMessage);
                return {};
            }
            const auto waiterIt = state->pendingWaiters.find(id);
            if (waiterIt == std::end(state->pendingWaiters)) {
                if (state->closing || state->closed || state->terminalFailure)
                    return {};
                return Unex(
                    CdpFailure{
                        .code = kProtocol,
                        .detail = text::format("missing cdp waiter for request id {}", id),
                    }
                );
            }
            waiter = waiterIt->second;
            state->pendingWaiters.erase(waiterIt);
            state->pendingRequests.erase(id);
        }
        const auto errorValue = value["error"];
        traceResponse(id, &requestCopy, errorMessage);
        auto waiterState = waiter->data.Lock();
        if (errorValue.IsMissing()) {
            waiterState->result = value["result"];
        } else {
            waiterState->failure = CdpFailure{.code = kCommandFailed, .detail = *errorMessage};
        }
        waiterState->done = true;
        waiterState->cv.NotifyOne();
        return {};
    }

    if (value["method"].IsMissing()) {
        return Unex(
            CdpFailure{
                .code = kProtocol,
                .detail = "cdp message missing id and method"_t,
            }
        );
    }

    auto eventMessage = TRY(
        exu::json::as<dto::CdpEventMessage>(value, CdpFailure{.code = kProtocol, .detail = {}})
    );
    const auto sessionId = TRY_MAP_ERR(text::optionalString(eventMessage.sessionId), [](auto) {
        return CdpFailure{
            .code = CdpError::kProtocol,
            .detail = "cdp event contained invalid session id text"_t,
        };
    });
    const auto method = TRY_MAP_ERR(String::fromBytes(eventMessage.method), [](auto) {
        return CdpFailure{
            .code = CdpError::kProtocol,
            .detail = "cdp event contained invalid method text"_t,
        };
    });
    const auto routingSessionId = extractRoutingSessionId(eventMessage);
    const auto routingTargetId = extractRoutingTargetId(eventMessage);
    traceEvent(method, sessionId);

    CdpEvent event{
        .method = method,
        .params = std::move(eventMessage.params),
        .sessionId = sessionId,
    };

    std::shared_ptr<CdpSessionState> sessionState;
    {
        auto state = sharedState.Lock();
        if (routingSessionId) {
            if (const auto it = state->sessionsById.find(*routingSessionId);
                it != std::end(state->sessionsById)) {
                sessionState = it->second;
            }
        }
        if (!sessionState) {
            if (routingTargetId) {
                if (const auto it = state->sessionsByTargetId.find(*routingTargetId);
                    it != std::end(state->sessionsByTargetId)) {
                    sessionState = it->second;
                }
            }
        }
    }
    if (!sessionState)
        return {};

    auto sessionData = sessionState->data.Lock();
    if (sessionData->closed || sessionData->failure)
        return {};
    sessionData->events.push_back(std::move(event));
    sessionData->cv.NotifyOne();
    return {};
}

Expected<CdpEvent, CdpFailure> CdpClient::waitForSessionEvent(
    const std::shared_ptr<CdpSessionState> &sessionState, eng::Deadline deadline,
    const String &timeoutMessage
)
{
    auto sessionData = sessionState->data.UniqueLock();
    const auto ready = [&sessionData]() {
        return !sessionData->events.empty() || sessionData->failure || sessionData->closed;
    };
    if (!ready() && !sessionData->cv.WaitUntil(sessionData.GetLock(), deadline, ready))
        return Unex(makeTimeoutFailure(timeoutMessage));
    if (sessionData->failure)
        return Unex(*sessionData->failure);
    if (sessionData->closed)
        return Unex(makeSocketClosedFailure("cdp session is closed"_t));
    invariant(!sessionData->events.empty(), "cdp session woke without queued event"_t);
    auto event = std::move(sessionData->events.front());
    sessionData->events.pop_front();
    return event;
}

std::vector<CdpEvent>
CdpClient::drainSessionEvents(const std::shared_ptr<CdpSessionState> &sessionState)
{
    std::vector<CdpEvent> events;
    auto sessionData = sessionState->data.Lock();
    events.reserve(sessionData->events.size());
    while (!sessionData->events.empty()) {
        events.push_back(std::move(sessionData->events.front()));
        sessionData->events.pop_front();
    }
    return events;
}

void CdpClient::unregisterSession(
    const String &sessionId, const String &targetId,
    const std::shared_ptr<CdpSessionState> &sessionState
) noexcept
{
    {
        auto state = sharedState.Lock();
        if (const auto it = state->sessionsById.find(sessionId);
            it != std::end(state->sessionsById) && it->second == sessionState) {
            state->sessionsById.erase(it);
        }
        if (const auto it = state->sessionsByTargetId.find(targetId);
            it != std::end(state->sessionsByTargetId) && it->second == sessionState) {
            state->sessionsByTargetId.erase(it);
        }
    }
    auto sessionData = sessionState->data.Lock();
    sessionData->closed = true;
    if (!sessionData->failure)
        sessionData->failure = makeSocketClosedFailure("cdp session is closed"_t);
    sessionData->cv.NotifyAll();
}

void CdpClient::closeQuietly() noexcept
{
    {
        auto state = sharedState.Lock();
        state->closing = true;
        state->closed = true;
    }
    stopReaderTask();
    connection.reset();
}

void CdpClient::stopReaderTask() noexcept
{
    if (!readerTask.IsValid())
        return;
    readerTask.RequestCancel();
    const eng::TaskCancellationBlocker blocker;
    static_cast<void>(readerTask.WaitNothrow());
    readerTask = {};
}

void CdpClient::failTerminal(CdpFailure failure)
{
    std::vector<std::shared_ptr<PendingCommandWaiter>> waiters;
    std::vector<std::shared_ptr<CdpSessionState>> sessions;
    {
        auto state = sharedState.Lock();
        if (state->terminalFailure)
            return;
        state->terminalFailure = failure;
        state->closed = true;
        for (auto &waiter : std::views::values(state->pendingWaiters))
            waiters.push_back(waiter);
        state->pendingWaiters.clear();
        state->pendingRequests.clear();
        for (auto &sessionState : std::views::values(state->sessionsById))
            sessions.push_back(sessionState);
        state->sessionsById.clear();
        state->sessionsByTargetId.clear();
    }

    for (const auto &waiter : waiters) {
        auto waiterState = waiter->data.Lock();
        waiterState->failure = failure;
        waiterState->done = true;
        waiterState->cv.NotifyOne();
    }
    for (const auto &sessionState : sessions) {
        auto sessionData = sessionState->data.Lock();
        sessionData->failure = failure;
        sessionData->closed = true;
        sessionData->cv.NotifyAll();
    }
}

Expected<CdpEvent, CdpFailure>
CdpSession::waitEvent(eng::Deadline deadline, const String &timeoutMessage)
{
    invariant(client != nullptr, "cdp session is not attached"_t);
    invariant(sessionState != nullptr, "cdp session state is missing"_t);
    return client->waitForSessionEvent(sessionState, deadline, timeoutMessage);
}

std::vector<CdpEvent> CdpSession::drainAvailableEvents()
{
    invariant(client != nullptr, "cdp session is not attached"_t);
    invariant(sessionState != nullptr, "cdp session state is missing"_t);
    return client->drainSessionEvents(sessionState);
}

Expected<void, CdpFailure> CdpClient::writeTraceLine(const json::Value &value)
{
    using enum CdpError;
    auto line = TRY(
        exu::json::stringifyBytes(value, CdpFailure{.code = kTraceWriteFailed, .detail = {}})
    );
    line.push_back('\n');
    TRY(writeTraceBytes(fsTaskProcessor, traceFile, line));
    return {};
}

void CdpClient::writeTraceLineBestEffort(const json::Value &value)
{
    const auto written = writeTraceLine(value);
    if (written)
        return;
    LOG_WARNING() << std::format(
        "Suppressing CDP trace write failure for {} (code={})", tracePath,
        us::utils::UnderlyingValue(written.error().code)
    );
}

void CdpClient::traceCommand(i64 id, const String &method, const std::optional<String> &sessionId)
{
    json::ValueBuilder entry;
    entry["ts"] = currentTraceTimestamp();
    entry["direction"] = "out";
    entry["kind"] = "command";
    entry["id"] = raw(id);
    entry["method"] = toBytes(method);
    if (sessionId)
        entry["sessionId"] = toBytes(*sessionId);
    writeTraceLineBestEffort(entry.ExtractValue());
}

void CdpClient::traceResponse(
    i64 id, const CdpPendingRequest *request, const std::optional<String> &error
)
{
    json::ValueBuilder entry;
    entry["ts"] = currentTraceTimestamp();
    entry["direction"] = "in";
    entry["kind"] = error ? "error" : "response";
    entry["id"] = raw(id);
    if (request) {
        entry["method"] = toBytes(request->method);
        if (request->sessionId)
            entry["sessionId"] = toBytes(*request->sessionId);
    }
    if (error)
        entry["error"] = toBytes(*error);
    writeTraceLineBestEffort(entry.ExtractValue());
}

void CdpClient::traceEvent(const String &method, const std::optional<String> &sessionId)
{
    json::ValueBuilder entry;
    entry["ts"] = currentTraceTimestamp();
    entry["direction"] = "in";
    entry["kind"] = "event";
    entry["method"] = toBytes(method);
    if (sessionId)
        entry["sessionId"] = toBytes(*sessionId);
    writeTraceLineBestEffort(entry.ExtractValue());
}

void CdpClient::traceClose(const String &direction, int closeCode)
{
    json::ValueBuilder entry;
    entry["ts"] = currentTraceTimestamp();
    entry["direction"] = toBytes(direction);
    entry["kind"] = "close";
    entry["closeCode"] = closeCode;
    writeTraceLineBestEffort(entry.ExtractValue());
}

void CdpClient::traceTransportError(const String &operation, const String &error)
{
    json::ValueBuilder entry;
    entry["ts"] = currentTraceTimestamp();
    entry["kind"] = "transport_error";
    entry["operation"] = toBytes(operation);
    entry["error"] = toBytes(error);
    writeTraceLineBestEffort(entry.ExtractValue());
}

} // namespace v1::crawler
