#include "config.hpp"
#include "invariant.hpp"
/**
 * @file
 * @brief Component that provides typed read-only configuration.
 */

#include <chrono>
#include <string>
#include <string_view>

#include <userver/components/component.hpp>
#include <userver/utils/assert.hpp>
#include <userver/yaml_config/merge_schemas.hpp>

namespace v1 {
using namespace text::literals;
using namespace std::chrono_literals;

namespace {

[[nodiscard]] String
configText(const us::components::ComponentConfig &config, std::string_view fieldName)
{
    return String::fromBytes(config[std::string{fieldName}].As<std::string>()).expect();
}

} // namespace

Config::Config(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : us::components::ComponentBase(config, context),
      urlBytesMaxValue(usize{config["url_bytes_max"].As<size_t>()}),
      allowlistOnlyValue(config["allowlist_only"].As<bool>()),
      httpsOnlyValue(config["https_only"].As<bool>()),
      stateDirValue(config["state_dir"].As<std::string>()), clientIpSourceValue([&config]() {
          const auto source = config["client_ip_source"].As<std::string>();
          if (source == "peer")
              return ClientIpSource::kPeer;
          if (source == "trusted_header")
              return ClientIpSource::kTrustedHeader;
          invariant("client_ip_source must be peer or trusted_header"_t);
      }()),
      clientIpHeaderNameValue(config["client_ip_header_name"].As<std::string>()),
      s3ModeValue([&config]() {
          const auto mode = config["s3_mode"].As<std::string>();
          if (mode == "local")
              return S3Mode::kLocal;
          if (mode == "external")
              return S3Mode::kExternal;
          invariant("s3_mode must be local or external"_t);
      }()),
      s3BucketName(configText(config, "s3_bucket")),
      s3EndpointUrl(configText(config, "s3_endpoint")),
      s3RegionName(configText(config, "s3_region")),
      publicBaseUrlValue(configText(config, "public_base_url")),
      s3TimeoutDuration(config["s3_timeout_ms"].As<int>() * 1ms)
{
    invariant(!stateDirValue.empty(), "state_dir must not be empty"_t);
    invariant(
        clientIpSourceValue != ClientIpSource::kTrustedHeader || !clientIpHeaderNameValue.empty(),
        "client_ip_header_name must be set when client_ip_source is trusted_header"_t
    );
}

us::yaml_config::Schema Config::GetStaticConfigSchema()
{
    return us::yaml_config::MergeSchemas<us::components::ComponentBase>(R"(
type: object
description: Service static configuration
additionalProperties: false
properties:
  url_bytes_max:
    type: integer
    minimum: 1
    description: Maximum allowed URL length in bytes
    defaultDescription: "32768"
  allowlist_only:
    type: boolean
    description: Whether capture admission and browser fetches require allowlist membership
  https_only:
    type: boolean
    description: Whether crawler egress is restricted to HTTPS and WSS URLs
  state_dir:
    type: string
    description: Runner-owned state directory for this webshotd instance
  client_ip_source:
    type: string
    enum: [peer, trusted_header]
    description: Source used for per-IP cooldown identity
  client_ip_header_name:
    type: string
    description: Trusted header containing a single client IP literal when client_ip_source is trusted_header
  s3_bucket:
    type: string
    description: Target bucket name
  s3_mode:
    type: string
    enum: [local, external]
    description: S3 dependency mode from config vars
  s3_endpoint:
    type: string
    description: S3 HTTP endpoint (e.g., http://localhost:8333)
  s3_region:
    type: string
    description: Optional region label
  public_base_url:
    type: string
    description: Public HTTP base for stored objects (bucket path-style)
  s3_timeout_ms:
    type: integer
    minimum: 1
    description: HTTP timeout to S3 in milliseconds
)");
}
} // namespace v1
