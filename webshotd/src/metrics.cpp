#include "metrics.hpp"
/**
 * @file
 * @brief Service metrics exposed via userver statistics (scraped by ServerMonitor).
 */

#include <jemalloc/jemalloc.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

#include <userver/components/component.hpp>
#include <userver/components/statistics_storage.hpp>
#include <userver/utils/assert.hpp>
#include <userver/utils/statistics/labels.hpp>
#include <userver/yaml_config/merge_schemas.hpp>

namespace v1 {

namespace {

struct [[nodiscard]] ErrorLabels final {
    std::string_view subsystem;
    std::string_view op;
};

static constexpr size_t kErrorCount = 10;
static_assert(kErrorCount == numericCast<size_t>(Metrics::Error::kCount));

static constexpr std::array<ErrorLabels, kErrorCount> kErrorLabels = {{
    {"db", "capture_meta_read"},
    {"db", "capture_meta_write"},
    {"db", "shared_state_read"},
    {"db", "shared_state_write"},
    {"s3", "put_object"},
    {"s3", "delete_object"},
    {"sts", "refresh"},
    {"crawler", "run"},
    {"denylist", "check"},
    {"jemalloc", "mallctl"},
}};

[[nodiscard]] bool mallctlU64(const char *name, std::uint64_t &out) noexcept
{
    std::uint64_t value{0};
    size_t size{sizeof(value)};
    if (mallctl(name, &value, &size, nullptr, 0) != 0)
        return false;
    out = value;
    return true;
}

[[nodiscard]] bool refreshJemallocEpoch() noexcept
{
    std::uint64_t epoch{1};
    size_t size{sizeof(epoch)};
    return mallctl("epoch", &epoch, &size, &epoch, size) == 0;
}

} // namespace

Metrics::Metrics(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : us::components::ComponentBase(config, context), capture(), errors()
{
    us::utils::statistics::RegisterWriterScope(
        context, "", [this](us::utils::statistics::Writer &writer) {
            writer["capture"]["jobs_created_total"] = capture.jobsCreated.Load();

            writer["capture"]["captures_total"].ValueWithLabels(
                capture.succeeded.Load(), {{"result", "succeeded"}}
            );
            writer["capture"]["captures_total"].ValueWithLabels(
                capture.failed.Load(), {{"result", "failed"}}
            );

            writer["capture"]["duration_ms_sum"].ValueWithLabels(
                capture.succeededDurationMsSum.Load(), {{"result", "succeeded"}}
            );
            writer["capture"]["duration_ms_sum"].ValueWithLabels(
                capture.failedDurationMsSum.Load(), {{"result", "failed"}}
            );

            for (size_t i = 0; i < errors.size(); i++) {
                const auto &labels = kErrorLabels[i];
                writer["errors"]["total"].ValueWithLabels(
                    errors[i].Load(), {{"op", labels.op}, {"subsystem", labels.subsystem}}
                );
            }

            if (!refreshJemallocEpoch()) {
                accountError(Error::kJemallocMallctl);
                return;
            }

            std::uint64_t allocated{0};
            std::uint64_t resident{0};
            std::uint64_t active{0};
            const bool ok = mallctlU64("stats.allocated", allocated) &&
                            mallctlU64("stats.resident", resident) &&
                            mallctlU64("stats.active", active);
            if (!ok) {
                accountError(Error::kJemallocMallctl);
                return;
            }

            writer["memory"]["allocated_bytes"] = allocated;
            writer["memory"]["resident_bytes"] = resident;
            writer["memory"]["active_bytes"] = active;
        }
    );
}

Metrics::~Metrics() = default;

void Metrics::accountError(Error which) noexcept
{
    const auto idx = numericCast<size_t>(which);
    invariant(idx < errors.size(), "invalid Metrics::Error value");
    errors[idx]++;
}

void Metrics::accountCaptureJobCreated() noexcept { capture.jobsCreated++; }

void Metrics::accountCaptureCompleted(bool succeeded, std::chrono::milliseconds duration) noexcept
{
    invariant(duration.count() >= 0, "capture duration must be non-negative");
    const auto ms = numericCast<std::uint64_t>(duration.count());
    if (succeeded) {
        capture.succeeded++;
        capture.succeededDurationMsSum += us::utils::statistics::Rate{ms};
    } else {
        capture.failed++;
        capture.failedDurationMsSum += us::utils::statistics::Rate{ms};
    }
}

us::yaml_config::Schema Metrics::GetStaticConfigSchema()
{
    return us::yaml_config::MergeSchemas<us::components::ComponentBase>(R"(
type: object
description: Service metrics configuration
additionalProperties: false
properties: {}
)");
}

} // namespace v1
