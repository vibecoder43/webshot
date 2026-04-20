#pragma once

#include "crawler/cdp_request_tracker.hpp"
#include "expected.hpp"
#include "grab_value.hpp"
#include "integers.hpp"
#include "schema/cdp.hpp"
#include "text.hpp"
#include "try.hpp"
#include "userver_expected.hpp"
#include "userver_namespaces.hpp"

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
#include <userver/formats/json/value.hpp>
#include <userver/formats/json/value_builder.hpp>
#include <userver/fs/blocking/file_descriptor.hpp>
#include <userver/utils/assert.hpp>
#include <userver/websocket/connection.hpp>

namespace v1::crawler {

using v1::Expected;

struct [[nodiscard]] CdpEvent {
    String method;
    std::optional<dto::CdpEventMessage::Params> params;
    std::optional<String> sessionId;
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
    [[nodiscard]] static Expected<std::unique_ptr<CdpClient>, CdpFailure> connect(
        std::string socketPath, String websocketPath, std::string tracePath,
        eng::Deadline overallDeadline, std::chrono::milliseconds handshakeTimeout,
        std::chrono::milliseconds commandTimeout, i64 maxRemotePayloadBytes
    );

    ~CdpClient() noexcept;

    CdpClient(const CdpClient &) = delete;
    CdpClient(CdpClient &&) = delete;
    CdpClient &operator=(const CdpClient &) = delete;
    CdpClient &operator=(CdpClient &&) = delete;

    template <typename T> [[nodiscard]] Expected<T, CdpFailure> send(const String &method)
    {
        return exu::json::as<T>(
            TRY(sendRaw(method, json::Value{}, {})),
            CdpFailure{.code = CdpError::kProtocol, .detail = {}}
        );
    }

    template <typename T, typename Params>
    [[nodiscard]] Expected<T, CdpFailure> send(const String &method, const Params &params)
    {
        return send<T>(method, params, {});
    }

    template <typename T>
    [[nodiscard]] Expected<T, CdpFailure> send(const String &method, const String &sessionId)
    {
        return send<T>(method, std::optional{sessionId});
    }

    template <typename T>
    [[nodiscard]] Expected<T, CdpFailure>
    send(const String &method, const std::optional<String> &sessionId)
    {
        return exu::json::as<T>(
            TRY(sendRaw(method, json::Value{}, sessionId)),
            CdpFailure{.code = CdpError::kProtocol, .detail = {}}
        );
    }

    template <typename T, typename Params>
    [[nodiscard]] Expected<T, CdpFailure>
    send(const String &method, const Params &params, const std::optional<String> &sessionId)
    {
        const auto paramsValue = TRY(
            exu::json::valueOf(params, CdpFailure{.code = CdpError::kProtocol, .detail = {}})
        );
        return exu::json::as<T>(
            TRY(sendRaw(method, paramsValue, sessionId)),
            CdpFailure{.code = CdpError::kProtocol, .detail = {}}
        );
    }

    [[nodiscard]] Expected<std::unique_ptr<class CdpSession>, CdpFailure>
    createSession(String sessionId, String targetId);

    Expected<void, CdpFailure> close();

private:
    struct PendingCommandWaiter;
    struct SharedState {
        std::unordered_map<i64, std::shared_ptr<PendingCommandWaiter>> pendingWaiters;
        std::unordered_map<String, std::shared_ptr<CdpSessionState>> sessionsById;
        std::unordered_map<String, std::shared_ptr<CdpSessionState>> sessionsByTargetId;
        CdpRequestTracker pendingRequests;
        i64 nextRequestId{1_i64};
        std::optional<CdpFailure> terminalFailure;
        bool closing{false};
        bool closed{false};
    };
    struct SendState final {};

    CdpClient(
        std::string socketPath, String websocketPath,
        std::shared_ptr<us::websocket::WebSocketConnection> connection, std::string tracePath,
        us::fs::blocking::FileDescriptor traceFile, eng::Deadline overallDeadline,
        std::chrono::milliseconds commandTimeout
    );

    friend class CdpSession;

