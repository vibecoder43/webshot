#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <archive.h>
#include <archive_entry.h>

#include <gtest/gtest.h>

#include "arkhiv/zip_archive.hpp"

namespace {

using arkhiv::ZipArchive;
using arkhiv::ZipArchiveBuilder;
using arkhiv::ZipArchiveError;
using arkhiv::ZipArchiveErrorCode;

using ArchiveWriterPtr = std::unique_ptr<archive, decltype(&archive_write_free)>;
using ArchiveEntryPtr = std::unique_ptr<archive_entry, decltype(&archive_entry_free)>;

struct ZipEntrySpec {
    std::string path;
    std::string body;
    mode_t file_type = AE_IFREG;
};

int OpenStringArchive(archive *, void *) { return ARCHIVE_OK; }

la_ssize_t AppendArchiveBytes(archive *, void *ctx, const void *buffer, size_t nbytes)
{
    auto &out = *static_cast<std::string *>(ctx);
    out.append(static_cast<const char *>(buffer), nbytes);
    return static_cast<la_ssize_t>(nbytes);
}

int CloseStringArchive(archive *, void *) { return ARCHIVE_OK; }

[[noreturn]] void Fail(std::string_view message)
{
    ADD_FAILURE() << message;
    std::abort();
}

[[nodiscard]] ArchiveWriterPtr MakeWriter()
{
    auto *writer = archive_write_new();
    if (writer == nullptr)
        Fail("failed to allocate zip writer");
    auto writer_ptr = ArchiveWriterPtr(writer, &archive_write_free);

    if (archive_write_set_format_zip(writer_ptr.get()) != ARCHIVE_OK)
        Fail("failed to enable zip output");
    if (archive_write_zip_set_compression_store(writer_ptr.get()) != ARCHIVE_OK)
        Fail("failed to enable stored zip output");

    return writer_ptr;
}

[[nodiscard]] std::string MakeZip(const std::vector<ZipEntrySpec> &entries)
{
    auto writer = MakeWriter();
    std::string out;

    if (archive_write_open(
            writer.get(), &out, &OpenStringArchive, &AppendArchiveBytes, &CloseStringArchive
        ) != ARCHIVE_OK) {
        Fail("failed to open zip writer");
    }

    for (const auto &spec : entries) {
        auto entry = ArchiveEntryPtr(archive_entry_new(), &archive_entry_free);
        if (!entry)
            Fail("failed to allocate zip entry");

        archive_entry_set_pathname(entry.get(), spec.path.c_str());
        archive_entry_set_filetype(entry.get(), spec.file_type);
        archive_entry_set_perm(entry.get(), spec.file_type == AE_IFDIR ? 0755 : 0644);
        archive_entry_set_size(
            entry.get(), spec.file_type == AE_IFREG ? static_cast<la_int64_t>(spec.body.size()) : 0
        );

        if (archive_write_header(writer.get(), entry.get()) != ARCHIVE_OK)
            Fail("failed to write zip header");
        if (spec.file_type == AE_IFREG && !spec.body.empty()) {
            auto written = archive_write_data(writer.get(), spec.body.data(), spec.body.size());
            if (written != static_cast<la_ssize_t>(spec.body.size()))
                Fail("failed to write zip body");
        }
    }

    if (archive_write_close(writer.get()) != ARCHIVE_OK)
        Fail("failed to close zip writer");
    return out;
}

} // namespace

TEST(ZipArchive, WritesAndReadsStoredFilesByPath)
{
    ZipArchiveBuilder builder;
    ZipArchiveError error;

    ASSERT_TRUE(builder.AddStoredFile("datapackage.json", error, R"({"profile":"data-package"})"));
    ASSERT_TRUE(builder.AddStoredFile("archive/data.warc", error, "warc bytes"));
    ASSERT_TRUE(builder.AddStoredFile("pages/pages.jsonl", error, "{\"id\":\"pages\"}\n"));

    const auto zip_bytes = builder.Finish(error);
    ASSERT_TRUE(zip_bytes);
    if (!zip_bytes)
        return;
    EXPECT_EQ(error.code, ZipArchiveErrorCode::kNone);

    const auto archive = ZipArchive::FromBytes(zip_bytes.value(), error);
    ASSERT_TRUE(archive);
    if (!archive)
        return;
    EXPECT_EQ(error.code, ZipArchiveErrorCode::kNone);

    EXPECT_EQ(
        archive->EntryPathsInOrder(), (std::vector<std::string>{
                                          "datapackage.json",
                                          "archive/data.warc",
                                          "pages/pages.jsonl",
                                      })
    );

    const auto datapackage = archive->FindFile("datapackage.json");
    ASSERT_TRUE(datapackage);
    if (!datapackage)
        return;
    EXPECT_EQ(datapackage.value(), std::string_view{R"({"profile":"data-package"})"});

    const auto warc = archive->FindFile("archive/data.warc");
    ASSERT_TRUE(warc);
    if (!warc)
        return;
    EXPECT_EQ(warc.value(), std::string_view{"warc bytes"});
}

TEST(ZipArchive, BuilderRejectsDuplicateEntryNames)
{
    ZipArchiveBuilder builder;
    ZipArchiveError error;

    ASSERT_TRUE(builder.AddStoredFile("logs/stdout.log", error, "first"));
    EXPECT_FALSE(builder.AddStoredFile("logs/stdout.log", error, "second"));
    EXPECT_EQ(error.code, ZipArchiveErrorCode::kDuplicateEntry);
}

TEST(ZipArchive, BuilderRejectsInvalidPaths)
{
    ZipArchiveBuilder builder;
    ZipArchiveError error;

    EXPECT_FALSE(builder.AddStoredFile("/absolute.txt", error, ""));
    EXPECT_EQ(error.code, ZipArchiveErrorCode::kInvalidPath);

    EXPECT_FALSE(builder.AddStoredFile("logs\\stderr.log", error, ""));
    EXPECT_EQ(error.code, ZipArchiveErrorCode::kInvalidPath);

    EXPECT_FALSE(builder.AddStoredFile("logs/../stderr.log", error, ""));
    EXPECT_EQ(error.code, ZipArchiveErrorCode::kInvalidPath);

    EXPECT_FALSE(builder.AddStoredFile("logs//stderr.log", error, ""));
    EXPECT_EQ(error.code, ZipArchiveErrorCode::kInvalidPath);
}

TEST(ZipArchive, RejectsDuplicateEntriesOnRead)
{
    ZipArchiveError error;
    auto zip_bytes = MakeZip({
        ZipEntrySpec{.path = "logs/stdout.log", .body = "first"},
        ZipEntrySpec{.path = "logs/stdout.log", .body = "second"},
    });

    EXPECT_FALSE(ZipArchive::FromBytes(zip_bytes, error));
    EXPECT_EQ(error.code, ZipArchiveErrorCode::kDuplicateEntry);
}

TEST(ZipArchive, RejectsInvalidZipBytes)
{
    ZipArchiveError error;

    EXPECT_FALSE(ZipArchive::FromBytes(std::string_view{"not a zip archive"}, error));
    EXPECT_TRUE(
        error.code == ZipArchiveErrorCode::kOpenFailed ||
        error.code == ZipArchiveErrorCode::kReadHeaderFailed
    );
}
