#include <string>

#include <userver/utest/utest.hpp>

#include "host_policy.hpp"
#include "text.hpp"

using v1::HostPolicy::hasSpecialTldSuffix;
using v1::HostPolicy::isBareName;
using v1::HostPolicy::isDeniedHostname;
using namespace text::literals;

UTEST(HostPolicy, BareNameDetection)
{
    EXPECT_TRUE(isBareName("localhost"_t));
    EXPECT_TRUE(isBareName("printer"_t));
    EXPECT_FALSE(isBareName("example.com"_t));
}

UTEST(HostPolicy, DeniedHostnames)
{
    EXPECT_TRUE(isDeniedHostname("localhost"_t));
    EXPECT_FALSE(isDeniedHostname("example.com"_t));
}

UTEST(HostPolicy, SpecialTldSuffixesAndPlainNames)
{
    EXPECT_TRUE(hasSpecialTldSuffix("printer.local"_t));
    EXPECT_TRUE(hasSpecialTldSuffix("local"_t));
    EXPECT_TRUE(hasSpecialTldSuffix("router.home.arpa"_t));
    EXPECT_TRUE(hasSpecialTldSuffix("home.arpa"_t));
    EXPECT_TRUE(hasSpecialTldSuffix("test"_t));
    EXPECT_TRUE(hasSpecialTldSuffix("invalid"_t));
    EXPECT_TRUE(hasSpecialTldSuffix("example"_t));
    EXPECT_TRUE(hasSpecialTldSuffix("host.docker.internal"_t));

    EXPECT_FALSE(hasSpecialTldSuffix("example.com"_t));
    EXPECT_FALSE(hasSpecialTldSuffix("notlocaldomain"_t));
}
