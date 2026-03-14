#include <algorithm>
#include <bit>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <userver/utest/utest.hpp>

#include "crawler/artifacts.hpp"

using namespace v1::crawler;
using namespace text::literals;

namespace {

[[nodiscard]] uint16_t readU16Le(std::string_view bytes, size_t offset)
{
    if (offset + 2 > bytes.size())
        throw std::runtime_error("stored zip entry truncated");

    const auto low = numericCast<uint16_t>(std::bit_cast<uint8_t>(bytes[offset]));
    const auto high = numericCast<uint16_t>(std::bit_cast<uint8_t>(bytes[offset + 1]));
    return low | numericCast<uint16_t>(high << 8U);
}

[[nodiscard]] uint32_t readU32Le(std::string_view bytes, size_t offset)
{
    if (offset + 4 > bytes.size())
        throw std::runtime_error("stored zip entry truncated");

    const auto byte0 = numericCast<uint32_t>(std::bit_cast<uint8_t>(bytes[offset]));
    const auto byte1 = numericCast<uint32_t>(std::bit_cast<uint8_t>(bytes[offset + 1]));
    const auto byte2 = numericCast<uint32_t>(std::bit_cast<uint8_t>(bytes[offset + 2]));
    const auto byte3 = numericCast<uint32_t>(std::bit_cast<uint8_t>(bytes[offset + 3]));
    return byte0 | (byte1 << 8U) | (byte2 << 16U) | (byte3 << 24U);
}

[[nodiscard]] std::unordered_map<std::string, std::string> parseStoredZip(std::string_view bytes)
{
    std::unordered_map<std::string, std::string> entries;
    size_t offset = 0;

    while (offset + 4 <= bytes.size()) {
        const auto signature = readU32Le(bytes, offset);
        if (signature == 0x02014b50U || signature == 0x06054b50U)
            break;
        if (signature != 0x04034b50U)
            throw std::runtime_error("unexpected zip local header signature");

        const auto compressedSize = readU32Le(bytes, offset + 18);
        const auto fileNameLength = readU16Le(bytes, offset + 26);
        const auto extraLength = readU16Le(bytes, offset + 28);
        const auto nameStart = offset + 30;
        const auto bodyStart = nameStart + fileNameLength + extraLength;
        const auto bodyEnd = bodyStart + compressedSize;
        if (bodyEnd > bytes.size())
            throw std::runtime_error("stored zip body truncated");

        const auto name = std::string(bytes.substr(nameStart, fileNameLength));
        entries.emplace(name, std::string(bytes.substr(bodyStart, compressedSize)));
        offset = bodyEnd;
    }

    return entries;
}

[[nodiscard]] std::vector<std::string>
sortedEntryNames(const std::unordered_map<std::string, std::string> &entries)
{
    std::vector<std::string> names;
    names.reserve(entries.size());
    for (const auto &[name, _] : entries)
        names.push_back(name);
    std::sort(std::begin(names), std::end(names));
    return names;
}

[[nodiscard]] RunRequest makeRun(std::string_view seedUrl)
{
    return {
        .seedUrl = String::fromBytesThrow(seedUrl),
        .jobTimeoutMs = 30000_i64,
    };
}

[[nodiscard]] CapturedExchange makeBaseExchange(std::string_view finalUrl)
{
    return {
        .finalUrl = String::fromBytesThrow(finalUrl),
        .statusCode = 200_i64,
        .statusMessage = "OK"_t,
        .headers =
            {
                {"content-type", "text/html; charset=utf-8"},
            },
        .body = "<html><head><title>Seed</title></head><body>seed</body></html>",
        .timestamp = "2026-03-11T04:41:21.565Z"_t,
        .redirectChain = {String::fromBytesThrow(finalUrl)},
        .mainDocumentRedirects = {},
        .resources = {},
        .title = "Seed"_t,
    };
}

} // namespace

UTEST(CrawlerArtifacts, BuildWarcPreservesCapturedSubresourceMethods)
{
    auto exchange = makeBaseExchange("https://example.test/seed");
    exchange.resources.push_back({
        .resourceUrl = "https://example.test/submit?source=page"_t,
        .method = "POST"_t,
        .statusCode = 201_i64,
        .statusMessage = "Created"_t,
        .headers =
            {
                {"content-type", "text/plain; charset=utf-8"},
            },
        .body = "submitted",
        .timestamp = "2026-03-11T04:41:22.000Z"_t,
    });

    const auto warc = buildWarc(exchange);
    const auto warcText = std::string_view(warc.bytes);

    ASSERT_EQ(warc.cdxRecords.size(), numericCast<size_t>(2));
    EXPECT_NE(
        warcText.find("WARC-Target-URI: https://example.test/submit?source=page"),
        std::string_view::npos
    );
    EXPECT_NE(warcText.find("POST /submit?source=page HTTP/1.1"), std::string_view::npos);
}

