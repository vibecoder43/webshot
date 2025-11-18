#pragma once

#include <chrono>
#include <cstddef>
#include <string_view>

#include <userver/components/component_base.hpp>
#include <userver/yaml_config/schema.hpp>

namespace us = userver;

namespace v1 {
/**
 * @brief Read‑only configuration facade for the service.
 *
 * Exposes knobs used across handlers and the crawler pipeline.
 */
class [[nodiscard]] WebshotConfig final : public us::components::ComponentBase {
public:
    static constexpr std::string_view kName = "webshot-config";

    WebshotConfig(
        const us::components::ComponentConfig &config,
        const us::components::ComponentContext &context
    );

    [[nodiscard]] static us::yaml_config::Schema GetStaticConfigSchema();

    /** @return Maximum allowed length of URL query part. */
    [[nodiscard]] size_t queryPartLengthMax() const noexcept { return queryPartLengthMax_; }

    /** @name S3 parameters */
    ///@{
    [[nodiscard]] const std::string &s3Bucket() const noexcept { return s3Bucket_; }
    [[nodiscard]] const std::string &s3Endpoint() const noexcept { return s3Endpoint_; }
    [[nodiscard]] const std::string &s3Region() const noexcept { return s3Region_; }
    [[nodiscard]] const std::string &publicBaseUrl() const noexcept { return publicBaseUrl_; }
    [[nodiscard]] std::chrono::milliseconds s3Timeout() const noexcept { return s3Timeout_; }
    ///@}

private:
    size_t queryPartLengthMax_;
    std::string s3Bucket_;
    std::string s3Endpoint_;
    std::string s3Region_;
    std::string publicBaseUrl_;
    std::chrono::milliseconds s3Timeout_{10000};
};
} // namespace v1
