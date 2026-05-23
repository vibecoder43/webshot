#include <string>
#include <string_view>

#include <userver/utest/utest.hpp>

#include <uni_algo/conv.h>
#include <uni_algo/norm.h>

#include "integers.hpp"
#include "text.hpp"

using namespace text::literals;

namespace {

bool IsUtf8(std::string_view bytes) { return una::is_valid_utf8(bytes); }

bool IsStreamSafe(std::string_view bytes) { return una::norm::is_nfc_utf8(bytes); }

constexpr size_t StreamSafeExtraBytesUpperBoundForNonStarters(size_t non_starters)
{
    constexpr size_t max_non_starters_per_segment = 30UL;
    constexpr size_t cg_bytes = 2UL; // U+034F in UTF-8
    return (non_starters / max_non_starters_per_segment) * cg_bytes;
}

// Constexpr checks for text::String and the _t literal.
constexpr bool CheckStringFromBytes()
{
    auto opt = String::FromBytes("e\u0301"); // 'e' + combining acute
    if (!opt)
        return false;
    auto v = opt->View();
    return v.size() == 2 && v[0] == '\xC3' && v[1] == '\xA9';
}

constexpr bool CheckStringLiteralAndOps()
{
    using namespace text::literals;

    auto s = "e\u0301"_t;
    auto v = s.View();
    if (!(v.size() == 2 && v[0] == '\xC3' && v[1] == '\xA9'))
        return false;

    auto a = "abc"_t;
    auto b = "abd"_t;
    if (!(a == "abc"_t))
        return false;
    if (!(a < b))
        return false;

    auto lhs = "foo"_t;
    auto rhs = "bar"_t;
    auto sum = lhs + rhs;
    if (sum != "foobar"_t)
        return false;

    auto rev_opt = String::FromBytes("ab");
    if (!rev_opt)
        return false;
    auto rev = rev_opt->Reversed();
    if (rev != "ba"_t)
        return false;

    if (!una::is_valid_utf8(rev.View()))
        return false;
    if (!una::norm::is_nfc_utf8(rev.View()))
        return false;

    return true;
}

static_assert(CheckStringFromBytes(), "String::fromBytes constexpr normalization failed");
static_assert(CheckStringLiteralAndOps(), "String constexpr operations failed");

} // namespace

UTEST(TextString, FromBytesAscii)
{
    auto value = "hello"_t;
    EXPECT_FALSE(value.Empty());
    EXPECT_EQ(value, "hello"_t);
    EXPECT_TRUE(IsUtf8(value.View()));
    EXPECT_TRUE(IsStreamSafe(value.View()));
}

UTEST(TextString, FromBytesRejectsInvalidUtf8)
{
    std::string invalid("\xC3\x28", 2);
    auto value = String::FromBytes(invalid);
    EXPECT_FALSE(value);
}

UTEST(TextString, FromBytesNormalizesEquivalents)
{
    std::string precomposed("\xC3\xA9", 2);
    std::string decomposed("e\xCC\x81", 3);

    auto s1 = *String::FromBytes(precomposed);
    auto s2 = *String::FromBytes(decomposed);
    EXPECT_EQ(s1, s2);
    EXPECT_TRUE(IsUtf8(s1.View()));
    EXPECT_TRUE(IsStreamSafe(s1.View()));
}

UTEST(TextString, SizeAndEmptyConsistency)
{
    String empty;
    EXPECT_TRUE(empty.Empty());
    EXPECT_EQ(empty.SizeBytes(), NumericCast<size_t>(0));

    auto value = "xyz"_t;
    EXPECT_FALSE(value.Empty());
    EXPECT_EQ(value.SizeBytes(), NumericCast<size_t>(3));
}

UTEST(TextString, StartsWithEndsWithOverloads)
{
    String empty;
    EXPECT_FALSE(empty.StartsWith('/'));
    EXPECT_FALSE(empty.EndsWith('/'));
    EXPECT_TRUE(empty.StartsWith(""));
    EXPECT_TRUE(empty.EndsWith(""));

    auto value = "hello"_t;
    EXPECT_TRUE(value.StartsWith("he"));
    EXPECT_FALSE(value.StartsWith("ha"));
    EXPECT_TRUE(value.StartsWith('h'));
    EXPECT_FALSE(value.StartsWith('e'));

    EXPECT_TRUE(value.EndsWith("lo"));
    EXPECT_FALSE(value.EndsWith("la"));
    EXPECT_TRUE(value.EndsWith('o'));
    EXPECT_FALSE(value.EndsWith('l'));

    EXPECT_TRUE(value.EndsWith("lo"_t));
    EXPECT_FALSE(value.EndsWith("he"_t));
}

