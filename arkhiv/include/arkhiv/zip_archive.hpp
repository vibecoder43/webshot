#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace arkhiv {

enum class ZipArchiveErrorCode : uint8_t {
    kNone,
    kWriterInitFailed,
    kReaderInitFailed,
    kOpenFailed,
    kMissingPath,
    kInvalidPath,
    kUnsupportedEntryType,
    kInvalidEntrySize,
    kEntryTooLarge,
    kReadDataFailed,
    kTruncatedEntry,
    kDuplicateEntry,
    kReadHeaderFailed,
    kWriteHeaderFailed,
    kWriteDataFailed,
    kFinishFailed,
};

struct [[nodiscard]] ZipArchiveError {
    ZipArchiveErrorCode code = ZipArchiveErrorCode::kNone;
    std::string detail;
};

class [[nodiscard]] ZipArchiveBuilder {
public:
    [[nodiscard]] bool
    addStoredFile(std::string_view path, ZipArchiveError &errorOut, std::string_view body);

    [[nodiscard]] std::optional<std::string> finish(ZipArchiveError &errorOut) const;

private:
    struct [[nodiscard]] StoredEntry {
        std::string path;
        std::string body;
    };

    std::vector<StoredEntry> entries;
    std::set<std::string, std::less<>> entryPaths;
};

class [[nodiscard]] ZipArchive {
public:
    [[nodiscard]] static std::optional<ZipArchive>
    fromBytes(std::string_view bytes, ZipArchiveError &errorOut);

    [[nodiscard]] std::optional<std::string_view> findFile(std::string_view path) const noexcept;

    [[nodiscard]] const std::vector<std::string> &entryPathsInOrder() const noexcept;

private:
    ZipArchive(
        std::map<std::string, std::string, std::less<>> filesIn, std::vector<std::string> pathsIn
    );

    std::map<std::string, std::string, std::less<>> files;
    std::vector<std::string> paths;
};

} // namespace arkhiv
