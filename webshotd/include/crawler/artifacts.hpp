#pragma once

#include "integers.hpp"
#include "text.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace v1::crawler {

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
    i64 jobTimeoutMs;
};

struct [[nodiscard]] WarcCdxRecord {
    String recordUrl;
    String timestamp;
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

[[nodiscard]] std::string buildSuccessStdoutLog(
    const RunRequest &run, const CapturedExchange &exchange, i64 browserPid, bool reusedBrowser
);

[[nodiscard]] WarcBuildOutput buildWarc(const CapturedExchange &exchange);

[[nodiscard]] std::string buildWacz(
    const RunRequest &run, const std::string &pagesJsonl, const WarcBuildOutput &warc,
    const std::string &stdoutLog, const std::string &stderrLog
);

} // namespace v1::crawler