UTEST(CrawlerArtifacts, BuildWaczIncludesExpectedFilesAndSurtIndexKeys)
{
    auto exchange = makeBaseExchange("https://www.seed.test/");
    exchange.redirectChain = {"https://seed.test/"_t, "https://www.seed.test/"_t};
    exchange.mainDocumentRedirects.push_back({
        .redirectUrl = "https://seed.test/"_t,
        .statusCode = 301_i64,
        .statusMessage = "Moved Permanently"_t,
        .headers =
            {
                {"location", "https://www.seed.test/"},
                {"content-type", "text/html; charset=utf-8"},
            },
        .timestamp = "2026-03-11T04:41:21.100Z"_t,
    });

    const auto wacz = buildWacz(
        makeRun("https://seed.test/"), buildPagesJsonl(exchange), buildWarc(exchange), "stdout\n",
        ""
    );
    const auto entries = parseStoredZip(wacz);

    EXPECT_EQ(
        sortedEntryNames(entries), (std::vector<std::string>{
                                       "archive/data.warc",
                                       "datapackage.json",
                                       "indexes/index.cdxj",
                                       "logs/stderr.log",
                                       "logs/stdout.log",
                                       "pages/pages.jsonl",
                                   })
    );
    ASSERT_TRUE(entries.count("datapackage.json") == 1);
    ASSERT_TRUE(entries.count("indexes/index.cdxj") == 1);
    EXPECT_NE(
        entries.at("datapackage.json").find("\"path\":\"archive/data.warc\""), std::string::npos
    );
    EXPECT_NE(
        entries.at("pages/pages.jsonl").find("\"format\":\"json-pages-1.0\""), std::string::npos
    );
    EXPECT_NE(
        entries.at("indexes/index.cdxj")
            .find("test,seed)/ 20260311044121 {\"url\":\"https://seed.test/\""),
        std::string::npos
    );
    EXPECT_NE(
        entries.at("indexes/index.cdxj")
            .find("test,seed,www)/ 20260311044121 {\"url\":\"https://www.seed.test/\""),
        std::string::npos
    );
    EXPECT_EQ(entries.at("logs/stderr.log"), std::string{});
}

UTEST(CrawlerArtifacts, BuildWaczKeepsIpLiteralAndNonHttpKeysRaw)
{
    const auto ipWacz = buildWacz(
        makeRun("http://127.0.0.1:8080/seed"), std::string{"{\"id\":\"pages\"}\n"},
        {
            .bytes = "",
            .cdxRecords = {{
                .recordUrl = "http://127.0.0.1:8080/seed"_t,
                .timestamp = "20260311044121"_t,
                .statusCode = 200_i64,
                .headers =
                    {
                        {"content-type", "text/html; charset=utf-8"},
                    },
                .offset = 0_i64,
                .length = 123_i64,
            }},
        },
        "", ""
    );
    const auto ipEntries = parseStoredZip(ipWacz);

    ASSERT_TRUE(ipEntries.count("indexes/index.cdxj") == 1);
    EXPECT_NE(
        ipEntries.at("indexes/index.cdxj")
            .find(
                "http://127.0.0.1:8080/seed 20260311044121 {\"url\":\"http://127.0.0.1:8080/seed\""
            ),
        std::string::npos
    );
    EXPECT_EQ(
        ipEntries.at("indexes/index.cdxj").find("127.0.0.1:8080)/seed 20260311044121 {"),
        std::string::npos
    );

    const auto blobWacz = buildWacz(
        makeRun("https://seed.test/"), std::string{"{\"id\":\"pages\"}\n"},
        {
            .bytes = "",
            .cdxRecords = {{
                .recordUrl = "blob:https://seed.test/01234567-89ab-cdef-0123-456789abcdef"_t,
                .timestamp = "20260311044121"_t,
                .statusCode = 200_i64,
                .headers =
                    {
                        {"content-type", "application/octet-stream"},
                    },
                .offset = 0_i64,
                .length = 123_i64,
            }},
        },
        "", ""
    );
    const auto blobEntries = parseStoredZip(blobWacz);

    ASSERT_TRUE(blobEntries.count("indexes/index.cdxj") == 1);
    EXPECT_NE(
        blobEntries.at("indexes/index.cdxj")
            .find(
                "blob:https://seed.test/01234567-89ab-cdef-0123-456789abcdef 20260311044121 "
                "{\"url\":\"blob:https://seed.test/01234567-89ab-cdef-0123-456789abcdef\""
            ),
        std::string::npos
    );
}

UTEST(CrawlerArtifacts, BuildWaczStripsTrailingDotBeforeGeneratingSurtKeys)
{
    auto exchange = makeBaseExchange("https://seed.test./");

    const auto wacz = buildWacz(
        makeRun("https://seed.test./"), buildPagesJsonl(exchange), buildWarc(exchange), "", ""
    );
    const auto entries = parseStoredZip(wacz);

    ASSERT_TRUE(entries.count("indexes/index.cdxj") == 1);
    EXPECT_NE(
        entries.at("indexes/index.cdxj")
            .find("test,seed)/ 20260311044121 {\"url\":\"https://seed.test./\""),
        std::string::npos
    );
    EXPECT_EQ(
        entries.at("indexes/index.cdxj").find(",test,seed)/ 20260311044121 {"), std::string::npos
    );
}
