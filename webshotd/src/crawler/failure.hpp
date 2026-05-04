#pragma once

#include "crawler/fallback.hpp"
#include "text.hpp"

#include <optional>
#include <string>
#include <string_view>

#include <userver/engine/task/task_processor_fwd.hpp>

namespace ws::crawler {

namespace us = userver;
namespace eng = us::engine;
[[nodiscard]] String SanitizeProcessOutputTail(std::string_view bytes);

[[nodiscard]] std::optional<String> SummarizeProcessOutputs(
    eng::TaskProcessor &fs_task_processor, const std::string &stdout_path,
    const std::string &stderr_path
);

[[nodiscard]] String FormatAttemptContext(const AttemptSummary &attempt);

[[nodiscard]] String FormatAttemptStatus(std::string_view label, const AttemptSummary &attempt);

} // namespace ws::crawler
