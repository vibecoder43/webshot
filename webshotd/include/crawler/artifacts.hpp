#pragma once

#include "expected.hpp"
#include "integers.hpp"
#include "text.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace v1::crawler {

using v1::Expected;

enum class ReusedBrowser {
    kNo,
    kYes,
};

enum class ArtifactError {
    kGzipFailed,
    kZipFailed,
};

struct [[nodiscard]] ArtifactFailure {
    ArtifactError code;
    std::string detail;
};

struct [[nodiscard]] CapturedMainDocumentRedirect {
    String redirectUrl;
    i64 statusCode{0};
    String statusMessage;
    std::unordered_map<std::string, std::string> headers;
    String timestamp;
};

struct [[nodiscard]] CapturedResource {
    String resourceUrl;
    String method;
    std::optional<String> resourceType;
    i64 statusCode{0};
    String statusMessage;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    String timestamp;
};

struct [[nodiscard]] CapturedExchange {
    String seedUrl;
    String pageId;
    String finalUrl;
    i64 statusCode{0};
    String statusMessage;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    String timestamp;
    std::vector<String> redirectChain;
    std::vector<CapturedMainDocumentRedirect> mainDocumentRedirects;
    std::vector<CapturedResource> resources;
    std::optional<String> title;
};

struct [[nodiscard]] RunRequest {
    String seedUrl;
};

struct [[nodiscard]] WarcCdxRecord {
    String recordUrl;
    String timestamp;
    String digest;
    String recordDigest;
    i64 statusCode{0};
    std::unordered_map<std::string, std::string> headers;
    i64 offset{0};
    i64 length{0};
};

struct [[nodiscard]] WarcBuildOutput {
    std::string bytes;
    std::vector<WarcCdxRecord> cdxRecords;
};

[[nodiscard]] std::string buildPagesJsonl(const CapturedExchange &exchange);

[[nodiscard]] Expected<std::string, ArtifactFailure> buildSuccessStdoutLog(
    const RunRequest &run, const CapturedExchange &exchange, i64 browserPid,
    ReusedBrowser reusedBrowser
);

[[nodiscard]] Expected<WarcBuildOutput, ArtifactFailure>
buildWarc(const CapturedExchange &exchange);

[[nodiscard]] Expected<std::string, ArtifactFailure> buildWacz(
    const RunRequest &run, const std::string &pagesJsonl, const WarcBuildOutput &warc,
    const std::string &stdoutLog, const std::string &stderrLog
);

/**
 * @brief Compute a stable content fingerprint for deduplication.
 *
 * This hash is derived from the captured exchange (URLs, methods, status codes,
 * selected headers, and body digests) and intentionally excludes timestamps and
 * other non-deterministic fields.
 */
[[nodiscard]] std::string computeContentSha256(const CapturedExchange &exchange);

} // namespace v1::crawler
