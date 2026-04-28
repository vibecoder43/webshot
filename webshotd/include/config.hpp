#pragma once

#include "integers.hpp"
#include "text.hpp"
#include "userver_namespaces.hpp"

#include <chrono>
#include <string>
#include <string_view>

#include <userver/components/component_base.hpp>
#include <userver/yaml_config/schema.hpp>

namespace v1 {
enum class ClientIpSource {
    kPeer,
    kTrustedHeader,
};

enum class S3Mode {
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
    [[nodiscard]] usize urlBytesMax() const noexcept { return urlBytesMaxValue; }

    /** @return Whether captures must match the allowlist. */
    [[nodiscard]] bool allowlistOnly() const noexcept { return allowlistOnlyValue; }

    /** @return Whether crawler egress is restricted to HTTPS/WSS. */
    [[nodiscard]] bool httpsOnly() const noexcept { return httpsOnlyValue; }

    /** @return Runner-owned state directory for webshotd instance. */
    [[nodiscard]] std::string_view stateDir() const noexcept { return stateDirValue; }

    /** @return Source used for client IP cooldown identity. */
    [[nodiscard]] ClientIpSource clientIpSource() const noexcept { return clientIpSourceValue; }

    /** @return Trusted header name for client IP cooldown identity. */
    [[nodiscard]] std::string_view clientIpHeaderName() const noexcept
    {
        return clientIpHeaderNameValue;
    }

    /** @name S3 parameters */
    ///@{
    [[nodiscard]] S3Mode s3Mode() const noexcept { return s3ModeValue; }
    [[nodiscard]] const String &s3Bucket() const noexcept { return s3BucketName; }
    [[nodiscard]] const String &s3Endpoint() const noexcept { return s3EndpointUrl; }
    [[nodiscard]] const String &s3Region() const noexcept { return s3RegionName; }
    [[nodiscard]] const String &publicBaseUrl() const noexcept { return publicBaseUrlValue; }
    [[nodiscard]] std::chrono::milliseconds s3Timeout() const noexcept { return s3TimeoutDuration; }
    ///@}

private:
    usize urlBytesMaxValue;
    bool allowlistOnlyValue;
    bool httpsOnlyValue;
    std::string stateDirValue;
    ClientIpSource clientIpSourceValue;
    std::string clientIpHeaderNameValue;
    S3Mode s3ModeValue;
    String s3BucketName;
    String s3EndpointUrl;
    String s3RegionName;
    String publicBaseUrlValue;
    std::chrono::milliseconds s3TimeoutDuration;
};
} // namespace v1
