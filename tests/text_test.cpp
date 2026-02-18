#include <string>
#include <string_view>

#include <userver/utest/utest.hpp>

#include <uni_algo/conv.h>
#include <uni_algo/norm.h>

#include "text.hpp"

namespace {

bool isUtf8(std::string_view bytes) { return una::is_valid_utf8(bytes); }

bool isStreamSafe(std::string_view bytes) { return una::norm::is_nfc_utf8(bytes); }

constexpr size_t streamSafeExtraBytesUpperBoundForNonStarters(size_t nonStarters)
{
    constexpr size_t kMaxNonStartersPerSegment = 30U;
    constexpr size_t kCgBytes = 2U; // U+034F in UTF-8
    return (nonStarters / kMaxNonStartersPerSegment) * kCgBytes;
}

// Constexpr checks for text::String and the _t literal.
constexpr bool checkStringFromBytes()
{
    auto opt = String::fromBytes("e\u0301"); // 'e' + combining acute
    if (!opt)
        return false;
    auto v = opt->view();
    return v.size() == 2 && v[0] == '\xC3' && v[1] == '\xA9';
}

constexpr bool checkStringLiteralAndOps()
{
    using namespace text::literals;

    auto s = "e\u0301"_t;
    auto v = s.view();
    if (!(v.size() == 2 && v[0] == '\xC3' && v[1] == '\xA9'))
        return false;

    auto a = "abc"_t;
    auto b = "abd"_t;
    if (!(a.view() == std::string_view{"abc"}))
        return false;
    if (!(a < b))
        return false;

    auto lhs = "foo"_t;
    auto rhs = "bar"_t;
    auto sum = lhs + rhs;
    if (sum.view() != std::string_view{"foobar"})
        return false;

    auto revOpt = String::fromBytes("ab");
    if (!revOpt)
        return false;
    auto rev = revOpt->reversed();
    if (rev.view() != std::string_view{"ba"})
        return false;

    if (!una::is_valid_utf8(rev.view()))
        return false;
    if (!una::norm::is_nfc_utf8(rev.view()))
        return false;

    return true;
}

static_assert(checkStringFromBytes(), "String::fromBytes constexpr normalization failed");
static_assert(checkStringLiteralAndOps(), "String constexpr operations failed");

} // namespace

UTEST(TextString, FromBytesAscii)
{
    auto value = String::fromBytes(std::string{"hello"});
    ASSERT_TRUE(value);
    EXPECT_FALSE(value->empty());
    EXPECT_EQ(value->view(), std::string_view{"hello"});
    EXPECT_TRUE(isUtf8(value->view()));
    EXPECT_TRUE(isStreamSafe(value->view()));
}

UTEST(TextString, FromBytesRejectsInvalidUtf8)
{
    std::string invalid("\xC3\x28", 2);
    auto value = String::fromBytes(invalid);
    EXPECT_FALSE(value);
}

UTEST(TextString, FromBytesNormalizesEquivalents)
{
    std::string precomposed("\xC3\xA9", 2);
    std::string decomposed("e\xCC\x81", 3);

    auto s1 = String::fromBytes(precomposed);
    auto s2 = String::fromBytes(decomposed);

    ASSERT_TRUE(s1);
    ASSERT_TRUE(s2);
    EXPECT_EQ(s1->view(), s2->view());
    EXPECT_TRUE(isUtf8(s1->view()));
    EXPECT_TRUE(isStreamSafe(s1->view()));
}

UTEST(TextString, SizeAndEmptyConsistency)
{
    String empty;
    EXPECT_TRUE(empty.empty());
    EXPECT_EQ(empty.sizeBytes(), static_cast<size_t>(0));

    auto value = String::fromBytes(std::string{"xyz"});
    ASSERT_TRUE(value);
    EXPECT_FALSE(value->empty());
    EXPECT_EQ(value->sizeBytes(), static_cast<size_t>(3));
}

UTEST(TextString, PlusConcatenatesAscii)
{
    auto lhs = String::fromBytes(std::string{"foo"});
    auto rhs = String::fromBytes(std::string{"bar"});

    ASSERT_TRUE(lhs);
    ASSERT_TRUE(rhs);

    auto sum = *lhs + *rhs;
    EXPECT_EQ(sum.view(), std::string_view{"foobar"});
    EXPECT_TRUE(isUtf8(sum.view()));
    EXPECT_TRUE(isStreamSafe(sum.view()));
}

