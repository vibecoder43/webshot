#include "config.hpp"
/**
 * @file
 * @brief Component that provides typed read-only configuration.
 */

#include <chrono>

#include <userver/components/component.hpp>
#include <userver/utils/assert.hpp>
#include <userver/yaml_config/merge_schemas.hpp>

namespace v1 {
Config::Config(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : us::components::ComponentBase(config, context),
      urlBytesMaxValue(usize{config["url_bytes_max"].As<size_t>()}),
      stateDirValue(config["state_dir"].As<std::string>()),
      s3BucketName(String::fromBytes(config["s3_bucket"].As<std::string>()).expect()),
      s3EndpointUrl(String::fromBytes(config["s3_endpoint"].As<std::string>()).expect()),
      s3RegionName(String::fromBytes(config["s3_region"].As<std::string>()).expect()),
      publicBaseUrlValue(String::fromBytes(config["public_base_url"].As<std::string>()).expect()),
      s3TimeoutDuration(std::chrono::milliseconds(config["s3_timeout_ms"].As<int>()))
{
    UINVARIANT(!stateDirValue.empty(), "state_dir must not be empty");
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
  state_dir:
    type: string
    description: Runner-owned state directory for this webshotd instance
  s3_bucket:
    type: string
    description: Target bucket name
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
