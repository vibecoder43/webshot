#include "crawler/cgroup_stats.hpp"

#include "integers.hpp"
#include "try.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <unordered_map>

#include <userver/fs/read.hpp>
#include <userver/utils/text_light.hpp>

namespace ws::crawler {
namespace us = userver;
namespace eng = us::engine;
using namespace text::literals;

namespace {

using StatMap = std::unordered_map<std::string, int64_t>;

[[nodiscard]] Expected<int64_t, String> ParseStatValue(std::string_view value)
{
    auto trimmed = us::utils::text::TrimView(value);
    auto parsed = integers::Parse<int64_t>(trimmed);
    if (!parsed)
        return Unex(text::Format("invalid cgroup integer '{}'", value));
    return *parsed;
}

template <typename F> Expected<void, String> ForEachLine(std::string_view text, F &&handle_line)
{
    while (true) {
        auto next = text.find('\n');
        auto line = us::utils::text::TrimView(
            next == std::string_view::npos ? text : text.substr(0, next)
        );
        if (!line.empty())
            TRY(handle_line(line));
        if (next == std::string_view::npos)
            return {};
        text.remove_prefix(next + 1);
    }
}

[[nodiscard]] int64_t ValueOrZero(const StatMap &stats, const std::string &key)
{
    auto it = stats.find(key);
    if (it == std::end(stats))
        return 0;
    return it->second;
}

Expected<void, String> ParseFlatStatLine(StatMap &stats, std::string_view line)
{
    auto split = line.find(' ');
    if (split == std::string_view::npos)
        return {};
    auto key = std::string(line.substr(0, split));
    stats[key] = TRY(ParseStatValue(line.substr(split + 1)));
    return {};
}

[[nodiscard]] Expected<StatMap, String> ParseFlatStat(std::string_view text)
{
    StatMap stats;
    TRY(ForEachLine(text, [&stats](std::string_view line) {
        return ParseFlatStatLine(stats, line);
    }));
    return stats;
}

Expected<void, String> ParseIoStatLine(StatMap &stats, std::string_view line)
{
    auto tokens = us::utils::text::SplitIntoStringViewVector(line, " ");
    for (size_t i = 1; i < tokens.size(); i++) {
        auto token = tokens[i];
        auto colon = token.find(':');
        if (colon != std::string_view::npos) {
            auto key = std::string(token.substr(0, colon));
            stats[key] += TRY(ParseStatValue(token.substr(colon + 1)));
        }
    }
    return {};
}

[[nodiscard]] Expected<StatMap, String> ParseIoStat(std::string_view text)
{
    StatMap stats;
    TRY(ForEachLine(text, [&stats](std::string_view line) {
        return ParseIoStatLine(stats, line);
    }));
    return stats;
}

[[nodiscard]] Expected<std::string, String>
ReadOptionalFile(eng::TaskProcessor &fs_task_processor, const std::string &path)
{
    try {
        return us::fs::ReadFileContents(fs_task_processor, path);
    } catch (const std::runtime_error &e) {
        return Unex(*String::FromBytes(e.what()));
    }
}

void ApplyCpuStats(CgroupStats &out, const StatMap &stats) noexcept
{
    out.cpu_usage_usec = ValueOrZero(stats, "usage_usec");
    out.cpu_user_usec = ValueOrZero(stats, "user_usec");
    out.cpu_system_usec = ValueOrZero(stats, "system_usec");
}

void ApplyMemoryEvents(CgroupStats &out, const StatMap &stats) noexcept
{
    out.memory_oom = ValueOrZero(stats, "oom");
    out.memory_oom_kill = ValueOrZero(stats, "oom_kill");
    out.memory_oom_group_kill = ValueOrZero(stats, "oom_group_kill");
}

void ApplyIoStats(CgroupStats &out, const StatMap &stats) noexcept
{
    out.io_read_bytes = ValueOrZero(stats, "rbytes");
    out.io_write_bytes = ValueOrZero(stats, "wbytes");
    out.io_read_ops = ValueOrZero(stats, "rios");
    out.io_write_ops = ValueOrZero(stats, "wios");
}

[[nodiscard]] Expected<void, String>
ApplySnapshotSection(CgroupStats &out, std::string_view section, std::string_view body)
{
    if (section == "cpu.stat") {
        ApplyCpuStats(out, TRY(ParseFlatStat(body)));
    } else if (section == "memory.current") {
        out.memory_current = TRY(ParseStatValue(body));
    } else if (section == "memory.events") {
        ApplyMemoryEvents(out, TRY(ParseFlatStat(body)));
    } else if (section == "io.stat") {
        ApplyIoStats(out, TRY(ParseIoStat(body)));
    }
    return {};
}

} // namespace

CgroupStats operator+(const CgroupStats &lhs, const CgroupStats &rhs) noexcept
{
    return {
        .cpu_usage_usec = lhs.cpu_usage_usec + rhs.cpu_usage_usec,
        .cpu_user_usec = lhs.cpu_user_usec + rhs.cpu_user_usec,
        .cpu_system_usec = lhs.cpu_system_usec + rhs.cpu_system_usec,
        .memory_current = lhs.memory_current + rhs.memory_current,
        .memory_oom = lhs.memory_oom + rhs.memory_oom,
        .memory_oom_kill = lhs.memory_oom_kill + rhs.memory_oom_kill,
        .memory_oom_group_kill = lhs.memory_oom_group_kill + rhs.memory_oom_group_kill,
        .io_read_bytes = lhs.io_read_bytes + rhs.io_read_bytes,
        .io_write_bytes = lhs.io_write_bytes + rhs.io_write_bytes,
        .io_read_ops = lhs.io_read_ops + rhs.io_read_ops,
        .io_write_ops = lhs.io_write_ops + rhs.io_write_ops,
    };
}

bool HasBrowserOomKill(const CgroupStats &stats) noexcept
{
    return stats.memory_oom_kill > 0 || stats.memory_oom_group_kill > 0;
}

Expected<CgroupStats, String>
ReadCgroupStats(eng::TaskProcessor &fs_task_processor, const std::string &cgroup_path)
{
    CgroupStats out;

    if (auto cpu = ReadOptionalFile(fs_task_processor, cgroup_path + "/cpu.stat")) {
        ApplyCpuStats(out, TRY(ParseFlatStat(*cpu)));
    }
    if (auto memory_current =
            ReadOptionalFile(fs_task_processor, cgroup_path + "/memory.current")) {
        out.memory_current = TRY(ParseStatValue(*memory_current));
    }
    if (auto events = ReadOptionalFile(fs_task_processor, cgroup_path + "/memory.events")) {
        ApplyMemoryEvents(out, TRY(ParseFlatStat(*events)));
    }
    if (auto io = ReadOptionalFile(fs_task_processor, cgroup_path + "/io.stat")) {
        ApplyIoStats(out, TRY(ParseIoStat(*io)));
    }

    return out;
}

Expected<CgroupStats, String> ParseCgroupStatsSnapshot(const std::string &snapshot)
{
    CgroupStats out;
    std::string_view section;
    std::string body;

    TRY(ForEachLine(snapshot, [&](std::string_view line) -> Expected<void, String> {
        if (us::utils::text::StartsWith(line, "[") && us::utils::text::EndsWith(line, "]")) {
            if (!section.empty()) {
                TRY(ApplySnapshotSection(out, section, body));
                body.clear();
            }
            section = line.substr(1, line.size() - 2);
            return {};
        }
        if (!section.empty()) {
            body.append(line);
            body.push_back('\n');
        }
        return {};
    }));
    if (!section.empty()) {
        TRY(ApplySnapshotSection(out, section, body));
    }
    return out;
}

String FormatCgroupStats(const CgroupStats &stats)
{
    return text::Format(
        "cpu_usage_usec={}, cpu_user_usec={}, cpu_system_usec={}, "
        "memory_current={}, memory_oom={}, memory_oom_kill={}, "
        "memory_oom_group_kill={}, io_read_bytes={}, io_write_bytes={}, io_read_ops={}, "
        "io_write_ops={}",
        stats.cpu_usage_usec, stats.cpu_user_usec, stats.cpu_system_usec, stats.memory_current,
        stats.memory_oom, stats.memory_oom_kill, stats.memory_oom_group_kill, stats.io_read_bytes,
        stats.io_write_bytes, stats.io_read_ops, stats.io_write_ops
    );
}

} // namespace ws::crawler
