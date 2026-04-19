#include <chrono>
#include <cstdio>
#include <memory>
#include <string_view>

#include <userver/engine/run_standalone.hpp>
#include <userver/utest/http_client.hpp>
#include <userver/utils/datetime.hpp>

#include "s3/s3_v4_client.hpp"
#include "text.hpp"

using namespace std::chrono_literals;
using v1::s3v4::AccessKeyId;
using v1::s3v4::S3Credentials;
using v1::s3v4::S3V4Client;
using v1::s3v4::S3V4Config;
using v1::s3v4::SecretAccessKey;
using namespace text::literals;

namespace {

S3V4Config makeConfig()
{
    auto cfg = S3V4Config{};
    cfg.endpoint = "https://examplebucket.s3.amazonaws.com"_t;
    cfg.region = "us-east-1"_t;
    cfg.timeout = 1000ms;
    cfg.virtualHosted = false;
    return cfg;
}

S3Credentials makeCreds()
{
    auto creds = S3Credentials{};
    creds.accessKeyId = AccessKeyId{"AKIAIOSFODNN7EXAMPLE"_t};
    creds.secretAccessKey = SecretAccessKey{"wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY"_t};
    return creds;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <scenario>\n", argv[0]);
        return 64;
    }

    const auto scenario = std::string_view{argv[1]};
    if (scenario == "virtual-host-requires-bucket") {
        userver::engine::RunStandalone([] {
            auto httpClient = userver::utest::CreateHttpClient();
            auto client = std::make_shared<S3V4Client>(
                *httpClient, makeConfig(), makeCreds(), String{}
            );
            const auto expiresAt = userver::utils::datetime::Now() + 60s;
            static_cast<void>(
                client->GenerateDownloadUrlVirtualHostAddressing("obj", expiresAt, "https")
            );
        });
        return 0;
    }

    std::fprintf(stderr, "unknown scenario: %s\n", argv[1]);
    return 64;
}
