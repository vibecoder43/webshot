#include <string>

#include <userver/clients/http/client.hpp>
#include <userver/clients/http/request.hpp>
#include <userver/clients/http/response.hpp>
#include <userver/utest/http_client.hpp>
#include <userver/utest/http_server_mock.hpp>
#include <userver/utest/utest.hpp>

namespace {

userver::utest::HttpServerMock::HttpResponse
handleRequest(const userver::utest::HttpServerMock::HttpRequest &req)
{
    if (req.path == "/bad")
        return {400, {}, R"({"error":"invalid request body"})"};
    return {200, {}, "ok"};
}

} // namespace

UTEST(HttpHandlersSimpleServer, ReturnsBadRequest)
{
    userver::utest::HttpServerMock mock(handleRequest);
    auto client = userver::utest::CreateHttpClient();

    const auto url = mock.GetBaseUrl() + "/bad";
    auto resp = client->CreateRequest()
                    .post(url, "broken")
                    .retry(1)
                    .timeout(std::chrono::milliseconds{1000})
                    .perform();

    EXPECT_EQ(resp->status_code(), userver::clients::http::Status::BadRequest);
    EXPECT_EQ(resp->body(), std::string(R"({"error":"invalid request body"})"));
}
