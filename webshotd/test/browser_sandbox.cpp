#include "crawler/browser_sandbox.hpp"

#include <algorithm>
#include <string>

#include <userver/utest/utest.hpp>

namespace {

UTEST(BrowserSandbox, DoesNotDisableCertificateVerification)
{
    const auto args = v1::crawler::buildChromiumArgs("/tmp/profile", "/tmp/netlog.json");
    const auto ignoreCertErrors = std::find(
        std::begin(args), std::end(args), std::string{"--ignore-certificate-errors"}
    );
    EXPECT_EQ(ignoreCertErrors, std::end(args));
}

} // namespace