    void startReaderTask();
    [[nodiscard]] Expected<json::Value, CdpFailure> sendRaw(
        const String &method, const json::Value &params, const std::optional<String> &sessionId
    );
    void readerLoop();
    Expected<void, CdpFailure> handleMessage(const std::string &payload);
    [[nodiscard]] Expected<CdpEvent, CdpFailure> waitForSessionEvent(
        const std::shared_ptr<CdpSessionState> &sessionState, eng::Deadline deadline,
        const String &timeoutMessage
    );
    [[nodiscard]] std::vector<CdpEvent>
    drainSessionEvents(const std::shared_ptr<CdpSessionState> &sessionState);
    void unregisterSession(
        const String &sessionId, const String &targetId,
        const std::shared_ptr<CdpSessionState> &sessionState
    ) noexcept;
    void closeQuietly() noexcept;
    Expected<void, CdpFailure> writeTraceLine(const json::Value &value);
    void traceCommand(i64 id, const String &method, const std::optional<String> &sessionId);
    void
    traceResponse(i64 id, const CdpPendingRequest *request, const std::optional<String> &error);
    void traceEvent(const String &method, const std::optional<String> &sessionId);
    void traceClose(const String &direction, int closeCode);
    void traceTransportError(const String &operation, const String &error);
    void failTerminal(CdpFailure failure);

    std::string socketPath;
    String websocketPath;
    std::shared_ptr<us::websocket::WebSocketConnection> connection;
    us::concurrent::Variable<SharedState> sharedState;
    us::concurrent::Variable<SendState> sendState;
    std::string tracePath;
    us::fs::blocking::FileDescriptor traceFile;
    eng::Deadline overallDeadline;
    std::chrono::milliseconds commandTimeout;
    eng::Task readerTask;
};

class [[nodiscard]] CdpSession final {
public:
    ~CdpSession();

    CdpSession(const CdpSession &) = delete;
    CdpSession(CdpSession &&) = delete;
    CdpSession &operator=(const CdpSession &) = delete;
    CdpSession &operator=(CdpSession &&) = delete;

    template <typename T> [[nodiscard]] Expected<T, CdpFailure> send(const String &method)
    {
        invariant(client != nullptr, "cdp session is not attached");
        return client->send<T>(method, sessionIdValue);
    }

    template <typename T, typename Params>
    [[nodiscard]] Expected<T, CdpFailure> send(const String &method, const Params &params)
    {
        invariant(client != nullptr, "cdp session is not attached");
        return client->send<T>(method, params, sessionIdValue);
    }

    template <typename Params>
    [[nodiscard]] Expected<void, CdpFailure> sendVoid(const String &method, const Params &params)
    {
        invariant(client != nullptr, "cdp session is not attached");
        TRY(client->send<dto::CdpEmptyObject>(method, params, sessionIdValue));
        return {};
    }

    [[nodiscard]] Expected<void, CdpFailure> sendVoid(const String &method)
    {
        invariant(client != nullptr, "cdp session is not attached");
        TRY(client->send<dto::CdpEmptyObject>(method, sessionIdValue));
        return {};
    }

    [[nodiscard]] Expected<CdpEvent, CdpFailure>
    waitEvent(eng::Deadline deadline, const String &timeoutMessage);
    [[nodiscard]] std::vector<CdpEvent> drainAvailableEvents();
    [[nodiscard]] const String &sessionId() const noexcept { return sessionIdValue; }
    [[nodiscard]] const String &targetId() const noexcept { return targetIdValue; }

private:
    CdpSession(
        CdpClient &clientIn, String sessionIdIn, String targetIdIn,
        std::shared_ptr<CdpSessionState> sessionStateIn
    )
        : client(&clientIn), sessionIdValue(std::move(sessionIdIn)),
          targetIdValue(std::move(targetIdIn)), sessionState(std::move(sessionStateIn))
    {
    }

    friend class CdpClient;

    CdpClient *client{nullptr};
    String sessionIdValue;
    String targetIdValue;
    std::shared_ptr<CdpSessionState> sessionState;
};

[[nodiscard]] String describeCdpFailure(const String &action, const CdpFailure &failure);

} // namespace v1::crawler
