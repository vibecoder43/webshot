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

namespace ws::crawler {

using ws::Expected;

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
    String redirect_url;
    i64 status_code{0};
    String status_message;
    std::unordered_map<std::string, std::string> headers;
    String timestamp;
};

struct [[nodiscard]] CapturedResource {
    String resource_url;
    String method;
    std::optional<String> resource_type;
    i64 status_code{0};
    String status_message;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    String timestamp;
};

struct [[nodiscard]] CapturedExchange {
    String seed_url;
    String page_id;
    String final_url;
    i64 status_code{0};
    String status_message;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    String timestamp;
    std::vector<String> redirect_chain;
    std::vector<CapturedMainDocumentRedirect> main_document_redirects;
    std::vector<CapturedResource> resources;
    std::optional<String> title;
};

struct [[nodiscard]] RunRequest {
    String seed_url;
};

struct [[nodiscard]] WarcCdxRecord {
    String record_url;
    String timestamp;
    String digest;
    String record_digest;
    i64 status_code{0};
    std::unordered_map<std::string, std::string> headers;
    i64 offset{0};
    i64 length{0};
};

struct [[nodiscard]] WarcBuildOutput {
    std::string bytes;
    std::vector<WarcCdxRecord> cdx_records;
};

[[nodiscard]] std::string BuildPagesJsonl(const CapturedExchange &exchange);

[[nodiscard]] Expected<std::string, ArtifactFailure> BuildSuccessStdoutLog(
    const RunRequest &run, const CapturedExchange &exchange, i64 browser_pid,
    ReusedBrowser reused_browser
);

[[nodiscard]] Expected<WarcBuildOutput, ArtifactFailure>
BuildWarc(const CapturedExchange &exchange);

[[nodiscard]] Expected<std::string, ArtifactFailure> BuildWacz(
    const RunRequest &run, const std::string &pages_jsonl, const WarcBuildOutput &warc,
    const std::string &stdout_log, const std::string &stderr_log
);

/**
 * @brief Compute a stable content fingerprint for deduplication.
 *
 * This hash is derived from the captured exchange (URLs, methods, status codes,
 * selected headers, and body digests) and intentionally excludes timestamps and
 * other non-deterministic fields.
 */
[[nodiscard]] std::string ComputeContentSha256(const CapturedExchange &exchange);

} // namespace ws::crawler
