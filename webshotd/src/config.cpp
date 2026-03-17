/**
 * @file
 * @brief Component that provides typed read-only configuration.
 */

#include "config.hpp"
#include <chrono>
#include <string>
#include <userver/yaml_config/merge_schemas.hpp>
#include <userver/yaml_config/yaml_config.hpp>

namespace v1 {
Config::Config(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : us::components::ComponentBase(config, context),
      queryPartLengthMaxValue(config["query_part_length_max"].As<size_t>()),
      s3BucketName(String::fromBytesThrow(config["s3_bucket"].As<std::string>())),
      s3EndpointUrl(String::fromBytesThrow(config["s3_endpoint"].As<std::string>())),
      s3RegionName(String::fromBytesThrow(config["s3_region"].As<std::string>())),
      publicBaseUrlValue(String::fromBytesThrow(config["public_base_url"].As<std::string>())),
      s3TimeoutDuration(std::chrono::milliseconds(config["s3_timeout_ms"].As<int>()))
{
}

us::yaml_config::Schema Config::GetStaticConfigSchema()
{
    return us::yaml_config::MergeSchemas<us::components::ComponentBase>(R"(
type: object
description: Service static configuration
additionalProperties: false
properties:
  query_part_length_max:
    type: integer
    minimum: 1
    description: Maximum allowed length of the query part in the URL
    defaultDescription: "1024"
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
