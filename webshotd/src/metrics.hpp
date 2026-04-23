#pragma once

#include "integers.hpp"
#include "userver_namespaces.hpp"

#include <array>
#include <chrono>
#include <cstdint>

#include <userver/components/component_base.hpp>
#include <userver/components/component_config.hpp>
#include <userver/components/component_context.hpp>
#include <userver/utils/statistics/rate_counter.hpp>
#include <userver/utils/statistics/relaxed_counter.hpp>
#include <userver/yaml_config/schema.hpp>

namespace v1 {

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

    void accountError(Error which) noexcept;

    void accountCaptureJobCreated() noexcept;

    void accountCaptureCompleted(bool succeeded, std::chrono::milliseconds duration) noexcept;

    static us::yaml_config::Schema GetStaticConfigSchema();

private:
    struct [[nodiscard]] CaptureCounters {
        us::utils::statistics::RateCounter jobsCreated;

        us::utils::statistics::RateCounter succeeded;
        us::utils::statistics::RateCounter failed;

        us::utils::statistics::RateCounter succeededDurationMsSum;
        us::utils::statistics::RateCounter failedDurationMsSum;
    };

    static constexpr size_t kErrorCount = numericCast<size_t>(Error::kCount);

    CaptureCounters capture;
    std::array<us::utils::statistics::RateCounter, kErrorCount> errors;
};

} // namespace v1
