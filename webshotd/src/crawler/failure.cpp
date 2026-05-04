#include "crawler/failure.hpp"

#include <algorithm>
#include <stdexcept>

#include <userver/engine/exception.hpp>
#include <userver/fs/read.hpp>

namespace ws::crawler {
namespace us = userver;
namespace eng = us::engine;
using namespace text::literals;

namespace {

constexpr size_t kProcessOutputTailBytes = 4096UL;
constexpr size_t kProcessOutputCharsMax = 240UL;

[[nodiscard]] String EscapeForQuotedValue(std::string_view input)
{
    std::string escaped;
    escaped.reserve(input.size());
    for (char ch : input) {
        if (ch == '\\' || ch == '"')
            escaped.push_back('\\');
        escaped.push_back(ch);
    }
    return String::FromBytes(escaped).Expect();
}

[[nodiscard]] std::optional<String>
ReadSanitizedProcessOutput(eng::TaskProcessor &fs_task_processor, const std::string &path)
{
    try {
        auto text = SanitizeProcessOutputTail(us::fs::ReadFileContents(fs_task_processor, path));
        if (!text.Empty())
            return text;
    } catch (const std::runtime_error &) {
        // Best-effort diagnostics: ignore unreadable or missing process-output files.
    }
    return {};
}

} // namespace

String SanitizeProcessOutputTail(std::string_view bytes)
{
    const bool trimmed_front = bytes.size() > kProcessOutputTailBytes;
    if (trimmed_front)
        bytes.remove_prefix(bytes.size() - kProcessOutputTailBytes);

    std::string sanitized;
    sanitized.reserve(std::min(bytes.size(), kProcessOutputCharsMax) + 8);

    bool previous_was_space = true;
    bool trimmed_back = false;

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
            if (previous_was_space)
                continue;
            previous_was_space = true;
        } else {
            previous_was_space = false;
        }

        if (sanitized.size() >= kProcessOutputCharsMax) {
            trimmed_back = true;
            break;
        }
        sanitized.push_back(normalized);
    }

    while (!sanitized.empty() && sanitized.back() == ' ')
        sanitized.pop_back();

    if (sanitized.empty())
        return {};

    auto escaped = EscapeForQuotedValue(sanitized);
    if (trimmed_front)
        escaped = "... "_t + escaped;
    if (trimmed_back)
        escaped += " ..."_t;
    return escaped;
}

std::optional<String> SummarizeProcessOutputs(
    eng::TaskProcessor &fs_task_processor, const std::string &stdout_path,
    const std::string &stderr_path
)
{
    const auto stdout_text = ReadSanitizedProcessOutput(fs_task_processor, stdout_path);
    const auto stderr_text = ReadSanitizedProcessOutput(fs_task_processor, stderr_path);

    if (!stdout_text && !stderr_text)
        return {};

    String detail;
    if (stdout_text)
        detail = text::Format("stdout=\"{}\"", *stdout_text);
    if (stderr_text) {
        if (!detail.Empty())
            detail += ", "_t;
        detail += text::Format("stderr=\"{}\"", *stderr_text);
    }
    return detail;
}

String FormatAttemptContext(const AttemptSummary &attempt)
{
    String msg;
    if (attempt.seed_probe) {
        msg = text::Format(
            "seedProbe status={} loadState={}", attempt.seed_probe->status.value_or(0),
            attempt.seed_probe->load_state.value_or(-1)
        );
    }
    if (attempt.failure_detail) {
        if (!msg.Empty())
            msg += ", "_t;
        msg += *attempt.failure_detail;
    }
    return msg;
}

String FormatAttemptStatus(std::string_view label, const AttemptSummary &attempt)
{
    String msg;
    if (label.empty()) {
        msg = text::Format(
            "exit code {}: {}", attempt.exit_code, CrawlerFailureReason(attempt.exit_code)
        );
    } else {
        msg = text::Format(
            "{} exit code {}: {}", label, attempt.exit_code, CrawlerFailureReason(attempt.exit_code)
        );
    }

    const auto context = FormatAttemptContext(attempt);
    if (!context.Empty())
        msg += ", "_t + context;
    return msg;
}

} // namespace ws::crawler
