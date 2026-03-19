#include "crawler/failure.hpp"

#include <algorithm>
#include <exception>

#include <fmt/format.h>

#include <userver/fs/blocking/read.hpp>

namespace us = userver;

namespace v1::crawler {
using namespace text::literals;

namespace {

constexpr size_t kProcessOutputTailBytes = 4096UL;
constexpr size_t kProcessOutputCharsMax = 240UL;

[[nodiscard]] String escapeForQuotedValue(std::string_view input)
{
    std::string escaped;
    escaped.reserve(input.size());
    for (char ch : input) {
        if (ch == '\\' || ch == '"')
            escaped.push_back('\\');
        escaped.push_back(ch);
    }
    return String::fromBytesThrow(escaped);
}

[[nodiscard]] std::optional<String> readSanitizedProcessOutput(const std::string &path)
{
    try {
        auto text = sanitizeProcessOutputTail(us::fs::blocking::ReadFileContents(path));
        if (!text.empty())
            return text;
    } catch (const std::exception &) {
        // Best-effort diagnostics: ignore unreadable or missing process-output files.
    }
    return {};
}

} // namespace

String sanitizeProcessOutputTail(std::string_view bytes)
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

    auto escaped = escapeForQuotedValue(sanitized);
    if (trimmedFront)
        escaped = "... "_t + escaped;
    if (trimmedBack)
        escaped += " ..."_t;
    return escaped;
}

std::optional<String>
summarizeProcessOutputs(const std::string &stdoutPath, const std::string &stderrPath)
{
    const auto stdoutText = readSanitizedProcessOutput(stdoutPath);
    const auto stderrText = readSanitizedProcessOutput(stderrPath);

    if (!stdoutText && !stderrText)
        return {};

    String detail;
    if (stdoutText)
        detail = text::format("stdout=\"{}\"", stdoutText.value());
    if (stderrText) {
        if (!detail.empty())
            detail += ", "_t;
        detail += text::format("stderr=\"{}\"", stderrText.value());
    }
    return detail;
}

String formatAttemptContext(const AttemptSummary &attempt)
{
    String msg;
    if (attempt.seedProbe) {
        msg = text::format(
            "seedProbe status={} loadState={}", attempt.seedProbe->status.value_or(0),
            attempt.seedProbe->loadState.value_or(-1)
        );
    }
    if (attempt.failureDetail) {
        if (!msg.empty())
            msg += ", "_t;
        msg += attempt.failureDetail.value();
    }
    return msg;
}

String formatAttemptStatus(std::string_view label, const AttemptSummary &attempt)
{
    String msg;
    if (label.empty()) {
        msg = text::format(
            "exit code {}: {}", attempt.exitCode, crawlerFailureReason(attempt.exitCode)
        );
    } else {
        msg = text::format(
            "{} exit code {}: {}", label, attempt.exitCode, crawlerFailureReason(attempt.exitCode)
        );
    }

    const auto context = formatAttemptContext(attempt);
    if (!context.empty())
        msg += ", "_t + context;
    return msg;
}

} // namespace v1::crawler
