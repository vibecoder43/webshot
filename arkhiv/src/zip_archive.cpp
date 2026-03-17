#include "arkhiv/zip_archive.hpp"

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <archive.h>
#include <archive_entry.h>

namespace arkhiv {

namespace {

using ArchiveReaderPtr = std::unique_ptr<archive, decltype(&archive_read_free)>;
using ArchiveWriterPtr = std::unique_ptr<archive, decltype(&archive_write_free)>;
using ArchiveEntryPtr = std::unique_ptr<archive_entry, decltype(&archive_entry_free)>;

template <typename T>
[[nodiscard]] std::optional<T>
fail(ZipArchiveError &errorOut, ZipArchiveErrorCode code, std::string detail)
{
    errorOut.code = code;
    errorOut.detail = std::move(detail);
    return {};
}

[[nodiscard]] bool failBool(ZipArchiveError &errorOut, ZipArchiveErrorCode code, std::string detail)
{
    errorOut.code = code;
    errorOut.detail = std::move(detail);
    return false;
}

[[nodiscard]] std::string formatArchiveDetail(archive *handle, std::string_view context)
{
    auto detail = std::string(context);
    const auto *archiveDetail = archive_error_string(handle);
    if (archiveDetail != nullptr) {
        detail += ": ";
        detail += archiveDetail;
    }
    return detail;
}

[[nodiscard]] bool isAllowedZipPath(std::string_view path) noexcept
{
    if (path.empty() || path == "." || path == "..")
        return false;
    if (path.front() == '/' || path.front() == '\\')
        return false;
    if (path.back() == '/' || path.back() == '\\')
        return false;

    size_t componentStart = 0;
    for (size_t index = 0; index <= path.size(); index++) {
        if (index < path.size()) {
            const auto ch = path[index];
            if (ch == '\\')
                return false;
            if (static_cast<unsigned char>(ch) < 0x20U || ch == 0x7f)
                return false;
            if (ch != '/')
                continue;
        }

        const auto componentSize = index - componentStart;
        if (componentSize == 0)
            return false;

        const auto component = path.substr(componentStart, componentSize);
        if (component == "." || component == "..")
            return false;
        componentStart = index + 1;
    }

    return true;
}

[[nodiscard]] ArchiveEntryPtr
makeRegularFileEntry(std::string_view path, ZipArchiveError &errorOut, std::string_view body)
{
    auto entry = ArchiveEntryPtr(archive_entry_new(), &archive_entry_free);
    if (!entry) {
        errorOut.code = ZipArchiveErrorCode::kWriterInitFailed;
        errorOut.detail = "failed to allocate zip entry";
        return {nullptr, &archive_entry_free};
    }

    const auto pathText = std::string(path);
    archive_entry_set_pathname(entry.get(), pathText.c_str());
    archive_entry_set_filetype(entry.get(), AE_IFREG);
    archive_entry_set_perm(entry.get(), 0644);
    archive_entry_set_size(entry.get(), static_cast<la_int64_t>(body.size()));
    return entry;
}

int openStringArchive(archive *, void *) { return ARCHIVE_OK; }

la_ssize_t appendArchiveBytes(archive *, void *ctx, const void *buffer, size_t nbytes)
{
    auto &out = *static_cast<std::string *>(ctx);
    out.append(static_cast<const char *>(buffer), nbytes);
    return static_cast<la_ssize_t>(nbytes);
}

int closeStringArchive(archive *, void *) { return ARCHIVE_OK; }

[[nodiscard]] std::optional<ArchiveWriterPtr> makeZipWriter(ZipArchiveError &errorOut)
{
    auto *writer = archive_write_new();
    if (writer == nullptr)
        return fail<ArchiveWriterPtr>(
            errorOut, ZipArchiveErrorCode::kWriterInitFailed, "failed to allocate zip writer"
        );

    auto writerPtr = ArchiveWriterPtr(writer, &archive_write_free);
    if (archive_write_set_format_zip(writerPtr.get()) != ARCHIVE_OK) {
        return fail<ArchiveWriterPtr>(
            errorOut, ZipArchiveErrorCode::kWriterInitFailed,
            formatArchiveDetail(writerPtr.get(), "failed to enable zip writer")
        );
    }
    if (archive_write_zip_set_compression_store(writerPtr.get()) != ARCHIVE_OK) {
        return fail<ArchiveWriterPtr>(
            errorOut, ZipArchiveErrorCode::kWriterInitFailed,
            formatArchiveDetail(writerPtr.get(), "failed to configure stored zip output")
        );
    }

    return writerPtr;
}

[[nodiscard]] std::optional<ArchiveReaderPtr> makeZipReader(ZipArchiveError &errorOut)
{
    auto *reader = archive_read_new();
    if (reader == nullptr)
        return fail<ArchiveReaderPtr>(
            errorOut, ZipArchiveErrorCode::kReaderInitFailed, "failed to allocate zip reader"
        );

    auto readerPtr = ArchiveReaderPtr(reader, &archive_read_free);
    if (archive_read_support_format_zip(readerPtr.get()) != ARCHIVE_OK) {
        return fail<ArchiveReaderPtr>(
            errorOut, ZipArchiveErrorCode::kReaderInitFailed,
            formatArchiveDetail(readerPtr.get(), "failed to enable zip reader")
        );
    }

    return readerPtr;
}

[[nodiscard]] std::optional<std::string>
readEntryBytes(archive *reader, archive_entry *entry, ZipArchiveError &errorOut)
{
    const auto entrySize = archive_entry_size(entry);
    if (entrySize < 0) {
        return fail<std::string>(
            errorOut, ZipArchiveErrorCode::kInvalidEntrySize, "zip entry size must not be negative"
        );
    }
    if (static_cast<uint64_t>(entrySize) >
        static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        return fail<std::string>(
            errorOut, ZipArchiveErrorCode::kEntryTooLarge, "zip entry is too large to fit in memory"
        );
    }

    auto data = std::string(static_cast<size_t>(entrySize), '\0');
    size_t copied = 0;

    while (copied < data.size()) {
        const auto rc = archive_read_data(reader, data.data() + copied, data.size() - copied);
        if (rc < 0) {
            return fail<std::string>(
                errorOut, ZipArchiveErrorCode::kReadDataFailed,
                formatArchiveDetail(reader, "failed to read zip entry body")
            );
        }
        if (rc == 0)
            break;
        copied += static_cast<size_t>(rc);
    }

    if (copied != data.size()) {
        return fail<std::string>(
            errorOut, ZipArchiveErrorCode::kTruncatedEntry, "zip entry body was truncated"
        );
    }

    return data;
}

} // namespace

bool ZipArchiveBuilder::addStoredFile(
    std::string_view path, ZipArchiveError &errorOut, std::string_view body
)
{
    errorOut = {};

    if (!isAllowedZipPath(path)) {
        return failBool(
            errorOut, ZipArchiveErrorCode::kInvalidPath,
            "zip entry path is not allowed: " + std::string(path)
        );
    }

    const auto pathText = std::string(path);
    if (entryPaths.contains(pathText)) {
        return failBool(
            errorOut, ZipArchiveErrorCode::kDuplicateEntry, "duplicate zip entry: " + pathText
        );
    }

    entries.push_back({pathText, std::string(body)});
    entryPaths.insert(entries.back().path);
    return true;
}

std::optional<std::string> ZipArchiveBuilder::finish(ZipArchiveError &errorOut) const
{
    errorOut = {};

    auto writer = makeZipWriter(errorOut);
    if (!writer)
        return {};

    std::string out;
    if (archive_write_open(
            writer->get(), &out, &openStringArchive, &appendArchiveBytes, &closeStringArchive
        ) != ARCHIVE_OK) {
        return fail<std::string>(
            errorOut, ZipArchiveErrorCode::kOpenFailed,
            formatArchiveDetail(writer->get(), "failed to open zip writer")
        );
    }

    for (const auto &entrySpec : entries) {
        auto entry = makeRegularFileEntry(entrySpec.path, errorOut, entrySpec.body);
        if (!entry)
            return {};

        if (archive_write_header(writer->get(), entry.get()) != ARCHIVE_OK) {
            return fail<std::string>(
                errorOut, ZipArchiveErrorCode::kWriteHeaderFailed,
                formatArchiveDetail(
                    writer->get(), std::string("failed to write zip header for ") + entrySpec.path
                )
            );
        }
        if (!entrySpec.body.empty()) {
            const auto written = archive_write_data(
                writer->get(), entrySpec.body.data(), entrySpec.body.size()
            );
            if (written != static_cast<la_ssize_t>(entrySpec.body.size())) {
                return fail<std::string>(
                    errorOut, ZipArchiveErrorCode::kWriteDataFailed,
                    formatArchiveDetail(
                        writer->get(), std::string("failed to write zip body for ") + entrySpec.path
                    )
                );
            }
        }
    }

    if (archive_write_close(writer->get()) != ARCHIVE_OK) {
        return fail<std::string>(
            errorOut, ZipArchiveErrorCode::kFinishFailed,
            formatArchiveDetail(writer->get(), "failed to close zip writer")
        );
    }

    errorOut = {};
    return out;
}

ZipArchive::ZipArchive(
    std::map<std::string, std::string, std::less<>> filesIn, std::vector<std::string> pathsIn
)
    : files(std::move(filesIn)), paths(std::move(pathsIn))
{
}

std::optional<ZipArchive> ZipArchive::fromBytes(std::string_view bytes, ZipArchiveError &errorOut)
{
    errorOut = {};

    auto reader = makeZipReader(errorOut);
    if (!reader)
        return {};

    if (archive_read_open_memory(reader->get(), bytes.data(), bytes.size()) != ARCHIVE_OK) {
        return fail<ZipArchive>(
            errorOut, ZipArchiveErrorCode::kOpenFailed,
            formatArchiveDetail(reader->get(), "failed to open zip archive from memory")
        );
    }

    std::map<std::string, std::string, std::less<>> filesOut;
    std::vector<std::string> pathsOut;
    archive_entry *entry = nullptr;

    while (true) {
        const auto rc = archive_read_next_header(reader->get(), &entry);
        if (rc == ARCHIVE_EOF)
            break;
        if (rc != ARCHIVE_OK) {
            return fail<ZipArchive>(
                errorOut, ZipArchiveErrorCode::kReadHeaderFailed,
                formatArchiveDetail(reader->get(), "failed to read zip entry header")
            );
        }

        const auto *pathBytes = archive_entry_pathname(entry);
        if (pathBytes == nullptr) {
            return fail<ZipArchive>(
                errorOut, ZipArchiveErrorCode::kMissingPath, "zip entry is missing a pathname"
            );
        }

        const auto path = std::string_view(pathBytes);
        if (!isAllowedZipPath(path)) {
            return fail<ZipArchive>(
                errorOut, ZipArchiveErrorCode::kInvalidPath,
                "zip entry path is not allowed: " + std::string(path)
            );
        }

        if (archive_entry_filetype(entry) != AE_IFREG) {
            return fail<ZipArchive>(
                errorOut, ZipArchiveErrorCode::kUnsupportedEntryType,
                "zip entry is not a regular file: " + std::string(path)
            );
        }
        if (archive_entry_symlink(entry) != nullptr || archive_entry_hardlink(entry) != nullptr) {
            return fail<ZipArchive>(
                errorOut, ZipArchiveErrorCode::kUnsupportedEntryType,
                "zip entry must not be a link: " + std::string(path)
            );
        }

        auto [it, inserted] = filesOut.emplace(std::string(path), std::string());
        if (!inserted) {
            return fail<ZipArchive>(
                errorOut, ZipArchiveErrorCode::kDuplicateEntry,
                "duplicate zip entry: " + std::string(path)
            );
        }

        auto body = readEntryBytes(reader->get(), entry, errorOut);
        if (!body)
            return {};
        it->second = std::move(body.value());
        pathsOut.push_back(std::string(path));
    }

    errorOut = {};
    return ZipArchive(std::move(filesOut), std::move(pathsOut));
}

std::optional<std::string_view> ZipArchive::findFile(std::string_view path) const noexcept
{
    const auto it = files.find(path);
    if (it == files.end())
        return {};
    return std::string_view(it->second.data(), it->second.size());
}

const std::vector<std::string> &ZipArchive::entryPathsInOrder() const noexcept { return paths; }

} // namespace arkhiv
