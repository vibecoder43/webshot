#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <arkhiv/zip_archive.hpp>

#include <userver/utest/utest.hpp>

#include "crawler/artifacts.hpp"

using namespace v1::crawler;
using namespace text::literals;

namespace {

[[nodiscard]] std::vector<std::string> sortedEntryNames(const arkhiv::ZipArchive &archive)
{
    auto names = archive.entryPathsInOrder();
    std::sort(std::begin(names), std::end(names));
    return names;
}

[[nodiscard]] arkhiv::ZipArchive parseZipOrThrow(std::string_view bytes)
{
    arkhiv::ZipArchiveError error;
    const auto archive = arkhiv::ZipArchive::fromBytes(bytes, error);
    if (!archive)
        throw std::runtime_error(error.detail);
    return *archive;
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
    const auto archive = parseZipOrThrow(wacz);

    EXPECT_EQ(
        sortedEntryNames(archive), (std::vector<std::string>{
                                       "archive/data.warc",
                                       "datapackage.json",
                                       "indexes/index.cdxj",
                                       "logs/stderr.log",
                                       "logs/stdout.log",
                                       "pages/pages.jsonl",
                                   })
    );
    const auto datapackage = archive.findFile("datapackage.json");
    ASSERT_TRUE(datapackage);
    const auto cdxj = archive.findFile("indexes/index.cdxj");
    ASSERT_TRUE(cdxj);
    const auto pages = archive.findFile("pages/pages.jsonl");
    ASSERT_TRUE(pages);
    const auto stderrLog = archive.findFile("logs/stderr.log");
    ASSERT_TRUE(stderrLog);
    EXPECT_NE(datapackage->find("\"path\":\"archive/data.warc\""), std::string_view::npos);
    EXPECT_NE(pages->find("\"format\":\"json-pages-1.0\""), std::string_view::npos);
    EXPECT_NE(
        cdxj->find("test,seed)/ 20260311044121 {\"url\":\"https://seed.test/\""),
        std::string_view::npos
    );
    EXPECT_NE(
        cdxj->find("test,seed,www)/ 20260311044121 {\"url\":\"https://www.seed.test/\""),
        std::string_view::npos
    );
    EXPECT_TRUE(stderrLog->empty());
}

UTEST(CrawlerArtifacts, BuildWaczStoresFilesInExpectedZipOrder)
{
    auto exchange = makeBaseExchange("https://www.seed.test/");

    const auto wacz = buildWacz(
        makeRun("https://seed.test/"), buildPagesJsonl(exchange), buildWarc(exchange), "stdout\n",
        "stderr\n"
    );
    const auto archive = parseZipOrThrow(wacz);

    EXPECT_EQ(
        archive.entryPathsInOrder(), (std::vector<std::string>{
                                         "datapackage.json",
                                         "archive/data.warc",
                                         "pages/pages.jsonl",
                                         "logs/stdout.log",
                                         "logs/stderr.log",
                                         "indexes/index.cdxj",
                                     })
    );
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
    const auto ipArchive = parseZipOrThrow(ipWacz);

    const auto ipCdxj = ipArchive.findFile("indexes/index.cdxj");
    ASSERT_TRUE(ipCdxj);
    EXPECT_NE(
        ipCdxj->find(
            "http://127.0.0.1:8080/seed 20260311044121 "
            "{\"url\":\"http://127.0.0.1:8080/seed\""
        ),
        std::string_view::npos
    );
    EXPECT_EQ(ipCdxj->find("127.0.0.1:8080)/seed 20260311044121 {"), std::string_view::npos);

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
    const auto blobArchive = parseZipOrThrow(blobWacz);

    const auto blobCdxj = blobArchive.findFile("indexes/index.cdxj");
    ASSERT_TRUE(blobCdxj);
    EXPECT_NE(
        blobCdxj->find(
            "blob:https://seed.test/01234567-89ab-cdef-0123-456789abcdef 20260311044121 "
            "{\"url\":\"blob:https://seed.test/01234567-89ab-cdef-0123-456789abcdef\""
        ),
        std::string_view::npos
    );
}

UTEST(CrawlerArtifacts, BuildWaczStripsTrailingDotBeforeGeneratingSurtKeys)
{
    auto exchange = makeBaseExchange("https://seed.test./");

    const auto wacz = buildWacz(
        makeRun("https://seed.test./"), buildPagesJsonl(exchange), buildWarc(exchange), "", ""
    );
    const auto archive = parseZipOrThrow(wacz);

    const auto cdxj = archive.findFile("indexes/index.cdxj");
    ASSERT_TRUE(cdxj);
    EXPECT_NE(
        cdxj->find("test,seed)/ 20260311044121 {\"url\":\"https://seed.test./\""),
        std::string_view::npos
    );
    EXPECT_EQ(cdxj->find(",test,seed)/ 20260311044121 {"), std::string_view::npos);
}
