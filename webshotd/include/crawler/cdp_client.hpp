#pragma once

#include "integers.hpp"
#include "schema/cdp.hpp"
#include "text.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <userver/engine/deadline.hpp>
#include <userver/formats/json/value.hpp>
#include <userver/formats/json/value_builder.hpp>
#include <userver/fs/blocking/file_descriptor.hpp>
#include <userver/websocket/connection.hpp>

namespace us = userver;

namespace v1::crawler {

struct [[nodiscard]] CdpEvent {
    String method;
    std::optional<dto::CdpEventMessage::Params> params;
    std::optional<String> sessionId;
};

class [[nodiscard]] CdpClient final {
public:
    using ListenerId = int64_t;
    using EventListener = std::function<void(CdpEvent)>;

    CdpClient(std::string socketPath, String websocketPath, std::string tracePathIn);

    ~CdpClient() noexcept;

    CdpClient(const CdpClient &) = delete;
    CdpClient(CdpClient &&) = delete;
    CdpClient &operator=(const CdpClient &) = delete;
    CdpClient &operator=(CdpClient &&) = delete;

    template <typename T> [[nodiscard]] T send(std::string_view method)
    {
        const auto value = sendRaw(method, us::formats::json::Value{}, {});
        return value.As<T>();
    }

    template <typename T, typename Params>
    [[nodiscard]] T send(std::string_view method, const Params &params)
    {
        return send<T>(method, params, {});
    }

    template <typename T> [[nodiscard]] T send(std::string_view method, const String &sessionId)
    {
        return send<T>(method, std::make_optional(sessionId));
    }

    template <typename T>
    [[nodiscard]] T send(std::string_view method, const std::optional<String> &sessionId)
    {
        const auto value = sendRaw(method, us::formats::json::Value{}, sessionId);
        return value.As<T>();
    }

    template <typename T, typename Params>
    [[nodiscard]] T
    send(std::string_view method, const Params &params, const std::optional<String> &sessionId)
    {
        const auto value = sendRaw(
            method, us::formats::json::ValueBuilder(params).ExtractValue(), sessionId
        );
        return value.As<T>();
    }

    ListenerId addListener(EventListener listener);
    void removeListener(ListenerId id);

    bool tryPumpOnce();
    void waitUntil(
        const std::function<bool()> &predicate, us::engine::Deadline deadline,
        std::string_view timeoutMessage
    );

    void close();

private:
    struct [[nodiscard]] PendingRequestTrace {
        std::string method;
        std::optional<String> sessionId;
    };

    [[nodiscard]] us::formats::json::Value sendRaw(
        std::string_view method, const us::formats::json::Value &params,
        const std::optional<String> &sessionId
    );
    void pumpOne();
    void dispatchEvent(CdpEvent event);
    void handleMessage(const std::string &payload);
    void closeNoThrow() noexcept;
    [[nodiscard]] std::string makeEndpointPath() const;
    void writeTraceLine(const us::formats::json::Value &value);
    void traceCommand(int64_t id, std::string_view method, const std::optional<String> &sessionId);
    void traceResponse(
        int64_t id, const PendingRequestTrace *request, const std::optional<String> &error
    );
    void traceEvent(std::string_view method, const std::optional<String> &sessionId);
    void traceClose(std::string_view direction, int closeCode);
    void traceTransportError(std::string_view operation, std::string_view error);

    std::string socketPath;
    String websocketPath;
    std::shared_ptr<us::websocket::WebSocketConnection> connection;
    std::unordered_map<ListenerId, EventListener> listeners;
    std::unordered_map<int64_t, us::formats::json::Value> pendingResults;
    std::unordered_map<int64_t, PendingRequestTrace> pendingRequests;
    std::string tracePath;
    us::fs::blocking::FileDescriptor traceFile;
    ListenerId nextListenerId{1};
    int64_t nextRequestId{1};
    bool closed{false};
};

} // namespace v1::crawler
