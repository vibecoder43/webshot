#include "failure.hpp"

#include "integers.hpp"

#include <stdexcept>

#include <userver/engine/exception.hpp>
#include <userver/fs/read.hpp>

namespace ws::crawler {
namespace us = userver;
namespace eng = us::engine;
using namespace text::literals;

namespace {

constexpr size_t kProcessOutputRetainedBytes = 4096UL;
constexpr std::string_view kSkippedMarker = "\n...skipped...\n";

[[nodiscard]] std::optional<String>
ReadProcessOutputText(eng::TaskProcessor &fs_task_processor, const std::string &path)
{
    try {
        auto text = RetainProcessOutputText(us::fs::ReadFileContents(fs_task_processor, path));
        if (!text.Empty())
            return text;
    } catch (const std::runtime_error &) {
        // Best-effort diagnostics: ignore unreadable or missing process-output files.
    }
    return {};
}

} // namespace

std::string RetainLogHeadAndTail(std::string bytes, i64 limit_bytes)
{
    if (limit_bytes <= 0_i64)
        return {};

    const auto limit = NumericCast<size_t>(limit_bytes);
    if (bytes.size() <= limit)
        return bytes;

    const auto head_size = limit / 2;
    const auto tail_size = limit - head_size;
    std::string retained;
    retained.reserve(head_size + kSkippedMarker.size() + tail_size);
    retained.append(bytes.data(), head_size);
    retained.append(kSkippedMarker);
    retained.append(bytes.data() + bytes.size() - tail_size, tail_size);
    return retained;
}

String RetainProcessOutputText(std::string_view bytes)
{
    const auto retained = RetainLogHeadAndTail(
        std::string(bytes), i64{kProcessOutputRetainedBytes}
    );
    return *String::FromBytes(retained);
}

std::optional<String> FormatProcessOutputDiagnostics(
    eng::TaskProcessor &fs_task_processor, const std::string &stdout_path,
    const std::string &stderr_path
)
{
    const auto stdout_text = ReadProcessOutputText(fs_task_processor, stdout_path);
    const auto stderr_text = ReadProcessOutputText(fs_task_processor, stderr_path);

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

String FormatCrawlerError(const CrawlerError &error)
{
    String msg = CrawlerErrorKindText(error.kind);
    if (error.seed_probe) {
        msg += text::Format(
            ", seedProbe status={} loadState={}", error.seed_probe->status.value_or(0),
            error.seed_probe->load_state.value_or(-1)
        );
    }
    if (error.detail) {
        if (!msg.Empty())
            msg += ", "_t;
        msg += *error.detail;
    }
    if (error.cgroup_stats) {
        msg += text::Format(", cgroup={}", FormatCgroupStats(*error.cgroup_stats));
    }
    return msg;
}

} // namespace ws::crawler
