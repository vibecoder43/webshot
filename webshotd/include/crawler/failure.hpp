#pragma once

#include "crawler/fallback.hpp"
#include "text.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace v1::crawler {

[[nodiscard]] String sanitizeProcessOutputTail(std::string_view bytes);

[[nodiscard]] std::optional<String>
summarizeProcessOutputs(const std::string &stdoutPath, const std::string &stderrPath);

[[nodiscard]] String formatAttemptContext(const AttemptSummary &attempt);

[[nodiscard]] String formatAttemptStatus(std::string_view label, const AttemptSummary &attempt);

} // namespace v1::crawler
