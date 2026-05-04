#include "config.hpp"
#include "invariant.hpp"
/**
 * @file
 * @brief Component that provides typed read-only configuration.
 */

#include <string>
#include <string_view>

#include <userver/components/component.hpp>
#include <userver/utils/assert.hpp>
#include <userver/yaml_config/merge_schemas.hpp>

namespace ws {
namespace us = userver;
using namespace text::literals;
using namespace std::chrono_literals;

namespace {

[[nodiscard]] String
ConfigText(const us::components::ComponentConfig &config, std::string_view field_name)
{
    return *String::FromBytes(config[std::string{field_name}].As<std::string>());
}

} // namespace

Config::Config(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : us::components::ComponentBase(config, context),
      url_bytes_max_(usize{config["url_bytes_max"].As<size_t>()}),
      allowlist_only_(config["allowlist_only"].As<bool>()),
      https_only_(config["https_only"].As<bool>()),
      state_dir_(config["state_dir"].As<std::string>()), client_ip_source_([&config]() {
          const auto source = config["client_ip_source"].As<std::string>();
          if (source == "peer")
              return ClientIpSource::kPeer;
          if (source == "trusted_header")
              return ClientIpSource::kTrustedHeader;
          Invariant("client_ip_source must be peer or trusted_header"_t);
      }()),
      client_ip_header_name_(config["client_ip_header_name"].As<std::string>()),
      s3_mode_([&config]() {
          const auto mode = config["s3_mode"].As<std::string>();
          if (mode == "local")
              return Mode::kLocal;
          if (mode == "external")
              return Mode::kExternal;
          Invariant("s3_mode must be local or external"_t);
      }()),
      s3_bucket_name_(ConfigText(config, "s3_bucket")),
      s3_endpoint_url_(ConfigText(config, "s3_endpoint")),
      s3_region_name_(ConfigText(config, "s3_region")),
      public_base_url_(ConfigText(config, "public_base_url")),
      s3_timeout_duration_(config["s3_timeout_ms"].As<int>() * 1ms)
{
    Invariant(!state_dir_.empty(), "state_dir must not be empty"_t);
    Invariant(
        client_ip_source_ != ClientIpSource::kTrustedHeader || !client_ip_header_name_.empty(),
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
    description:  dependency mode from config vars
  s3_endpoint:
    type: string
    description:  HTTP endpoint (e.g., http://127.0.0.1:8333)
  s3_region:
    type: string
    description: Optional region label
  public_base_url:
    type: string
    description: Public HTTP base for stored objects (bucket path-style)
  s3_timeout_ms:
    type: integer
    minimum: 1
    description: HTTP timeout to  in milliseconds
)");
}
} // namespace ws
