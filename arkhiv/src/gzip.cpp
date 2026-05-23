#include "arkhiv/gzip.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace arkhiv {

namespace {

using ArchiveWriterPtr = std::unique_ptr<archive, decltype(&archive_write_free)>;
using ArchiveEntryPtr = std::unique_ptr<archive_entry, decltype(&archive_entry_free)>;

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

int OpenStringArchive(archive *, void *) { return ARCHIVE_OK; }

la_ssize_t AppendArchiveBytes(archive *, void *ctx, const void *buffer, size_t nbytes)
{
    auto &out = *static_cast<std::string *>(ctx);
    out.append(static_cast<const char *>(buffer), nbytes);
    return static_cast<la_ssize_t>(nbytes);
}

int CloseStringArchive(archive *, void *) { return ARCHIVE_OK; }

[[nodiscard]] ArchiveEntryPtr MakeEntry(size_t size)
{
    auto entry = ArchiveEntryPtr(archive_entry_new(), &archive_entry_free);
    if (!entry)
        return {nullptr, &archive_entry_free};

    archive_entry_set_pathname(entry.get(), "member");
    archive_entry_set_filetype(entry.get(), AE_IFREG);
    archive_entry_set_perm(entry.get(), 0644);
    archive_entry_set_size(entry.get(), static_cast<la_int64_t>(size));
    return entry;
}

} // namespace

std::optional<std::string> GzipCompressMember(std::string_view body, GzipError &error_out)
{
    error_out = {};

    auto *writer = archive_write_new();
    if (writer == nullptr) {
        error_out.code = GzipErrorCode::kWriterInitFailed;
        error_out.detail = "failed to allocate gzip writer";
        return {};
    }

    auto writer_ptr = ArchiveWriterPtr(writer, &archive_write_free);

    if (archive_write_set_format_raw(writer_ptr.get()) != ARCHIVE_OK) {
        error_out.code = GzipErrorCode::kWriterInitFailed;
        error_out.detail = FormatArchiveDetail(writer_ptr.get(), "failed to enable raw output");
        return {};
    }
    if (archive_write_add_filter_gzip(writer_ptr.get()) != ARCHIVE_OK) {
        error_out.code = GzipErrorCode::kWriterInitFailed;
        error_out.detail = FormatArchiveDetail(writer_ptr.get(), "failed to enable gzip filter");
        return {};
    }

    std::string out;
    if (archive_write_open(
            writer_ptr.get(), &out, &OpenStringArchive, &AppendArchiveBytes, &CloseStringArchive
        ) != ARCHIVE_OK) {
        error_out.code = GzipErrorCode::kOpenFailed;
        error_out.detail = FormatArchiveDetail(writer_ptr.get(), "failed to open gzip writer");
        return {};
    }

    auto entry = MakeEntry(body.size());
    if (!entry) {
        error_out.code = GzipErrorCode::kWriterInitFailed;
        error_out.detail = "failed to allocate gzip entry";
        return {};
    }

    if (archive_write_header(writer_ptr.get(), entry.get()) != ARCHIVE_OK) {
        error_out.code = GzipErrorCode::kWriteHeaderFailed;
        error_out.detail = FormatArchiveDetail(writer_ptr.get(), "failed to write gzip header");
        return {};
    }

    if (!body.empty()) {
        auto written = archive_write_data(
            writer_ptr.get(), body.data(), static_cast<size_t>(body.size())
        );
        if (written != static_cast<la_ssize_t>(body.size())) {
            error_out.code = GzipErrorCode::kWriteDataFailed;
            error_out.detail = FormatArchiveDetail(writer_ptr.get(), "failed to write gzip body");
            return {};
        }
    }

    if (archive_write_close(writer_ptr.get()) != ARCHIVE_OK) {
        error_out.code = GzipErrorCode::kFinishFailed;
        error_out.detail = FormatArchiveDetail(writer_ptr.get(), "failed to close gzip writer");
        return {};
    }

    error_out = {};
    return out;
}

} // namespace arkhiv
