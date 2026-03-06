#include "crawler_failure.hpp"

#include <algorithm>
#include <exception>

#include <fmt/format.h>

#include <userver/fs/blocking/read.hpp>

namespace v1::crawler {
namespace {

constexpr size_t kProcessOutputTailBytes = 4096;
constexpr size_t kProcessOutputCharsMax = 240;

[[nodiscard]] std::string escapeForQuotedValue(std::string_view input)
{
    std::string escaped;
    escaped.reserve(input.size());
    for (char ch : input) {
        if (ch == '\\' || ch == '"')
            escaped.push_back('\\');
        escaped.push_back(ch);
    }
    return escaped;
}

[[nodiscard]] std::string appendAttemptContextPieces(const AttemptSummary &attempt)
{
    std::string msg;
    if (attempt.seedProbe) {
        msg = fmt::format(
            "seedProbe status={} loadState={}", attempt.seedProbe->status.value_or(0),
            attempt.seedProbe->loadState.value_or(-1)
        );
    }
    if (attempt.failureDetail) {
        if (!msg.empty())
            msg += ", ";
        msg += std::string(attempt.failureDetail->view());
    }
    return msg;
}

} // namespace

std::string sanitizeProcessOutputTail(std::string_view bytes)
{
    const bool trimmedFront = bytes.size() > kProcessOutputTailBytes;
    if (trimmedFront)
        bytes.remove_prefix(bytes.size() - kProcessOutputTailBytes);

    std::string sanitized;
    sanitized.reserve(std::min(bytes.size(), kProcessOutputCharsMax) + 8);

    bool previousWasSpace = true;
    bool trimmedBack = false;

    for (char raw : bytes) {
        const auto byte = static_cast<unsigned char>(raw);
        char normalized = '\0';
        if (byte == '\n' || byte == '\r' || byte == '\t' || byte == ' ') {
            normalized = ' ';
        } else if (byte >= 0x20 && byte < 0x7f) {
            normalized = static_cast<char>(byte);
        } else {
            normalized = '?';
        }

        if (normalized == ' ') {
            if (previousWasSpace)
                continue;
            previousWasSpace = true;
        } else {
            previousWasSpace = false;
        }

        if (sanitized.size() >= kProcessOutputCharsMax) {
            trimmedBack = true;
            break;
        }
        sanitized.push_back(normalized);
    }

    while (!sanitized.empty() && sanitized.back() == ' ')
        sanitized.pop_back();

    if (sanitized.empty())
        return {};

    sanitized = escapeForQuotedValue(sanitized);

    if (trimmedFront)
        sanitized.insert(0, "... ");
    if (trimmedBack)
        sanitized.append(" ...");
    return sanitized;
}

std::optional<String>
summarizeProcessOutputs(const std::string &stdoutPath, const std::string &stderrPath)
{
    std::string stdoutText;
    std::string stderrText;

    try {
        stdoutText = sanitizeProcessOutputTail(userver::fs::blocking::ReadFileContents(stdoutPath));
    } catch (const std::exception &) {
    }

    try {
        stderrText = sanitizeProcessOutputTail(userver::fs::blocking::ReadFileContents(stderrPath));
    } catch (const std::exception &) {
    }

    if (stdoutText.empty() && stderrText.empty())
        return {};

    std::string detail;
    if (!stdoutText.empty())
        detail = fmt::format("stdout=\"{}\"", stdoutText);
    if (!stderrText.empty()) {
        if (!detail.empty())
            detail += ", ";
        detail += fmt::format("stderr=\"{}\"", stderrText);
    }
    return String::fromBytesThrow(detail);
}

String formatAttemptContext(const AttemptSummary &attempt)
{
    return String::fromBytesThrow(appendAttemptContextPieces(attempt));
}

String formatAttemptStatus(std::string_view label, const AttemptSummary &attempt)
{
    std::string msg;
    if (label.empty()) {
        msg = fmt::format(
            "exit code {}: {}", attempt.exitCode, crawlerFailureReason(attempt.exitCode)
        );
    } else {
        msg = fmt::format(
            "{} exit code {}: {}", label, attempt.exitCode, crawlerFailureReason(attempt.exitCode)
        );
    }

    const auto context = appendAttemptContextPieces(attempt);
    if (!context.empty())
        msg += fmt::format(", {}", context);
    return String::fromBytesThrow(msg);
}

} // namespace v1::crawler
