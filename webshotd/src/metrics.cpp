#include "metrics.hpp"
#include "invariant.hpp"
/**
 * @file
 * @brief Service metrics exposed via userver statistics (scraped by ServerMonitor).
 */

#include <cstddef>
#include <userver/components/component.hpp>
#include <userver/components/statistics_storage.hpp>
#include <userver/utils/statistics/labels.hpp>
#include <userver/yaml_config/merge_schemas.hpp>

namespace ws {

namespace us = userver;
using namespace text::literals;

namespace {

template <typename Value>
void WriteErrorMetric(
    us::utils::statistics::Writer &writer, Metrics::Error which, Value value
) noexcept
{
    using enum Metrics::Error;

    switch (which) {
    case kDbCaptureMetaRead:
        writer["errors"]["total"].ValueWithLabels(
            value, {{"op", "capture_meta_read"}, {"subsystem", "db"}}
        );
        return;
    case kDbCaptureMetaWrite:
        writer["errors"]["total"].ValueWithLabels(
            value, {{"op", "capture_meta_write"}, {"subsystem", "db"}}
        );
        return;
    case kDbSharedStateRead:
        writer["errors"]["total"].ValueWithLabels(
            value, {{"op", "shared_state_read"}, {"subsystem", "db"}}
        );
        return;
    case kDbSharedStateWrite:
        writer["errors"]["total"].ValueWithLabels(
            value, {{"op", "shared_state_write"}, {"subsystem", "db"}}
        );
        return;
    case kS3PutObject:
        writer["errors"]["total"].ValueWithLabels(
            value, {{"op", "put_object"}, {"subsystem", "s3"}}
        );
        return;
    case kS3DeleteObject:
        writer["errors"]["total"].ValueWithLabels(
            value, {{"op", "delete_object"}, {"subsystem", "s3"}}
        );
        return;
    case kStsRefresh:
        writer["errors"]["total"].ValueWithLabels(value, {{"op", "refresh"}, {"subsystem", "sts"}});
        return;
    case kCrawlerRun:
        writer["errors"]["total"].ValueWithLabels(value, {{"op", "run"}, {"subsystem", "crawler"}});
        return;
    case kDenylistCheck:
        writer["errors"]["total"].ValueWithLabels(
            value, {{"op", "check"}, {"subsystem", "denylist"}}
        );
        return;
    case kCount:
        break;
    }
}

} // namespace

Metrics::Metrics(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : us::components::ComponentBase(config, context), capture_(), errors_()
{
    us::utils::statistics::RegisterWriterScope(
        context, "", [this](us::utils::statistics::Writer &writer) {
            writer["capture"]["jobs_created_total"] = capture_.jobs_created.Load();

            writer["capture"]["captures_total"].ValueWithLabels(
                capture_.succeeded.Load(), {{"result", "succeeded"}}
            );
            writer["capture"]["captures_total"].ValueWithLabels(
                capture_.failed.Load(), {{"result", "failed"}}
            );
            writer["capture"]["duration_ms_sum"].ValueWithLabels(
                capture_.succeeded_duration_ms_sum.Load(), {{"result", "succeeded"}}
            );
            writer["capture"]["duration_ms_sum"].ValueWithLabels(
                capture_.failed_duration_ms_sum.Load(), {{"result", "failed"}}
            );
            for (size_t i = 0; i < errors_.size(); i++) {
                WriteErrorMetric(writer, NumericCast<Metrics::Error>(i), errors_[i].Load());
            }
        }
    );
}

Metrics::~Metrics() = default;

void Metrics::AccountError(Error which) noexcept { errors_[NumericCast<size_t>(which)]++; }

void Metrics::AccountCaptureJobCreated() noexcept { capture_.jobs_created++; }

void Metrics::AccountCaptureCompleted(bool succeeded, std::chrono::milliseconds duration) noexcept
{
    const auto rate = us::utils::statistics::Rate{NumericCast<uint64_t>(duration.count())};
    if (succeeded) {
        capture_.succeeded++;
        capture_.succeeded_duration_ms_sum += rate;
    } else {
        capture_.failed++;
        capture_.failed_duration_ms_sum += rate;
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

} // namespace ws
