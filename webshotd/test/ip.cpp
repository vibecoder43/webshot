#include "ip.hpp"

#include <variant>

#include <userver/utest/utest.hpp>

namespace {

using namespace text::literals;

[[nodiscard]] ws::Ip4 ParseIp4Strict(const String &text)
{
    auto ip = ws::ParseIp4(text);
    EXPECT_TRUE(ip);
    return *ip;
}

[[nodiscard]] ws::Ip6 ParseIp6Strict(const String &text)
{
    auto ip = ws::ParseIp6(text);
    EXPECT_TRUE(ip);
    return *ip;
}

[[nodiscard]] ws::Ip ParseIpStrict(const String &text)
{
    auto ip = ws::ParseIp(text);
    EXPECT_TRUE(ip);
    return *ip;
}

} // namespace

UTEST(Ip, ParsesIpLiteralsIntoTypedValues)
{
    EXPECT_TRUE(ws::ParseIp4("203.0.113.1"_t));
    EXPECT_TRUE(ws::ParseIp6("2001:db8::1"_t));
    EXPECT_TRUE(ws::ParseIp6("[2001:db8::1]"_t));
    EXPECT_TRUE(std::holds_alternative<ws::Ip4>(ParseIpStrict("203.0.113.1"_t)));
    EXPECT_TRUE(std::holds_alternative<ws::Ip6>(ParseIpStrict("2001:db8::1"_t)));
}

UTEST(Ip, FormatsCanonicalIpText)
{
    EXPECT_EQ(*ws::ToCanonicalIpText(ParseIpStrict("203.0.113.1"_t)), "203.0.113.1"_t);
    EXPECT_EQ(*ws::ToCanonicalIpText(ParseIpStrict("[2001:db8::1]"_t)), "2001:db8::1"_t);
    EXPECT_EQ(
        *ws::ToCanonicalIpText(ParseIpStrict("::ffff:198.51.100.9"_t)), "::ffff:198.51.100.9"_t
    );
}

UTEST(Ip, RejectsInvalidIpText)
{
    EXPECT_FALSE(ws::ParseIp(""_t));
    EXPECT_FALSE(ws::ParseIp("example.com"_t));
    EXPECT_FALSE(ws::ParseIp("127.0.0.1:80"_t));
    EXPECT_FALSE(ws::ParseIp("[2001:db8::1"_t));
}

UTEST(Ip, PublicRoutableIpv4BlocksSpecialRanges)
{
    EXPECT_FALSE(ws::IsPublicRoutable(ParseIp4Strict("0.0.0.1"_t)));
    EXPECT_FALSE(ws::IsPublicRoutable(ParseIp4Strict("10.0.0.1"_t)));
    EXPECT_FALSE(ws::IsPublicRoutable(ParseIp4Strict("100.64.0.1"_t)));
    EXPECT_FALSE(ws::IsPublicRoutable(ParseIp4Strict("127.0.0.1"_t)));
    EXPECT_FALSE(ws::IsPublicRoutable(ParseIp4Strict("169.254.169.254"_t)));
    EXPECT_FALSE(ws::IsPublicRoutable(ParseIp4Strict("172.16.0.1"_t)));
    EXPECT_FALSE(ws::IsPublicRoutable(ParseIp4Strict("192.168.0.1"_t)));
    EXPECT_FALSE(ws::IsPublicRoutable(ParseIp4Strict("198.18.0.1"_t)));
    EXPECT_FALSE(ws::IsPublicRoutable(ParseIp4Strict("224.0.0.1"_t)));
    EXPECT_FALSE(ws::IsPublicRoutable(ParseIp4Strict("240.0.0.1"_t)));
}

UTEST(Ip, PublicRoutableIpv4AllowsPublicRange)
{
    EXPECT_TRUE(ws::IsPublicRoutable(ParseIp4Strict("8.8.8.8"_t)));
}

UTEST(Ip, PublicRoutableIpv6BlocksSpecialRanges)
{
    EXPECT_FALSE(ws::IsPublicRoutable(ParseIp6Strict("::"_t)));
    EXPECT_FALSE(ws::IsPublicRoutable(ParseIp6Strict("::1"_t)));
    EXPECT_FALSE(ws::IsPublicRoutable(ParseIp6Strict("fc00::1"_t)));
    EXPECT_FALSE(ws::IsPublicRoutable(ParseIp6Strict("fe80::1"_t)));
    EXPECT_FALSE(ws::IsPublicRoutable(ParseIp6Strict("::ffff:192.0.2.1"_t)));
    EXPECT_FALSE(ws::IsPublicRoutable(ParseIp6Strict("ff00::1"_t)));
}

UTEST(Ip, PublicRoutableIpv6AllowsGlobalUnicast)
{
    EXPECT_TRUE(ws::IsPublicRoutable(ParseIp6Strict("2606:4700:4700::1111"_t)));
}
