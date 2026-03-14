#include <algorithm>
#include <string>

#include <userver/utest/utest.hpp>

#include "crawler/browser_sandbox.hpp"

using namespace v1::crawler;
using namespace text::literals;

UTEST(CrawlerBrowserSandbox, ParseGeometryAcceptsValidWidthAndHeight)
{
    const auto geometry = parseGeometry("1600x900"_t);

    EXPECT_EQ(geometry.width, 1600_i64);
    EXPECT_EQ(geometry.height, 900_i64);
}

UTEST(CrawlerBrowserSandbox, ParseGeometryRejectsMalformedInput)
{
    EXPECT_THROW(parseGeometry("bad-input"_t), std::runtime_error);
    EXPECT_THROW(parseGeometry(""_t), std::runtime_error);
    EXPECT_THROW(parseGeometry("0x900"_t), std::runtime_error);
}

UTEST(CrawlerBrowserSandbox, BuildChromiumArgsUsesDefaultGeometryWhenUnset)
{
    const auto args = buildChromiumArgs({
        .browserBin = "chromium",
        .userDataDir = "/tmp/user-data",
        .proxyUpstreamSocket = "/tmp/proxy.sock",
        .cdpSocket = "/tmp/cdp.sock",
        .netlogPath = "/tmp/netlog.json",
        .geometry = {},
    });

    EXPECT_NE(
        std::find(std::begin(args), std::end(args), std::string("--window-size=1600,900")),
        std::end(args)
    );
}
