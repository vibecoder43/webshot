#pragma once

#include "crawler/cdp_request_tracker.hpp"
#include "expected.hpp"
#include "grab_value.hpp"
#include "integers.hpp"
#include "invariant.hpp"
#include "json.hpp"
#include "schema/cdp.hpp"
#include "text.hpp"
#include "try.hpp"

#include <chrono>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <userver/concurrent/variable.hpp>
#include <userver/engine/condition_variable.hpp>
#include <userver/engine/deadline.hpp>
#include <userver/engine/task/task.hpp>
#include <userver/engine/task/task_processor_fwd.hpp>
#include <userver/formats/json/value.hpp>
#include <userver/formats/json/value_builder.hpp>
#include <userver/fs/blocking/file_descriptor.hpp>
#include <userver/utils/assert.hpp>
#include <userver/websocket/connection.hpp>

namespace ws::crawler {

namespace us = userver;
namespace eng = us::engine;
namespace json = us::formats::json;
using text::literals::operator""_t;
using ws::Expected;

struct [[nodiscard]] CdpEvent {
    String method;
    std::optional<dto::CdpEventMessage::Params> params;
    std::optional<String> session_id;
};

enum class CdpError {
    kTraceFileOpenFailed,
    kTraceWriteFailed,
    kSocketConnectFailed,
    kHandshakeTimeout,
    kHandshakeResponseTooLarge,
    kHandshakeUnexpectedEof,
    kHandshakeMalformedResponse,
    kHandshakeRejected,
    kHandshakeMissingHeader,
    kSocketClosed,
    kTransport,
    kTimeout,
    kJsonParseFailed,
    kProtocol,
    kCommandFailed,
};

struct [[nodiscard]] CdpFailure final {
    CdpError code;
    std::optional<String> detail;
};

struct CdpSessionState;

class [[nodiscard]] CdpClient final {
public:
    [[nodiscard]] static Expected<std::unique_ptr<CdpClient>, CdpFailure> Connect(
        std::string socket_path, String websocket_path, std::string trace_path,
        eng::TaskProcessor &fs_task_processor, eng::Deadline overall_deadline,
        std::chrono::milliseconds handshake_timeout, std::chrono::milliseconds command_timeout,
        i64 max_remote_payload_bytes
    );

    ~CdpClient() noexcept;

    CdpClient(const CdpClient &) = delete;
    CdpClient(CdpClient &&) = delete;
    CdpClient &operator=(const CdpClient &) = delete;
    CdpClient &operator=(CdpClient &&) = delete;

    template <typename T> [[nodiscard]] Expected<T, CdpFailure> Send(const String &method)
    {
        return ws::json::As<T>(
            TRY(SendRaw(method, json::Value{}, {})),
            CdpFailure{.code = CdpError::kProtocol, .detail = {}}
        );
    }

    template <typename T, typename Params>
    [[nodiscard]] Expected<T, CdpFailure> Send(const String &method, const Params &params)
    {
        return Send<T>(method, params, {});
    }

    template <typename T>
    [[nodiscard]] Expected<T, CdpFailure> Send(const String &method, const String &session_id)
    {
        return Send<T>(method, std::optional{session_id});
    }

    template <typename T>
    [[nodiscard]] Expected<T, CdpFailure>
    Send(const String &method, const std::optional<String> &session_id)
    {
        return ws::json::As<T>(
            TRY(SendRaw(method, json::Value{}, session_id)),
            CdpFailure{.code = CdpError::kProtocol, .detail = {}}
        );
    }

    template <typename T, typename Params>
    [[nodiscard]] Expected<T, CdpFailure>
    Send(const String &method, const Params &params, const std::optional<String> &session_id)
    {
        const auto params_value = TRY(
            ws::json::ValueOf(params, CdpFailure{.code = CdpError::kProtocol, .detail = {}})
        );
        return ws::json::As<T>(
            TRY(SendRaw(method, params_value, session_id)),
            CdpFailure{.code = CdpError::kProtocol, .detail = {}}
        );
    }

    [[nodiscard]] Expected<std::unique_ptr<class CdpSession>, CdpFailure>
    CreateSession(String session_id, String target_id);

    Expected<void, CdpFailure> Close();

private:
    struct PendingCommandWaiter;
    struct SharedState {
        std::unordered_map<i64, std::shared_ptr<PendingCommandWaiter>> pending_waiters;
        std::unordered_map<String, std::shared_ptr<CdpSessionState>> sessions_by_id;
        std::unordered_map<String, std::shared_ptr<CdpSessionState>> sessions_by_target_id;
        CdpRequestTracker pending_requests;
        i64 next_request_id{1_i64};
        std::optional<CdpFailure> terminal_failure;
        bool closing{false};
        bool closed{false};
    };
    struct SendState final {};

