#pragma once

#include "expected.hpp"
#include "text.hpp"

#include <cstdint>
#include <string>

#include <userver/engine/task/task_processor_fwd.hpp>

namespace ws::crawler {

namespace us = userver;
namespace eng = us::engine;

struct [[nodiscard]] CgroupStats final {
    int64_t cpu_usage_usec{0};
    int64_t cpu_user_usec{0};
    int64_t cpu_system_usec{0};

    int64_t memory_current{0};
    int64_t memory_oom{0};
    int64_t memory_oom_kill{0};
    int64_t memory_oom_group_kill{0};

    int64_t io_read_bytes{0};
    int64_t io_write_bytes{0};
    int64_t io_read_ops{0};
    int64_t io_write_ops{0};
};

[[nodiscard]] CgroupStats operator+(const CgroupStats &lhs, const CgroupStats &rhs) noexcept;

[[nodiscard]] bool HasBrowserOomKill(const CgroupStats &stats) noexcept;

[[nodiscard]] Expected<CgroupStats, String>
ReadCgroupStats(eng::TaskProcessor &fs_task_processor, const std::string &cgroup_path);

[[nodiscard]] Expected<CgroupStats, String> ParseCgroupStatsSnapshot(const std::string &snapshot);

[[nodiscard]] String FormatCgroupStats(const CgroupStats &stats);

} // namespace ws::crawler