UTEST(TextString, PlusConcatenatesAscii)
{
    auto lhs = "foo"_t;
    auto rhs = "bar"_t;

    auto sum = lhs + rhs;
    EXPECT_EQ(sum, "foobar"_t);
    EXPECT_TRUE(IsUtf8(sum.View()));
    EXPECT_TRUE(IsStreamSafe(sum.View()));
}

UTEST(TextString, PlusNormalizesCrossBoundary)
{
    auto lhs = "e"_t;
    std::string combining("\xCC\x81", 2);
    auto rhs = *String::FromBytes(combining);

    auto combined = lhs + rhs;

    std::string precomposed("\xC3\xA9", 2);
    auto expected = *String::FromBytes(precomposed);

    EXPECT_EQ(combined, expected);
}

UTEST(TextString, PlusEqualsEmptyRhsNoChange)
{
    auto value = "abc"_t;

    const std::string original{value.View()};

    String empty;
    value += empty;

    EXPECT_EQ(value, *String::FromBytes(original));
}

UTEST(TextString, EqualityAndOrdering)
{
    auto a1 = "abc"_t;
    auto a2 = "abc"_t;
    auto b = "abd"_t;

    EXPECT_TRUE(a1 == a2);
    EXPECT_FALSE(a1 == b);
    EXPECT_TRUE(a1 < b);
}

UTEST(TextString, ReversedIsUtf8AndNormalized)
{
    std::string input;
    input.push_back('e');
    input.push_back('\xCC');
    input.push_back('\x81');
    input.append("abc");
    auto value = *String::FromBytes(input);

    auto rev = value.Reversed();
    EXPECT_TRUE(IsUtf8(rev.View()));
    EXPECT_TRUE(IsStreamSafe(rev.View()));
}

UTEST(TextString, HandlesLongCombiningSequenceStreamSafe)
{
    std::string input;
    input.push_back('e');
    for (int i = 0; i < 1000; i++) {
        input.push_back('\xCC');
        input.push_back('\x81');
    }

    auto value = *String::FromBytes(input);

    EXPECT_TRUE(IsUtf8(value.View()));
    constexpr size_t non_starters = 1000UL;
    constexpr size_t extra_bytes = StreamSafeExtraBytesUpperBoundForNonStarters(non_starters);
    EXPECT_LE(value.SizeBytes(), input.size() + extra_bytes);
}

UTEST(TextString, Idempotence)
{
    std::string raw = "e\xCC\x81"; // e + combining acute
    auto value = *String::FromBytes(raw);

    auto value2 = *String::FromBytes(std::string{value.View()});
    EXPECT_EQ(value, value2);
}

UTEST(TextString, RejectsVariousInvalidUtf8)
{
    auto ws = String::FromBytes(std::string{"\x80"});
    EXPECT_FALSE(ws);

    auto v2 = String::FromBytes(std::string{"\xC2"});
    EXPECT_FALSE(v2);

    auto v3 = String::FromBytes(std::string{"\xE2\x82"});
    EXPECT_FALSE(v3);

    auto v4 = String::FromBytes(std::string{"\xF0\x9F\x92"});
    EXPECT_FALSE(v4);
}

UTEST(TextString, HandlesNonBmpCharacters)
{
    std::string emoji("\xF0\x9F\x98\x80\xF0\x9F\x92\xA9", 8); // U+1F600 U+1F4A9

    auto value = *String::FromBytes(emoji);
    EXPECT_TRUE(IsUtf8(value.View()));
    EXPECT_TRUE(IsStreamSafe(value.View()));
    EXPECT_EQ(value, *String::FromBytes(emoji));

    auto prefix = "prefix-"_t;
    auto suffix = "-suffix"_t;

    auto combined = prefix + value + suffix;
    EXPECT_TRUE(IsUtf8(combined.View()));
    EXPECT_TRUE(IsStreamSafe(combined.View()));
}
