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
Fail(ZipArchiveError &error_out, ZipArchiveErrorCode code, std::string detail)
{
    error_out.code = code;
    error_out.detail = std::move(detail);
    return {};
}

[[nodiscard]] bool
FailBool(ZipArchiveError &error_out, ZipArchiveErrorCode code, std::string detail)
{
    error_out.code = code;
    error_out.detail = std::move(detail);
    return false;
}

[[nodiscard]] std::string FormatArchiveDetail(archive *handle, std::string_view context)
{
    auto detail = std::string(context);
    const auto *archive_detail = archive_error_string(handle);
    if (archive_detail != nullptr) {
        detail += ": ";
        detail += archive_detail;
    }
    return detail;
}

[[nodiscard]] bool IsAllowedZipPath(std::string_view path) noexcept
{
    if (path.empty() || path == "." || path == "..")
        return false;
    if (path.front() == '/' || path.front() == '\\')
        return false;
    if (path.back() == '/' || path.back() == '\\')
        return false;

    size_t component_start = 0;
    for (size_t index = 0; index <= path.size(); index++) {
        if (index < path.size()) {
            auto ch = path[index];
            if (ch == '\\')
                return false;
            if (static_cast<unsigned char>(ch) < 0x20U || ch == 0x7f)
                return false;
            if (ch != '/')
                continue;
        }

        auto component_size = index - component_start;
        if (component_size == 0)
            return false;

        auto component = path.substr(component_start, component_size);
        if (component == "." || component == "..")
            return false;
        component_start = index + 1;
    }

    return true;
}

[[nodiscard]] ArchiveEntryPtr
MakeRegularFileEntry(std::string_view path, ZipArchiveError &error_out, std::string_view body)
{
    auto entry = ArchiveEntryPtr(archive_entry_new(), &archive_entry_free);
    if (!entry) {
        error_out.code = ZipArchiveErrorCode::kWriterInitFailed;
        error_out.detail = "failed to allocate zip entry";
        return {nullptr, &archive_entry_free};
    }

    auto path_text = std::string(path);
    archive_entry_set_pathname(entry.get(), path_text.c_str());
    archive_entry_set_filetype(entry.get(), AE_IFREG);
    archive_entry_set_perm(entry.get(), 0644);
    archive_entry_set_size(entry.get(), static_cast<la_int64_t>(body.size()));
    return entry;
}

int OpenStringArchive(archive *, void *) { return ARCHIVE_OK; }

la_ssize_t AppendArchiveBytes(archive *, void *ctx, const void *buffer, size_t nbytes)
{
    auto &out = *static_cast<std::string *>(ctx);
    out.append(static_cast<const char *>(buffer), nbytes);
    return static_cast<la_ssize_t>(nbytes);
}

int CloseStringArchive(archive *, void *) { return ARCHIVE_OK; }

[[nodiscard]] std::optional<ArchiveWriterPtr> MakeZipWriter(ZipArchiveError &error_out)
{
    auto *writer = archive_write_new();
    if (writer == nullptr)
        return Fail<ArchiveWriterPtr>(
            error_out, ZipArchiveErrorCode::kWriterInitFailed, "failed to allocate zip writer"
        );

    auto writer_ptr = ArchiveWriterPtr(writer, &archive_write_free);
    if (archive_write_set_format_zip(writer_ptr.get()) != ARCHIVE_OK) {
        return Fail<ArchiveWriterPtr>(
            error_out, ZipArchiveErrorCode::kWriterInitFailed,
            FormatArchiveDetail(writer_ptr.get(), "failed to enable zip writer")
        );
    }
    if (archive_write_zip_set_compression_store(writer_ptr.get()) != ARCHIVE_OK) {
        return Fail<ArchiveWriterPtr>(
            error_out, ZipArchiveErrorCode::kWriterInitFailed,
            FormatArchiveDetail(writer_ptr.get(), "failed to configure stored zip output")
        );
    }

    return writer_ptr;
}

