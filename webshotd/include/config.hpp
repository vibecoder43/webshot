#pragma once

#include "integers.hpp"
#include "text.hpp"

#include <chrono>
#include <string>
#include <string_view>

#include <userver/components/component_base.hpp>
#include <userver/yaml_config/schema.hpp>

namespace ws {
namespace us = userver;
enum class ClientIpSource {
    kPeer,
    kTrustedHeader,
};

enum class Mode {
    kLocal,
    kExternal,
};

/**
 * @brief Read-only configuration facade for the service.
 *
 * Exposes knobs used across handlers and the crawler pipeline.
 */
class [[nodiscard]] Config final : public us::components::ComponentBase {
public:
    static constexpr std::string_view kName = "config";

    Config(
        const us::components::ComponentConfig &config,
        const us::components::ComponentContext &context
    );

    [[nodiscard]] static us::yaml_config::Schema GetStaticConfigSchema();

    /** @return Maximum allowed URL length in bytes. */
    [[nodiscard]] usize UrlBytesMax() const noexcept { return url_bytes_max_; }

    /** @return Whether captures must match the allowlist. */
    [[nodiscard]] bool AllowlistOnly() const noexcept { return allowlist_only_; }

    /** @return Whether crawler egress is restricted to HTTPS/WSS. */
    [[nodiscard]] bool HttpsOnly() const noexcept { return https_only_; }

    /** @return Runner-owned state directory for webshotd instance. */
    [[nodiscard]] std::string_view StateDir() const noexcept { return state_dir_; }

    /** @return Source used for client IP cooldown identity. */
    [[nodiscard]] ClientIpSource ClientIpSource() const noexcept { return client_ip_source_; }

    /** @return Trusted header name for client IP cooldown identity. */
    [[nodiscard]] std::string_view ClientIpHeaderName() const noexcept
    {
        return client_ip_header_name_;
    }

    /** @name S3 parameters */
    ///@{
    [[nodiscard]] Mode S3Mode() const noexcept { return s3_mode_; }
    [[nodiscard]] const String &S3Bucket() const noexcept { return s3_bucket_name_; }
    [[nodiscard]] const String &S3Endpoint() const noexcept { return s3_endpoint_url_; }
    [[nodiscard]] const String &S3Region() const noexcept { return s3_region_name_; }
    [[nodiscard]] const String &S3PublicBaseUrl() const noexcept { return public_base_url_; }
    [[nodiscard]] std::chrono::milliseconds S3Timeout() const noexcept
    {
        return s3_timeout_duration_;
    }
    ///@}

private:
    usize url_bytes_max_;
    bool allowlist_only_;
    bool https_only_;
    std::string state_dir_;
    enum ClientIpSource client_ip_source_;
    std::string client_ip_header_name_;
    enum Mode s3_mode_;
    String s3_bucket_name_;
    String s3_endpoint_url_;
    String s3_region_name_;
    String public_base_url_;
    std::chrono::milliseconds s3_timeout_duration_;
};
} // namespace ws
