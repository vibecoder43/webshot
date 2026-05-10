#include "storage_url.hpp"

#include <userver/utest/utest.hpp>

#include <boost/uuid/string_generator.hpp>

using namespace ws;
using namespace text::literals;
using enum Mode;
using enum StorageUrlError;

namespace {

[[nodiscard]] ws::uuid::Uuid SampleUuid()
{
    return boost::uuids::string_generator{}("123e4567-e89b-12d3-a456-426614174000");
}

} // namespace

UTEST(StorageUrl, ExternalModePreservesConfiguredBase)
{
    const auto url = MakeCaptureDownloadUrl(
        SampleUuid(), kExternal, "https://storage.example/webshot"_t, "client.example:8080"_t, {},
        {}, false
    );

    ASSERT_TRUE(url);
    EXPECT_EQ(
        url->Href(), "https://storage.example/webshot/123e4567-e89b-12d3-a456-426614174000.wacz"_t
    );
}

UTEST(StorageUrl, LocalModeUsesRequestHostnameAndConfiguredPort)
{
    const auto url = MakeCaptureDownloadUrl(
        SampleUuid(), kLocal, "http://127.0.0.1:8333/webshot"_t, "client.example:8080"_t, {}, {},
        false
    );

    ASSERT_TRUE(url);
    EXPECT_EQ(
        url->Href(),
        "http://client.example:8333/webshot/123e4567-e89b-12d3-a456-426614174000.wacz"_t
    );
}

UTEST(StorageUrl, LocalModeHandlesBracketedIpv6RequestHost)
{
    const auto url = MakeCaptureDownloadUrl(
        SampleUuid(), kLocal, "http://127.0.0.1:8333/webshot"_t, "[2001:db8::1]:8080"_t, {}, {},
        false
    );

    ASSERT_TRUE(url);
    EXPECT_EQ(
        url->Href(), "http://[2001:db8::1]:8333/webshot/123e4567-e89b-12d3-a456-426614174000.wacz"_t
    );
}

UTEST(StorageUrl, LocalModeRejectsMissingRequestHost)
{
    const auto url = MakeCaptureDownloadUrl(
        SampleUuid(), kLocal, "http://127.0.0.1:8333/webshot"_t, {}, {}, {}, false
    );

    ASSERT_FALSE(url);
    EXPECT_EQ(url.Error(), kMissingRequestHost);
}

UTEST(StorageUrl, LocalModeRejectsInvalidRequestHost)
{
    const auto url = MakeCaptureDownloadUrl(
        SampleUuid(), kLocal, "http://127.0.0.1:8333/webshot"_t, "client.example/path"_t, {}, {},
        false
    );

    ASSERT_FALSE(url);
    EXPECT_EQ(url.Error(), kInvalidRequestHost);
}
