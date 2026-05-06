#pragma once

#include "crawler/fallback.hpp"
#include "integers.hpp"
#include "text.hpp"

#include <optional>
#include <string>
#include <string_view>

#include <userver/engine/task/task_processor_fwd.hpp>

namespace ws::crawler {

namespace us = userver;
namespace eng = us::engine;
[[nodiscard]] std::string RetainLogHeadAndTail(std::string bytes, i64 limit_bytes);

[[nodiscard]] String RetainProcessOutputText(std::string_view bytes);

[[nodiscard]] std::optional<String> FormatProcessOutputDiagnostics(
    eng::TaskProcessor &fs_task_processor, const std::string &stdout_path,
    const std::string &stderr_path
);

[[nodiscard]] String FormatCrawlerError(const CrawlerError &error);

} // namespace ws::crawler