    CdpClient(
        std::string socket_path, String websocket_path,
        std::shared_ptr<us::websocket::WebSocketConnection> connection, std::string trace_path,
        us::fs::blocking::FileDescriptor trace_file, eng::TaskProcessor &fs_task_processor,
        eng::Deadline overall_deadline, std::chrono::milliseconds command_timeout
    );

    friend class CdpSession;

    void StartReaderTask();
    [[nodiscard]] Expected<json::Value, CdpFailure> SendRaw(
        const String &method, const json::Value &params, const std::optional<String> &session_id
    );
    void ReaderLoop();
    Expected<void, CdpFailure> HandleMessage(const std::string &payload);
    [[nodiscard]] Expected<CdpEvent, CdpFailure> WaitForSessionEvent(
        const std::shared_ptr<CdpSessionState> &session_state, eng::Deadline deadline,
        const String &timeout_message
    );
    [[nodiscard]] std::vector<CdpEvent>
    DrainSessionEvents(const std::shared_ptr<CdpSessionState> &session_state);
    void UnregisterSession(
        const String &session_id, const String &target_id,
        const std::shared_ptr<CdpSessionState> &session_state
    ) noexcept;
    void CloseQuietly() noexcept;
    void StopReaderTask() noexcept;
    Expected<void, CdpFailure> WriteTraceLine(const json::Value &value);
    void WriteTraceLineBestEffort(const json::Value &value);
    void TraceCommand(i64 id, const String &method, const std::optional<String> &session_id);
    void
    TraceResponse(i64 id, const CdpPendingRequest *request, const std::optional<String> &error);
    void TraceEvent(const String &method, const std::optional<String> &session_id);
    void TraceClose(const String &direction, int close_code);
    void TraceTransportError(const String &operation, const String &error);
    void FailTerminal(CdpFailure failure);

    std::string socket_path_;
    String websocket_path_;
    std::shared_ptr<us::websocket::WebSocketConnection> connection_;
    us::concurrent::Variable<SharedState> shared_state_;
    us::concurrent::Variable<SendState> send_state_;
    std::string trace_path_;
    us::fs::blocking::FileDescriptor trace_file_;
    eng::TaskProcessor &fs_task_processor_;
    eng::Deadline overall_deadline_;
    std::chrono::milliseconds command_timeout_;
    eng::Task reader_task_;
};

class [[nodiscard]] CdpSession final {
public:
    ~CdpSession();

    CdpSession(const CdpSession &) = delete;
    CdpSession(CdpSession &&) = delete;
    CdpSession &operator=(const CdpSession &) = delete;
    CdpSession &operator=(CdpSession &&) = delete;

    template <typename T> [[nodiscard]] Expected<T, CdpFailure> Send(const String &method)
    {
        Invariant(client_ != nullptr, "cdp session is not attached"_t);
        return client_->Send<T>(method, session_id_);
    }

    template <typename T, typename Params>
    [[nodiscard]] Expected<T, CdpFailure> Send(const String &method, const Params &params)
    {
        Invariant(client_ != nullptr, "cdp session is not attached"_t);
        return client_->Send<T>(method, params, session_id_);
    }

    template <typename Params>
    [[nodiscard]] Expected<void, CdpFailure> SendVoid(const String &method, const Params &params)
    {
        Invariant(client_ != nullptr, "cdp session is not attached"_t);
        TRY(client_->Send<dto::CdpEmptyObject>(method, params, session_id_));
        return {};
    }

    [[nodiscard]] Expected<void, CdpFailure> SendVoid(const String &method)
    {
        Invariant(client_ != nullptr, "cdp session is not attached"_t);
        TRY(client_->Send<dto::CdpEmptyObject>(method, session_id_));
        return {};
    }

    [[nodiscard]] Expected<CdpEvent, CdpFailure>
    WaitEvent(eng::Deadline deadline, const String &timeout_message);
    [[nodiscard]] std::vector<CdpEvent> DrainAvailableEvents();
    [[nodiscard]] const String &SessionId() const noexcept { return session_id_; }
    [[nodiscard]] const String &TargetId() const noexcept { return target_id_; }

private:
    CdpSession(
        CdpClient &client_in, String session_id_in, String target_id_in,
        std::shared_ptr<CdpSessionState> session_state_in
    )
        : client_(&client_in), session_id_(std::move(session_id_in)),
          target_id_(std::move(target_id_in)), session_state_(std::move(session_state_in))
    {
    }

    friend class CdpClient;

    CdpClient *client_{nullptr};
    String session_id_;
    String target_id_;
    std::shared_ptr<CdpSessionState> session_state_;
};

[[nodiscard]] String DescribeCdpFailure(const String &action, const CdpFailure &failure);

} // namespace ws::crawler
