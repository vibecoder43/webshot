#pragma once

#include "integers.hpp"

#include <array>
#include <chrono>
#include <cstdint>

#include <userver/components/component_base.hpp>
#include <userver/components/component_config.hpp>
#include <userver/components/component_context.hpp>
#include <userver/utils/statistics/rate_counter.hpp>
#include <userver/utils/statistics/relaxed_counter.hpp>
#include <userver/yaml_config/schema.hpp>

namespace ws {

namespace us = userver;
class [[nodiscard]] Metrics final : public us::components::ComponentBase {
public:
    static constexpr std::string_view kName = "metrics";

    explicit Metrics(
        const us::components::ComponentConfig &config,
        const us::components::ComponentContext &context
    );

    ~Metrics() override;

    enum class Error : uint8_t {
        kDbCaptureMetaRead = 0,
        kDbCaptureMetaWrite = 1,
        kDbSharedStateRead = 2,
        kDbSharedStateWrite = 3,
        kS3PutObject = 4,
        kS3DeleteObject = 5,
        kStsRefresh = 6,
        kCrawlerRun = 7,
        kDenylistCheck = 8,
        kCount = 9,
    };

    void AccountError(Error which) noexcept;

    void AccountCaptureJobCreated() noexcept;

    void AccountCaptureCompleted(bool succeeded, std::chrono::milliseconds duration) noexcept;

    static us::yaml_config::Schema GetStaticConfigSchema();

private:
    struct [[nodiscard]] CaptureCounters {
        us::utils::statistics::RateCounter jobs_created;

        us::utils::statistics::RateCounter succeeded;
        us::utils::statistics::RateCounter failed;

        us::utils::statistics::RateCounter succeeded_duration_ms_sum;
        us::utils::statistics::RateCounter failed_duration_ms_sum;
    };

    static constexpr size_t kErrorCount = NumericCast<size_t>(Error::kCount);

    CaptureCounters capture_;
    std::array<us::utils::statistics::RateCounter, kErrorCount> errors_;
};

} // namespace ws
