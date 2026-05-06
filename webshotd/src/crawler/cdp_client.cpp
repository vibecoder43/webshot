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
#include <userver/utils/text_light.hpp>
#include <userver/utils/underlying_value.hpp>
#include <userver/websocket/connection.hpp>
namespace chrono = std::chrono;

namespace ws::crawler {
namespace us = userver;
namespace eng = us::engine;
namespace json = us::formats::json;
namespace datetime = us::utils::datetime;
namespace utext = us::utils::text;
using namespace text::literals;
using ws::Expected;

namespace {

constexpr auto kMaxHandshakeResponseBytes = 16_i64 * 1024_i64;
constexpr std::string_view kWebsocketGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

[[nodiscard]] bool ICaseEqual(std::string_view lhs, std::string_view rhs) noexcept
{
    return lhs.size() == rhs.size() && utext::ICaseStartsWith(lhs, rhs);
}

[[nodiscard]] bool IsCancelRequested() { return eng::current_task::IsCancelRequested(); }

[[nodiscard]] std::string CurrentTraceTimestamp()
{
    return datetime::UtcTimestring(datetime::Now(), datetime::kRfc3339Format);
}

struct HandshakeResponse final {
    std::string status_line;
    std::unordered_map<std::string, std::string> headers;
    std::string raw_headers;
};

[[nodiscard]] Expected<us::fs::blocking::FileDescriptor, CdpFailure>
OpenTraceFile(eng::TaskProcessor &fs_task_processor, const std::string &trace_path)
{
    using enum CdpError;
    try {
        return eng::AsyncNoSpan(
                   fs_task_processor, [&trace_path] {
                       return us::fs::blocking::FileDescriptor::Open(
                           trace_path, us::fs::blocking::OpenMode{
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

[[nodiscard]] Expected<void, CdpFailure> WriteTraceBytes(
    eng::TaskProcessor &fs_task_processor, us::fs::blocking::FileDescriptor &trace_file,
    const std::string &line
)
{
    using enum CdpError;
    try {
        eng::AsyncNoSpan(fs_task_processor, [&trace_file, &line] { trace_file.Write(line); }).Get();
        return {};
    } catch (const std::runtime_error &) {
        return Unex(CdpFailure{.code = kTraceWriteFailed, .detail = {}});
    }
}

[[nodiscard]] Expected<void, CdpFailure>
ConnectCdpSocket(eng::io::Socket &socket, const eng::io::Sockaddr &address, eng::Deadline deadline)
{
    using enum CdpError;
    try {
        socket.Connect(address, deadline);
        return {};
    } catch (const std::exception &) {
        if (IsCancelRequested())
            throw;
        return Unex(CdpFailure{.code = kSocketConnectFailed, .detail = {}});
    }
}

[[nodiscard]] Expected<void, CdpFailure>
SendHandshakeRequest(eng::io::Socket &socket, const std::string &request, eng::Deadline deadline)
{
    using enum CdpError;
    try {
        static_cast<void>(socket.SendAll(request.data(), request.size(), deadline));
        return {};
    } catch (const std::exception &) {
        if (IsCancelRequested())
            throw;
        return Unex(CdpFailure{.code = kTransport, .detail = {}});
    }
}

[[nodiscard]] Expected<size_t, CdpFailure>
ReadHandshakeByte(eng::io::Socket &socket, char &ch, eng::Deadline deadline)
{
    using enum CdpError;
    try {
        return socket.RecvSome(&ch, 1, deadline);
    } catch (const std::exception &) {
        if (IsCancelRequested())
            throw;
        return Unex(CdpFailure{.code = kTransport, .detail = {}});
    }
}

[[nodiscard]] Expected<std::shared_ptr<us::websocket::WebSocketConnection>, CdpFailure>
MakeCdpWebSocketConnection(
    eng::io::Socket socket, eng::io::Sockaddr address, i64 max_remote_payload_bytes
)
{
    using enum CdpError;
    try {
        auto connection_socket = std::make_unique<eng::io::Socket>(std::move(socket));
        us::websocket::Config ws_config;
        ws_config.max_remote_payload = NumericCast<decltype(ws_config.max_remote_payload)>(
            max_remote_payload_bytes
        );
        return us::websocket::MakeClientWebSocketConnection(
            std::move(connection_socket), std::move(address), ws_config
        );
    } catch (const std::exception &) {
        if (IsCancelRequested())
            throw;
        return Unex(CdpFailure{.code = kTransport, .detail = {}});
    }
}

[[nodiscard]] Expected<void, std::string>
SendWebSocketText(us::websocket::WebSocketConnection &connection, const std::string &request_bytes)
{
    try {
        connection.SendText(request_bytes);
        return {};
    } catch (const std::exception &e) {
        if (IsCancelRequested())
            throw;
        return Unex(std::string(e.what()));
    }
}

[[nodiscard]] Expected<void, std::string>
CloseWebSocket(us::websocket::WebSocketConnection &connection)
{
    try {
        connection.Close(us::websocket::CloseStatus::kNormal);
        return {};
    } catch (const std::exception &e) {
        if (IsCancelRequested())
            throw;
        return Unex(std::string(e.what()));
    }
}

[[nodiscard]] Expected<String, CdpFailure> GetErrorMessage(const json::Value &error)
{
    using enum CdpError;
    Invariant(error.IsObject(), "cdp error payload must be object"_t);
    const auto parsed = TRY(
        ws::json::As<dto::CdpError, CdpFailure>(error, [](const json::Exception &e) -> CdpFailure {
            Invariant(
                text::Format("cdp error payload does not match dto::CdpError ({})", e.what())
            );
        })
    );
    auto message = String::FromBytes(parsed.message);
    if (!message)
        return Unex(CdpFailure{.code = kProtocol, .detail = {}});
    return std::move(*message);
}

[[nodiscard]] bool ContainsHeaderToken(std::string_view value, std::string_view token)
{
    auto remaining = value;
    while (true) {
        const auto comma_pos = remaining.find(',');
        const auto part = comma_pos == std::string_view::npos ? remaining
                                                              : remaining.substr(0, comma_pos);
        const auto trimmed_part = utext::TrimView(part);
        if (ICaseEqual(trimmed_part, token))
            return true;
        if (comma_pos == std::string_view::npos)
            break;
        remaining.remove_prefix(comma_pos + 1);
    }

    return false;
}

[[nodiscard]] Expected<std::string, CdpFailure>
ReadHandshakeResponse(eng::io::Socket &socket, eng::Deadline deadline)
{
    using enum CdpError;
    std::string response;
    response.reserve(1024);

    while (!response.contains("\r\n\r\n")) {
        if (ssize(response) >= kMaxHandshakeResponseBytes)
            return Unex(CdpFailure{.code = kHandshakeResponseTooLarge, .detail = {}});

        char ch = '\0';
        const auto bytes_read = TRY(ReadHandshakeByte(socket, ch, deadline));
        if (bytes_read == 0)
            return Unex(CdpFailure{.code = kHandshakeUnexpectedEof, .detail = {}});
        response.push_back(ch);
    }

    return response;
}

[[nodiscard]] std::unordered_map<std::string, std::string>
ParseHandshakeHeaders(std::string_view headers_block)
{
    std::unordered_map<std::string, std::string> headers;

    auto remaining = headers_block;
    while (!remaining.empty()) {
        const auto line_end = remaining.find("\r\n");
        const auto line = line_end == std::string::npos ? remaining : remaining.substr(0, line_end);
        if (line.empty())
            break;
        const auto colon_pos = line.find(':');
        if (colon_pos != std::string::npos) {
            const auto trimmed = utext::TrimView(line.substr(colon_pos + 1));
            headers.emplace(absl::AsciiStrToLower(line.substr(0, colon_pos)), std::string(trimmed));
        }

        if (line_end == std::string::npos)
            break;
        remaining.remove_prefix(line_end + 2);
    }

    return headers;
}

[[nodiscard]] Expected<HandshakeResponse, CdpFailure>
ParseHandshakeResponse(std::string_view response)
{
    const auto headers_end = response.find("\r\n\r\n");
    if (headers_end == std::string::npos)
        return Unex(CdpFailure{.code = CdpError::kHandshakeMalformedResponse, .detail = {}});

    HandshakeResponse parsed;
    parsed.raw_headers = std::string(response.substr(0, headers_end + 4));

    const auto status_line_end = response.find("\r\n");
    if (status_line_end == std::string::npos)
        return Unex(CdpFailure{.code = CdpError::kHandshakeMalformedResponse, .detail = {}});

    parsed.status_line = std::string(response.substr(0, status_line_end));
    parsed.headers = ParseHandshakeHeaders(
        response.substr(status_line_end + 2, headers_end - (status_line_end + 2))
    );
    return parsed;
}

Expected<void, CdpFailure>
ValidateHandshakeResponse(const HandshakeResponse &response, std::string_view sec_websocket_key)
{
    if (!utext::StartsWith(response.status_line, "HTTP/1.1 101 ") &&
        response.status_line != "HTTP/1.1 101") {
        return Unex(CdpFailure{.code = CdpError::kHandshakeRejected, .detail = {}});
    }

    const auto upgrade_it = response.headers.find("upgrade");
    if (upgrade_it == std::end(response.headers) ||
        !ICaseEqual(std::string_view{upgrade_it->second}, "websocket")) {
        return Unex(CdpFailure{.code = CdpError::kHandshakeMissingHeader, .detail = {}});
    }

    const auto connection_it = response.headers.find("connection");
    if (connection_it == std::end(response.headers) ||
        !ContainsHeaderToken(connection_it->second, "upgrade")) {
        return Unex(CdpFailure{.code = CdpError::kHandshakeMissingHeader, .detail = {}});
    }

    const auto accept_it = response.headers.find("sec-websocket-accept");
    if (accept_it == std::end(response.headers)) {
        return Unex(CdpFailure{.code = CdpError::kHandshakeMissingHeader, .detail = {}});
    }

    const auto expected_accept = us::crypto::base64::Base64Encode(
        us::crypto::hash::Sha1(
            {sec_websocket_key, kWebsocketGuid}, us::crypto::hash::OutputEncoding::kBinary
        )
    );
    if (accept_it->second != expected_accept)
        return Unex(CdpFailure{.code = CdpError::kHandshakeRejected, .detail = {}});
    return {};
}

} // namespace

String DescribeCdpFailure(const String &action, const CdpFailure &failure)
{
    auto message = action;
    if (failure.detail)
        message = text::Format("{}: {}", message, *failure.detail);
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

[[nodiscard]] String ParsePrintableText(std::string_view value)
{
    return *String::FromBytes(value);
}

[[nodiscard]] CdpFailure MakeTimeoutFailure(const String &timeout_message)
{
    return {
        .code = CdpError::kTimeout,
        .detail = std::optional<String>{timeout_message},
    };
}

[[nodiscard]] CdpFailure MakeSocketClosedFailure(const String &detail)
{
    return {
        .code = CdpError::kSocketClosed,
        .detail = std::optional<String>{detail},
    };
}

[[nodiscard]] std::optional<String>
ExtractRoutingSessionId(const dto::CdpEventMessage &event_message)
{
    if (event_message.sessionId)
        return *text::OptionalString(event_message.sessionId);
    if (!event_message.params)
        return {};
    const auto session_id_value = event_message.params->extra["sessionId"];
    if (session_id_value.IsMissing())
        return {};
    return TRY(String::FromBytes(session_id_value.As<std::string>()));
}

[[nodiscard]] std::optional<String>
ExtractRoutingTargetId(const dto::CdpEventMessage &event_message)
{
    if (!event_message.params)
        return {};
    const auto target_id_value = event_message.params->extra["targetId"];
    if (target_id_value.IsMissing())
        return {};
    return TRY(String::FromBytes(target_id_value.As<std::string>()));
}

} // namespace

CdpClient::CdpClient(
    std::string socket_path, String websocket_path,
    std::shared_ptr<us::websocket::WebSocketConnection> connection, std::string trace_path,
    us::fs::blocking::FileDescriptor trace_file, eng::TaskProcessor &fs_task_processor,
    eng::Deadline overall_deadline, chrono::milliseconds command_timeout
)
    : socket_path_(std::move(socket_path)), websocket_path_(std::move(websocket_path)),
      connection_(std::move(connection)), trace_path_(std::move(trace_path)),
      trace_file_(std::move(trace_file)), fs_task_processor_(fs_task_processor),
      overall_deadline_(overall_deadline), command_timeout_(command_timeout)
{
}

Expected<std::unique_ptr<CdpClient>, CdpFailure> CdpClient::Connect(
    std::string socket_path, String websocket_path, std::string trace_path,
    eng::TaskProcessor &fs_task_processor, eng::Deadline overall_deadline,
    chrono::milliseconds handshake_timeout, chrono::milliseconds command_timeout,
    i64 max_remote_payload_bytes
)
{
    using enum CdpError;

    Invariant(!trace_path.empty(), "cdp trace path must not be empty"_t);
    Invariant(overall_deadline.IsReachable(), "cdp overall deadline must be reachable"_t);

    auto trace_fd = TRY(OpenTraceFile(fs_task_processor, trace_path));

    eng::io::Socket socket{eng::io::AddrDomain::kUnix, eng::io::SocketType::kStream};
    const auto handshake_deadline = PickEarlierDeadline(
        overall_deadline, eng::Deadline::FromDuration(handshake_timeout)
    );
    auto address = eng::io::Sockaddr::MakeUnixSocketAddress(socket_path);
    TRY(ConnectCdpSocket(socket, address, handshake_deadline));

    std::array<char, 16> random_key{};
    us::crypto::GenerateRandomBlock(us::utils::span(random_key));
    const auto sec_websocket_key = us::crypto::base64::Base64Encode(
        std::string_view(random_key.data(), random_key.size())
    );

    const auto request_path = websocket_path.Empty()
                                  ? "/"_t
                                  : (websocket_path.StartsWith('/') ? websocket_path
                                                                    : ("/"_t + websocket_path));
    const auto request = std::format(
        "GET {} HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: Upgrade\r\n"
        "Upgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: {}\r\n"
        "\r\n",
        request_path, sec_websocket_key
    );
    TRY(SendHandshakeRequest(socket, request, handshake_deadline));

    auto response = TRY(ReadHandshakeResponse(socket, handshake_deadline));
    auto parsed_response = TRY(ParseHandshakeResponse(response));
    TRY(ValidateHandshakeResponse(parsed_response, sec_websocket_key));

    auto ws = TRY(
        MakeCdpWebSocketConnection(std::move(socket), std::move(address), max_remote_payload_bytes)
    );

    auto client = std::unique_ptr<CdpClient>(new CdpClient(
        std::move(socket_path), std::move(websocket_path), std::move(ws), std::move(trace_path),
        std::move(trace_fd), fs_task_processor, overall_deadline, command_timeout
    ));
    client->StartReaderTask();
    return client;
}

CdpClient::~CdpClient() noexcept { CloseQuietly(); }

void CdpClient::StartReaderTask()
{
    reader_task_ = std::move(eng::CriticalAsyncNoSpan([this]() { ReaderLoop(); })).AsTask();
}

Expected<json::Value, CdpFailure> CdpClient::SendRaw(
    const String &method, const json::Value &params, const std::optional<String> &session_id
)
{
    using enum CdpError;
    auto waiter = std::make_shared<PendingCommandWaiter>();
    i64 id{0};
    {
        auto state = shared_state_.Lock();
        if (state->terminal_failure)
            return Unex(*state->terminal_failure);
        if (state->closing || state->closed)
            return Unex(MakeSocketClosedFailure("cdp socket is closed"_t));
        id = state->next_request_id++;
        state->pending_requests.InsertWaiting(id, method, session_id);
        state->pending_waiters.emplace(id, waiter);
    }
    dto::CdpCommandRequest request{
        .id = Raw(id),
        .method = method.ToBytes(),
    };
    if (!params.IsMissing())
        request.params = dto::CdpCommandRequest::Params{params};
    if (session_id)
        request.sessionId = session_id->ToBytes();
    TraceCommand(id, method, session_id);
    const auto request_bytes = TRY(
        ws::json::StringifyBytes(request, CdpFailure{.code = kProtocol, .detail = {}})
    );
    Expected<void, std::string> sent;
    {
        const auto send_lock = send_state_.Lock();
        static_cast<void>(send_lock);
        sent = SendWebSocketText(*connection_, request_bytes);
    }
    if (!sent) {
        TraceTransportError("send"_t, ParsePrintableText(sent.Error()));
        CdpFailure failure{.code = kTransport, .detail = {}};
        FailTerminal(failure);
        return Unex(failure);
    }

    const auto deadline = PickEarlierDeadline(
        overall_deadline_, eng::Deadline::FromDuration(command_timeout_)
    );
    auto waiter_state = waiter->data.UniqueLock();
    const auto ready = [&waiter_state]() { return waiter_state->done; };
    if (!ready() && !waiter_state->cv.WaitUntil(waiter_state.GetLock(), deadline, ready)) {
        waiter_state.GetLock().unlock();
        {
            auto state = shared_state_.Lock();
            if (auto *request_in_flight = state->pending_requests.Find(id);
                request_in_flight != nullptr) {
                request_in_flight->ignore_response = true;
                state->pending_waiters.erase(id);
                return Unex(MakeTimeoutFailure("timed out waiting for cdp response"_t));
            }
        }
        waiter_state.GetLock().lock();
    }
    if (!waiter_state->done)
        return Unex(MakeTimeoutFailure("timed out waiting for cdp response"_t));
    if (waiter_state->failure)
        return Unex(*waiter_state->failure);
    Invariant(waiter_state->result, "cdp waiter completed without result"_t);
    return *waiter_state->result;
}

Expected<void, CdpFailure> CdpClient::Close()
{
    auto close_already_resolved = false;
    {
        auto state = shared_state_.Lock();
        if (state->closed || state->terminal_failure) {
            state->closing = true;
            state->closed = true;
            close_already_resolved = true;
        } else {
            state->closing = true;
        }
    }
    if (close_already_resolved) {
        StopReaderTask();
        connection_.reset();
        return {};
    }

    TraceClose("out"_t, NumericCast<int>(us::websocket::CloseStatus::kNormal));
    Expected<void, std::string> closed_connection;
    {
        const auto send_lock = send_state_.Lock();
        static_cast<void>(send_lock);
        closed_connection = connection_ ? CloseWebSocket(*connection_)
                                        : Expected<void, std::string>{};
    }
    if (!closed_connection) {
        TraceTransportError("close"_t, ParsePrintableText(closed_connection.Error()));
        CdpFailure failure{.code = CdpError::kTransport, .detail = {}};
        FailTerminal(failure);
        StopReaderTask();
        connection_.reset();
        return Unex(failure);
    }

    StopReaderTask();
    connection_.reset();
    return {};
}

Expected<std::unique_ptr<CdpSession>, CdpFailure>
CdpClient::CreateSession(String session_id, String target_id)
{
    auto session_state = std::make_shared<CdpSessionState>();
    {
        auto state = shared_state_.Lock();
        if (state->terminal_failure)
            return Unex(*state->terminal_failure);
        if (state->closing || state->closed)
            return Unex(MakeSocketClosedFailure("cdp socket is closed"_t));
        state->sessions_by_id.emplace(session_id, session_state);
        state->sessions_by_target_id.emplace(target_id, session_state);
    }
    return std::unique_ptr<CdpSession>(
        new CdpSession(*this, std::move(session_id), std::move(target_id), std::move(session_state))
    );
}

void CdpClient::ReaderLoop()
{
    while (true) {
        us::websocket::Message message;
        try {
            connection_->Recv(message);
        } catch (const std::exception &e) {
            if (IsCancelRequested()) {
                FailTerminal(MakeSocketClosedFailure("websocket close requested"_t));
                return;
            }
            TraceTransportError("recv"_t, ParsePrintableText(e.what()));
            FailTerminal(CdpFailure{.code = CdpError::kTransport, .detail = {}});
            return;
        }
        if (message.close_status) {
            const auto close_code = us::utils::UnderlyingValue(*message.close_status);
            TraceClose("in"_t, close_code);
            FailTerminal(
                CdpFailure{
                    .code = CdpError::kSocketClosed,
                    .detail = text::Format("websocket close status {}", close_code),
                }
            );
            return;
        }
        auto ok = HandleMessage(message.data);
        if (!ok) {
            FailTerminal(ok.Error());
            return;
        }
    }
}

CdpSession::~CdpSession()
{
    if (client_ == nullptr || session_state_ == nullptr)
        return;
    client_->UnregisterSession(session_id_, target_id_, session_state_);
}

Expected<void, CdpFailure> CdpClient::HandleMessage(const std::string &payload)
{
    using enum CdpError;
    const auto payload_text = String::FromBytes(payload);
    if (!payload_text)
        return Unex(CdpFailure{.code = kJsonParseFailed, .detail = {}});
    auto value = TRY(
        ws::json::Parse<json::Value>(
            *payload_text, CdpFailure{.code = kJsonParseFailed, .detail = {}}
        )
    );
    const auto id_value = value["id"];
    if (!id_value.IsMissing()) {
        const auto id = i64(id_value.As<int64_t>());
        std::shared_ptr<PendingCommandWaiter> waiter;
        std::optional<String> error_message;
        CdpPendingRequest request_copy;
        {
            auto state = shared_state_.Lock();
            auto *request = state->pending_requests.Find(id);
            if (request == nullptr) {
                if (state->closing || state->closed || state->terminal_failure)
                    return {};
                return Unex(
                    CdpFailure{
                        .code = kProtocol,
                        .detail = text::Format("cdp response for unknown request id {}", id),
                    }
                );
            }
            request_copy = *request;
            const auto error_value = value["error"];
            if (!error_value.IsMissing()) {
                error_message = TRY(GetErrorMessage(error_value));
            }
            if (request->ignore_response) {
                state->pending_requests.Erase(id);
                state->pending_waiters.erase(id);
                TraceResponse(id, &request_copy, error_message);
                return {};
            }
            const auto waiter_it = state->pending_waiters.find(id);
            if (waiter_it == std::end(state->pending_waiters)) {
                if (state->closing || state->closed || state->terminal_failure)
                    return {};
                return Unex(
                    CdpFailure{
                        .code = kProtocol,
                        .detail = text::Format("missing cdp waiter for request id {}", id),
                    }
                );
            }
            waiter = waiter_it->second;
            state->pending_waiters.erase(waiter_it);
            state->pending_requests.Erase(id);
        }
        const auto error_value = value["error"];
        TraceResponse(id, &request_copy, error_message);
        auto waiter_state = waiter->data.Lock();
        if (error_value.IsMissing()) {
            waiter_state->result = value["result"];
        } else {
            waiter_state->failure = CdpFailure{.code = kCommandFailed, .detail = *error_message};
        }
        waiter_state->done = true;
        waiter_state->cv.NotifyOne();
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

    auto event_message = TRY(
        ws::json::As<dto::CdpEventMessage>(value, CdpFailure{.code = kProtocol, .detail = {}})
    );
    const auto session_id = TRY_MAP_ERR(
        text::OptionalString(event_message.sessionId), ([](auto) {
            return CdpFailure{
                .code = CdpError::kProtocol,
                .detail = "cdp event contained invalid session id text"_t,
            };
        })
    );
    const auto method = TRY_MAP_ERR(String::FromBytes(event_message.method), ([](auto) {
                                        return CdpFailure{
                                            .code = CdpError::kProtocol,
                                            .detail = "cdp event contained invalid method text"_t,
                                        };
                                    }));
    const auto routing_session_id = ExtractRoutingSessionId(event_message);
    const auto routing_target_id = ExtractRoutingTargetId(event_message);
    TraceEvent(method, session_id);

    CdpEvent event{
        .method = method,
        .params = std::move(event_message.params),
        .session_id = session_id,
    };

    std::shared_ptr<CdpSessionState> session_state;
    {
        auto state = shared_state_.Lock();
        if (routing_session_id) {
            if (const auto it = state->sessions_by_id.find(*routing_session_id);
                it != std::end(state->sessions_by_id)) {
                session_state = it->second;
            }
        }
        if (!session_state) {
            if (routing_target_id) {
                if (const auto it = state->sessions_by_target_id.find(*routing_target_id);
                    it != std::end(state->sessions_by_target_id)) {
                    session_state = it->second;
                }
            }
        }
    }
    if (!session_state)
        return {};

    auto session_data = session_state->data.Lock();
    if (session_data->closed || session_data->failure)
        return {};
    session_data->events.push_back(std::move(event));
    session_data->cv.NotifyOne();
    return {};
}

Expected<CdpEvent, CdpFailure> CdpClient::WaitForSessionEvent(
    const std::shared_ptr<CdpSessionState> &session_state, eng::Deadline deadline,
    const String &timeout_message
)
{
    auto session_data = session_state->data.UniqueLock();
    const auto ready = [&session_data]() {
        return !session_data->events.empty() || session_data->failure || session_data->closed;
    };
    if (!ready() && !session_data->cv.WaitUntil(session_data.GetLock(), deadline, ready))
        return Unex(MakeTimeoutFailure(timeout_message));
    if (session_data->failure)
        return Unex(*session_data->failure);
    if (session_data->closed)
        return Unex(MakeSocketClosedFailure("cdp session is closed"_t));
    Invariant(!session_data->events.empty(), "cdp session woke without queued event"_t);
    auto event = std::move(session_data->events.front());
    session_data->events.pop_front();
    return event;
}

std::vector<CdpEvent>
CdpClient::DrainSessionEvents(const std::shared_ptr<CdpSessionState> &session_state)
{
    std::vector<CdpEvent> events;
    auto session_data = session_state->data.Lock();
    events.reserve(session_data->events.size());
    while (!session_data->events.empty()) {
        events.push_back(std::move(session_data->events.front()));
        session_data->events.pop_front();
    }
    return events;
}

void CdpClient::UnregisterSession(
    const String &session_id, const String &target_id,
    const std::shared_ptr<CdpSessionState> &session_state
) noexcept
{
    {
        auto state = shared_state_.Lock();
        if (const auto it = state->sessions_by_id.find(session_id);
            it != std::end(state->sessions_by_id) && it->second == session_state) {
            state->sessions_by_id.erase(it);
        }
        if (const auto it = state->sessions_by_target_id.find(target_id);
            it != std::end(state->sessions_by_target_id) && it->second == session_state) {
            state->sessions_by_target_id.erase(it);
        }
    }
    auto session_data = session_state->data.Lock();
    session_data->closed = true;
    if (!session_data->failure)
        session_data->failure = MakeSocketClosedFailure("cdp session is closed"_t);
    session_data->cv.NotifyAll();
}

void CdpClient::CloseQuietly() noexcept
{
    {
        auto state = shared_state_.Lock();
        state->closing = true;
        state->closed = true;
    }
    StopReaderTask();
    connection_.reset();
}

void CdpClient::StopReaderTask() noexcept
{
    if (!reader_task_.IsValid())
        return;
    reader_task_.RequestCancel();
    const eng::TaskCancellationBlocker blocker;
    static_cast<void>(reader_task_.WaitNothrow());
    reader_task_ = {};
}

void CdpClient::FailTerminal(CdpFailure failure)
{
    std::vector<std::shared_ptr<PendingCommandWaiter>> waiters;
    std::vector<std::shared_ptr<CdpSessionState>> sessions;
    {
        auto state = shared_state_.Lock();
        if (state->terminal_failure)
            return;
        state->terminal_failure = failure;
        state->closed = true;
        for (auto &waiter : std::views::values(state->pending_waiters))
            waiters.push_back(waiter);
        state->pending_waiters.clear();
        state->pending_requests.Clear();
        for (auto &session_state : std::views::values(state->sessions_by_id))
            sessions.push_back(session_state);
        state->sessions_by_id.clear();
        state->sessions_by_target_id.clear();
    }

    for (const auto &waiter : waiters) {
        auto waiter_state = waiter->data.Lock();
        waiter_state->failure = failure;
        waiter_state->done = true;
        waiter_state->cv.NotifyOne();
    }
    for (const auto &session_state : sessions) {
        auto session_data = session_state->data.Lock();
        session_data->failure = failure;
        session_data->closed = true;
        session_data->cv.NotifyAll();
    }
}

Expected<CdpEvent, CdpFailure>
CdpSession::WaitEvent(eng::Deadline deadline, const String &timeout_message)
{
    Invariant(client_ != nullptr, "cdp session is not attached"_t);
    Invariant(session_state_ != nullptr, "cdp session state is missing"_t);
    return client_->WaitForSessionEvent(session_state_, deadline, timeout_message);
}

std::vector<CdpEvent> CdpSession::DrainAvailableEvents()
{
    Invariant(client_ != nullptr, "cdp session is not attached"_t);
    Invariant(session_state_ != nullptr, "cdp session state is missing"_t);
    return client_->DrainSessionEvents(session_state_);
}

Expected<void, CdpFailure> CdpClient::WriteTraceLine(const json::Value &value)
{
    using enum CdpError;
    auto line = TRY(
        ws::json::StringifyBytes(value, CdpFailure{.code = kTraceWriteFailed, .detail = {}})
    );
    line.push_back('\n');
    TRY(WriteTraceBytes(fs_task_processor_, trace_file_, line));
    return {};
}

void CdpClient::WriteTraceLineBestEffort(const json::Value &value)
{
    const auto written = WriteTraceLine(value);
    if (written)
        return;
    LOG_WARNING() << std::format(
        "Suppressing CDP trace write failure for {} (code={})", trace_path_,
        us::utils::UnderlyingValue(written.Error().code)
    );
}

void CdpClient::TraceCommand(i64 id, const String &method, const std::optional<String> &session_id)
{
    json::ValueBuilder entry;
    entry["ts"] = CurrentTraceTimestamp();
    entry["direction"] = "out";
    entry["kind"] = "command";
    entry["id"] = Raw(id);
    entry["method"] = method.ToBytes();
    if (session_id)
        entry["sessionId"] = session_id->ToBytes();
    WriteTraceLineBestEffort(entry.ExtractValue());
}

void CdpClient::TraceResponse(
    i64 id, const CdpPendingRequest *request, const std::optional<String> &error
)
{
    json::ValueBuilder entry;
    entry["ts"] = CurrentTraceTimestamp();
    entry["direction"] = "in";
    entry["kind"] = error ? "error" : "response";
    entry["id"] = Raw(id);
    if (request) {
        entry["method"] = request->method.ToBytes();
        if (request->session_id)
            entry["sessionId"] = request->session_id->ToBytes();
    }
    if (error)
        entry["error"] = error->ToBytes();
    WriteTraceLineBestEffort(entry.ExtractValue());
}

void CdpClient::TraceEvent(const String &method, const std::optional<String> &session_id)
{
    json::ValueBuilder entry;
    entry["ts"] = CurrentTraceTimestamp();
    entry["direction"] = "in";
    entry["kind"] = "event";
    entry["method"] = method.ToBytes();
    if (session_id)
        entry["sessionId"] = session_id->ToBytes();
    WriteTraceLineBestEffort(entry.ExtractValue());
}

void CdpClient::TraceClose(const String &direction, int close_code)
{
    json::ValueBuilder entry;
    entry["ts"] = CurrentTraceTimestamp();
    entry["direction"] = direction.ToBytes();
    entry["kind"] = "close";
    entry["closeCode"] = close_code;
    WriteTraceLineBestEffort(entry.ExtractValue());
}

void CdpClient::TraceTransportError(const String &operation, const String &error)
{
    json::ValueBuilder entry;
    entry["ts"] = CurrentTraceTimestamp();
    entry["kind"] = "transport_error";
    entry["operation"] = operation.ToBytes();
    entry["error"] = error.ToBytes();
    WriteTraceLineBestEffort(entry.ExtractValue());
}

} // namespace ws::crawler
