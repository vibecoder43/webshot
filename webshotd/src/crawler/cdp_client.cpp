#include "crawler/cdp_client.hpp"

#include "schema/cdp.hpp"

#include <chrono>
#include <stdexcept>
#include <utility>

#include <userver/clients/http/websocket_response.hpp>
#include <userver/engine/sleep.hpp>
#include <userver/formats/json.hpp>
#include <userver/formats/json/value_builder.hpp>
#include <userver/utils/assert.hpp>
#include <userver/utils/datetime.hpp>
#include <userver/websocket/connection.hpp>

namespace us = userver;
namespace json = us::formats::json;
namespace chrono = std::chrono;

namespace v1::crawler {
using namespace text::literals;

namespace {

[[nodiscard]] String getErrorMessage(const json::Value &error)
{
    if (!error.IsObject())
        return "cdp command failed"_t;
    try {
        return String::fromBytesThrow(error.As<dto::CdpError>().message);
    } catch (const std::exception &) {
    }
    return "cdp command failed"_t;
}

} // namespace

CdpClient::CdpClient(
    us::clients::http::Client &httpClientIn, std::string socketPathIn, String websocketPathIn
)
    : httpClient(httpClientIn), socketPath(std::move(socketPathIn)),
      websocketPath(std::move(websocketPathIn))
{
    auto response = httpClient.CreateRequest()
                        .get(makeEndpointUrl())
                        .unix_socket_path(socketPath.c_str())
                        .follow_redirects(false)
                        .PerformWebSocketHandshake();
    connection = response.MakeWebSocketConnection();
}

CdpClient::~CdpClient() { close(); }

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
        request.params = params;
    if (sessionId)
        request.sessionId = std::string(sessionId->view());
    connection->SendText(json::ToString(json::ValueBuilder(request).ExtractValue()));

    waitUntil(
        [this, id]() { return pendingResults.contains(id); }, chrono::seconds(10),
        "timed out waiting for cdp response"
    );

    auto it = pendingResults.find(id);
    UASSERT(it != std::end(pendingResults));
    auto result = it->second;
    pendingResults.erase(it);
    return result;
}

CdpClient::ListenerId CdpClient::addListener(std::function<void(const CdpEvent &)> listener)
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
    if (!connection->TryRecv(message))
        return false;
    if (message.close_status)
        throw std::runtime_error("cdp socket closed");
    handleMessage(message.data);
    return true;
}

void CdpClient::waitUntil(
    const std::function<bool()> &predicate, std::chrono::milliseconds timeout,
    std::string_view timeoutMessage
)
{
    const auto deadline = us::utils::datetime::SteadyNow() + timeout;
    while (!predicate()) {
        if (tryPumpOnce())
            continue;
        if (us::utils::datetime::SteadyNow() >= deadline)
            throw std::runtime_error(std::string(timeoutMessage));
        us::engine::SleepFor(std::chrono::milliseconds(10));
    }
}

void CdpClient::close()
{
    if (closed)
        return;
    closed = true;
    if (connection)
        connection->Close(us::websocket::CloseStatus::kNormal);
}

void CdpClient::pumpOne()
{
    us::websocket::Message message;
    connection->Recv(message);
    if (message.close_status)
        throw std::runtime_error("cdp socket closed");
    handleMessage(message.data);
}

void CdpClient::handleMessage(const std::string &payload)
{
    const auto value = json::FromString(payload);
    const auto idValue = value["id"];
    if (!idValue.IsMissing()) {
        const auto id = idValue.As<int64_t>();
        const auto errorValue = value["error"];
        if (!errorValue.IsMissing())
            throw std::runtime_error(std::string(getErrorMessage(errorValue).view()));
        pendingResults.emplace(id, value["result"]);
        return;
    }

    if (value["method"].IsMissing())
        return;

    auto eventMessage = value.As<dto::CdpEventMessage>();
    CdpEvent event{
        String::fromBytesThrow(eventMessage.method),
        std::move(eventMessage.params),
        eventMessage.sessionId ? std::make_optional(String::fromBytesThrow(*eventMessage.sessionId))
                               : {},
    };
    for (const auto &[id, listener] : listeners) {
        static_cast<void>(id);
        listener(event);
    }
}

std::string CdpClient::makeEndpointUrl() const
{
    const auto path = websocketPath.view();
    if (path.empty())
        return "ws://localhost/";
    if (path.front() == '/')
        return "ws://localhost" + std::string(path);
    return "ws://localhost/" + std::string(path);
}

} // namespace v1::crawler
