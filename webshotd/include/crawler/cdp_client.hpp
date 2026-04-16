#pragma once

#include "crawler/cdp_request_tracker.hpp"
#include "expected.hpp"
#include "integers.hpp"
#include "schema/cdp.hpp"
#include "text.hpp"
#include "userver_namespaces.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <userver/engine/deadline.hpp>
#include <userver/engine/sleep.hpp>
#include <userver/formats/json/value.hpp>
#include <userver/formats/json/value_builder.hpp>
#include <userver/fs/blocking/file_descriptor.hpp>
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

class [[nodiscard]] CdpClient final {
public:
    using ListenerId = i64;
    using EventListener = std::function<void(CdpEvent)>;

    [[nodiscard]] static Expected<std::unique_ptr<CdpClient>, CdpFailure> connect(
        std::string socketPath, String websocketPath, std::string tracePath,
        us::engine::Deadline overallDeadline, std::chrono::seconds handshakeTimeout,
        std::chrono::seconds commandTimeout, std::chrono::milliseconds waitPollInterval,
        i64 maxRemotePayloadBytes
    );

    ~CdpClient() noexcept;

    CdpClient(const CdpClient &) = delete;
    CdpClient(CdpClient &&) = delete;
    CdpClient &operator=(const CdpClient &) = delete;
    CdpClient &operator=(CdpClient &&) = delete;

    template <typename T> [[nodiscard]] Expected<T, CdpFailure> send(std::string_view method)
    {
        auto value = sendRaw(method, us::formats::json::Value{}, {});
        if (!value)
            return std::unexpected(value.error());
        try {
            return (*value).As<T>();
        } catch (const us::formats::json::Exception &) {
            return std::unexpected(CdpFailure{.code = CdpError::kProtocol, .detail = {}});
        }
    }

    template <typename T, typename Params>
    [[nodiscard]] Expected<T, CdpFailure> send(std::string_view method, const Params &params)
    {
        return send<T>(method, params, {});
    }

    template <typename T>
    [[nodiscard]] Expected<T, CdpFailure> send(std::string_view method, const String &sessionId)
    {
        return send<T>(method, std::optional{sessionId});
    }

    template <typename T>
    [[nodiscard]] Expected<T, CdpFailure>
    send(std::string_view method, const std::optional<String> &sessionId)
    {
        auto value = sendRaw(method, us::formats::json::Value{}, sessionId);
        if (!value)
            return std::unexpected(value.error());
        try {
            return (*value).As<T>();
        } catch (const us::formats::json::Exception &) {
            return std::unexpected(CdpFailure{.code = CdpError::kProtocol, .detail = {}});
        }
    }

    template <typename T, typename Params>
    [[nodiscard]] Expected<T, CdpFailure>
    send(std::string_view method, const Params &params, const std::optional<String> &sessionId)
    {
        auto value = sendRaw(
            method, us::formats::json::ValueBuilder(params).ExtractValue(), sessionId
        );
        if (!value)
            return std::unexpected(value.error());
        try {
            return (*value).As<T>();
        } catch (const us::formats::json::Exception &) {
            return std::unexpected(CdpFailure{.code = CdpError::kProtocol, .detail = {}});
        }
    }

    template <typename Params>
    [[nodiscard]] Expected<void, CdpFailure> sendNoWait(
        std::string_view method, const Params &params, const std::optional<String> &sessionId
    )
    {
        return sendRawNoWait(
            method, us::formats::json::ValueBuilder(params).ExtractValue(), sessionId
        );
    }

    template <typename Params>
    [[nodiscard]] Expected<void, CdpFailure>
    sendNoWait(std::string_view method, const Params &params)
    {
        return sendNoWait(method, params, {});
    }

    ListenerId addListener(EventListener listener);
    void removeListener(ListenerId id);

    Expected<bool, CdpFailure> tryPumpOnce();
    template <typename Predicate>
    Expected<void, CdpFailure>
    waitUntil(Predicate &&predicate, us::engine::Deadline deadline, std::string_view timeoutMessage)
    {
        using enum CdpError;
        while (!std::invoke(predicate)) {
            auto pumped = tryPumpOnce();
            if (!pumped)
                return std::unexpected(pumped.error());
            if (*pumped)
                continue;
            if (deadline.IsReachable() && deadline.IsReached())
                return std::unexpected(
                    CdpFailure{
                        .code = kTimeout,
                        .detail = std::optional<String>{String::fromBytes(timeoutMessage).expect()},
                    }
                );
            us::engine::SleepFor(waitPollInterval);
        }
        return {};
    }

    Expected<void, CdpFailure> close();

private:
    CdpClient(
        std::string socketPath, String websocketPath,
        std::shared_ptr<us::websocket::WebSocketConnection> connection, std::string tracePath,
        us::fs::blocking::FileDescriptor traceFile, us::engine::Deadline overallDeadline,
        std::chrono::seconds commandTimeout, std::chrono::milliseconds waitPollInterval
    );

    [[nodiscard]] Expected<us::formats::json::Value, CdpFailure> sendRaw(
        std::string_view method, const us::formats::json::Value &params,
        const std::optional<String> &sessionId
    );
    [[nodiscard]] Expected<void, CdpFailure> sendRawNoWait(
        std::string_view method, const us::formats::json::Value &params,
        const std::optional<String> &sessionId
    );
    Expected<void, CdpFailure> pumpOne();
    void dispatchEvent(CdpEvent event);
    Expected<void, CdpFailure> handleMessage(const std::string &payload);
    void closeQuietly() noexcept;
    [[nodiscard]] std::string makeEndpointPath() const;
    Expected<void, CdpFailure> writeTraceLine(const us::formats::json::Value &value);
    void traceCommand(i64 id, std::string_view method, const std::optional<String> &sessionId);
    void
    traceResponse(i64 id, const CdpPendingRequest *request, const std::optional<String> &error);
    void traceEvent(std::string_view method, const std::optional<String> &sessionId);
    void traceClose(std::string_view direction, int closeCode);
    void traceTransportError(std::string_view operation, std::string_view error);

    std::string socketPath;
    String websocketPath;
    std::shared_ptr<us::websocket::WebSocketConnection> connection;
    std::unordered_map<ListenerId, EventListener> listeners;
    std::unordered_map<i64, us::formats::json::Value> pendingResults;
    CdpRequestTracker pendingRequests;
    std::string tracePath;
    us::fs::blocking::FileDescriptor traceFile;
    us::engine::Deadline overallDeadline;
    std::chrono::seconds commandTimeout;
    std::chrono::milliseconds waitPollInterval;
    ListenerId nextListenerId{1_i64};
    i64 nextRequestId{1_i64};
    bool closed{false};
};

} // namespace v1::crawler