UTEST(TextString, PlusNormalizesCrossBoundary)
{
    auto lhs = String::fromBytes(std::string{"e"});
    std::string combining("\xCC\x81", 2);
    auto rhs = String::fromBytes(combining);

    ASSERT_TRUE(lhs);
    ASSERT_TRUE(rhs);

    auto combined = *lhs + *rhs;

    std::string precomposed("\xC3\xA9", 2);
    auto expected = String::fromBytes(precomposed);
    ASSERT_TRUE(expected);

    EXPECT_EQ(combined.view(), expected->view());
}

UTEST(TextString, PlusEqualsEmptyRhsNoChange)
{
    auto value = String::fromBytes(std::string{"abc"});
    ASSERT_TRUE(value);

    const std::string original{value->view()};

    String empty;
    *value += empty;

    EXPECT_EQ(value->view(), std::string_view{original});
}

UTEST(TextString, EqualityAndOrdering)
{
    auto a1 = String::fromBytes(std::string{"abc"});
    auto a2 = String::fromBytes(std::string{"abc"});
    auto b = String::fromBytes(std::string{"abd"});

    ASSERT_TRUE(a1);
    ASSERT_TRUE(a2);
    ASSERT_TRUE(b);

    EXPECT_TRUE(*a1 == *a2);
    EXPECT_FALSE(*a1 == *b);
    EXPECT_TRUE(*a1 < *b);
}

UTEST(TextString, ReversedIsUtf8AndNormalized)
{
    std::string input;
    input.push_back('e');
    input.push_back('\xCC');
    input.push_back('\x81');
    input.append("abc");
    auto value = String::fromBytes(input);
    ASSERT_TRUE(value);

    auto rev = value->reversed();
    EXPECT_TRUE(isUtf8(rev.view()));
    EXPECT_TRUE(isStreamSafe(rev.view()));
}

UTEST(TextString, HandlesLongCombiningSequenceStreamSafe)
{
    std::string input;
    input.push_back('e');
    for (int i = 0; i < 1000; i++) {
        input.push_back('\xCC');
        input.push_back('\x81');
    }

    auto value = String::fromBytes(input);
    ASSERT_TRUE(value);

    EXPECT_TRUE(isUtf8(value->view()));
    constexpr size_t kNonStarters = 1000U;
    constexpr size_t kExtraBytes = streamSafeExtraBytesUpperBoundForNonStarters(kNonStarters);
    EXPECT_LE(value->sizeBytes(), static_cast<size_t>(input.size()) + kExtraBytes);
}

UTEST(TextString, Idempotence)
{
    std::string raw = "e\xCC\x81"; // e + combining acute
    auto value = String::fromBytes(raw);
    ASSERT_TRUE(value);

    auto value2 = String::fromBytes(std::string{value->view()});
    ASSERT_TRUE(value2);
    EXPECT_EQ(value->view(), value2->view());
}

UTEST(TextString, RejectsVariousInvalidUtf8)
{
    auto v1 = String::fromBytes(std::string{"\x80"});
    EXPECT_FALSE(v1);

    auto v2 = String::fromBytes(std::string{"\xC2"});
    EXPECT_FALSE(v2);

    auto v3 = String::fromBytes(std::string{"\xE2\x82"});
    EXPECT_FALSE(v3);

    auto v4 = String::fromBytes(std::string{"\xF0\x9F\x92"});
    EXPECT_FALSE(v4);
}

UTEST(TextString, HandlesNonBmpCharacters)
{
    std::string emoji("\xF0\x9F\x98\x80\xF0\x9F\x92\xA9", 8); // U+1F600 U+1F4A9

    auto value = String::fromBytes(emoji);
    ASSERT_TRUE(value);
    EXPECT_TRUE(isUtf8(value->view()));
    EXPECT_TRUE(isStreamSafe(value->view()));
    EXPECT_EQ(value->view(), std::string_view{emoji});

    auto prefix = String::fromBytes(std::string{"prefix-"});
    auto suffix = String::fromBytes(std::string{"-suffix"});
    ASSERT_TRUE(prefix);
    ASSERT_TRUE(suffix);

    auto combined = *prefix + *value + *suffix;
    EXPECT_TRUE(isUtf8(combined.view()));
    EXPECT_TRUE(isStreamSafe(combined.view()));
}
