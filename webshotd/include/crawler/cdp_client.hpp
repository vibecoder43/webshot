#pragma once

#include "integers.hpp"
#include "text.hpp"

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <userver/clients/http/client.hpp>
#include <userver/formats/json/value.hpp>
#include <userver/formats/json/value_builder.hpp>
#include <userver/websocket/connection.hpp>

namespace us = userver;

namespace v1::crawler {

struct [[nodiscard]] CdpEvent {
    String method;
    std::optional<us::formats::json::Value> params;
    std::optional<String> sessionId;
};

class [[nodiscard]] CdpClient final {
public:
    using ListenerId = int64_t;

    CdpClient(us::clients::http::Client &httpClient, std::string socketPath, String websocketPath);

    ~CdpClient();

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

    ListenerId addListener(std::function<void(const CdpEvent &)> listener);
    void removeListener(ListenerId id);

    bool tryPumpOnce();
    void waitUntil(
        const std::function<bool()> &predicate, std::chrono::milliseconds timeout,
        std::string_view timeoutMessage
    );

    void close();

private:
    [[nodiscard]] us::formats::json::Value sendRaw(
        std::string_view method, const us::formats::json::Value &params,
        const std::optional<String> &sessionId
    );
    void pumpOne();
    void handleMessage(const std::string &payload);
    [[nodiscard]] std::string makeEndpointUrl() const;

    us::clients::http::Client &httpClient;
    std::string socketPath;
    String websocketPath;
    std::shared_ptr<us::websocket::WebSocketConnection> connection;
    std::unordered_map<ListenerId, std::function<void(const CdpEvent &)>> listeners;
    std::unordered_map<int64_t, us::formats::json::Value> pendingResults;
    ListenerId nextListenerId{1};
    int64_t nextRequestId{1};
    bool closed{false};
};

} // namespace v1::crawler