[[nodiscard]] std::optional<ArchiveReaderPtr> MakeZipReader(ZipArchiveError &error_out)
{
    auto *reader = archive_read_new();
    if (reader == nullptr)
        return Fail<ArchiveReaderPtr>(
            error_out, ZipArchiveErrorCode::kReaderInitFailed, "failed to allocate zip reader"
        );

    auto reader_ptr = ArchiveReaderPtr(reader, &archive_read_free);
    if (archive_read_support_format_zip(reader_ptr.get()) != ARCHIVE_OK) {
        return Fail<ArchiveReaderPtr>(
            error_out, ZipArchiveErrorCode::kReaderInitFailed,
            FormatArchiveDetail(reader_ptr.get(), "failed to enable zip reader")
        );
    }

    return reader_ptr;
}

[[nodiscard]] std::optional<std::string>
ReadEntryBytes(archive *reader, archive_entry *entry, ZipArchiveError &error_out)
{
    const auto entry_size = archive_entry_size(entry);
    if (entry_size < 0) {
        return Fail<std::string>(
            error_out, ZipArchiveErrorCode::kInvalidEntrySize, "zip entry size must not be negative"
        );
    }
    if (static_cast<uint64_t>(entry_size) >
        static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        return Fail<std::string>(
            error_out, ZipArchiveErrorCode::kEntryTooLarge,
            "zip entry is too large to fit in memory"
        );
    }

    auto data = std::string(static_cast<size_t>(entry_size), '\0');
    size_t copied = 0;

    while (copied < data.size()) {
        auto rc = archive_read_data(reader, data.data() + copied, data.size() - copied);
        if (rc < 0) {
            return Fail<std::string>(
                error_out, ZipArchiveErrorCode::kReadDataFailed,
                FormatArchiveDetail(reader, "failed to read zip entry body")
            );
        }
        if (rc == 0)
            break;
        copied += static_cast<size_t>(rc);
    }

    if (copied != data.size()) {
        return Fail<std::string>(
            error_out, ZipArchiveErrorCode::kTruncatedEntry, "zip entry body was truncated"
        );
    }

    return data;
}

} // namespace

bool ZipArchiveBuilder::AddStoredFile(
    std::string_view path, ZipArchiveError &error_out, std::string_view body
)
{
    error_out = {};

    if (!IsAllowedZipPath(path)) {
        return FailBool(
            error_out, ZipArchiveErrorCode::kInvalidPath,
            "zip entry path is not allowed: " + std::string(path)
        );
    }

    auto path_text = std::string(path);
    if (entry_paths_.contains(path_text)) {
        return FailBool(
            error_out, ZipArchiveErrorCode::kDuplicateEntry, "duplicate zip entry: " + path_text
        );
    }

    entries_.push_back({path_text, std::string(body)});
    entry_paths_.insert(entries_.back().path);
    return true;
}

std::optional<std::string> ZipArchiveBuilder::Finish(ZipArchiveError &error_out) const
{
    error_out = {};

    auto writer = MakeZipWriter(error_out);
    if (!writer)
        return {};

    std::string out;
    if (archive_write_open(
            writer->get(), &out, &OpenStringArchive, &AppendArchiveBytes, &CloseStringArchive
        ) != ARCHIVE_OK) {
        return Fail<std::string>(
            error_out, ZipArchiveErrorCode::kOpenFailed,
            FormatArchiveDetail(writer->get(), "failed to open zip writer")
        );
    }

    for (const auto &entry_spec : entries_) {
        auto entry = MakeRegularFileEntry(entry_spec.path, error_out, entry_spec.body);
        if (!entry)
            return {};

        if (archive_write_header(writer->get(), entry.get()) != ARCHIVE_OK) {
            return Fail<std::string>(
                error_out, ZipArchiveErrorCode::kWriteHeaderFailed,
                FormatArchiveDetail(
                    writer->get(), std::string("failed to write zip header for ") + entry_spec.path
                )
            );
        }
        if (!entry_spec.body.empty()) {
            auto written = archive_write_data(
                writer->get(), entry_spec.body.data(), entry_spec.body.size()
            );
            if (written != static_cast<la_ssize_t>(entry_spec.body.size())) {
                return Fail<std::string>(
                    error_out, ZipArchiveErrorCode::kWriteDataFailed,
                    FormatArchiveDetail(
                        writer->get(),
                        std::string("failed to write zip body for ") + entry_spec.path
                    )
                );
            }
        }
    }

    if (archive_write_close(writer->get()) != ARCHIVE_OK) {
        return Fail<std::string>(
            error_out, ZipArchiveErrorCode::kFinishFailed,
            FormatArchiveDetail(writer->get(), "failed to close zip writer")
        );
    }

    error_out = {};
    return out;
}

ZipArchive::ZipArchive(
    std::map<std::string, std::string, std::less<>> files, std::vector<std::string> paths
)
    : files_(std::move(files)), paths_(std::move(paths))
{
}

std::optional<ZipArchive> ZipArchive::FromBytes(std::string_view bytes, ZipArchiveError &error_out)
{
    error_out = {};

    auto reader = MakeZipReader(error_out);
    if (!reader)
        return {};

    if (archive_read_open_memory(reader->get(), bytes.data(), bytes.size()) != ARCHIVE_OK) {
        return Fail<ZipArchive>(
            error_out, ZipArchiveErrorCode::kOpenFailed,
            FormatArchiveDetail(reader->get(), "failed to open zip archive from memory")
        );
    }

    std::map<std::string, std::string, std::less<>> files_out;
    std::vector<std::string> paths_out;
    archive_entry *entry = nullptr;

    while (true) {
        const auto rc = archive_read_next_header(reader->get(), &entry);
        if (rc == ARCHIVE_EOF)
            break;
        if (rc != ARCHIVE_OK) {
            return Fail<ZipArchive>(
                error_out, ZipArchiveErrorCode::kReadHeaderFailed,
                FormatArchiveDetail(reader->get(), "failed to read zip entry header")
            );
        }

        const auto *path_bytes = archive_entry_pathname(entry);
        if (path_bytes == nullptr) {
            return Fail<ZipArchive>(
                error_out, ZipArchiveErrorCode::kMissingPath, "zip entry is missing a pathname"
            );
        }

        const auto path = std::string_view(path_bytes);
        if (!IsAllowedZipPath(path)) {
            return Fail<ZipArchive>(
                error_out, ZipArchiveErrorCode::kInvalidPath,
                "zip entry path is not allowed: " + std::string(path)
            );
        }

        if (archive_entry_filetype(entry) != AE_IFREG) {
            return Fail<ZipArchive>(
                error_out, ZipArchiveErrorCode::kUnsupportedEntryType,
                "zip entry is not a regular file: " + std::string(path)
            );
        }
        if (archive_entry_symlink(entry) != nullptr || archive_entry_hardlink(entry) != nullptr) {
            return Fail<ZipArchive>(
                error_out, ZipArchiveErrorCode::kUnsupportedEntryType,
                "zip entry must not be a link: " + std::string(path)
            );
        }

        auto [it, inserted] = files_out.emplace(std::string(path), std::string());
        if (!inserted) {
            return Fail<ZipArchive>(
                error_out, ZipArchiveErrorCode::kDuplicateEntry,
                "duplicate zip entry: " + std::string(path)
            );
        }

        auto body = ReadEntryBytes(reader->get(), entry, error_out);
        if (!body)
            return {};
        it->second = std::move(body.value());
        paths_out.push_back(std::string(path));
    }

    error_out = {};
    return ZipArchive{std::move(files_out), std::move(paths_out)};
}

std::optional<std::string_view> ZipArchive::FindFile(std::string_view path) const noexcept
{
    auto it = files_.find(path);
    if (it == files_.end())
        return {};
    return std::string_view{it->second.data(), it->second.size()};
}

const std::vector<std::string> &ZipArchive::EntryPathsInOrder() const noexcept { return paths_; }

} // namespace arkhiv
