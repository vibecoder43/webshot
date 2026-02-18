#include "webshot_config.hpp"
/**
 * @file
 * @brief Component that provides typed read-only configuration.
 */

#include <chrono>

#include <userver/components/component.hpp>
#include <userver/yaml_config/merge_schemas.hpp>

namespace v1 {
WebshotConfig::WebshotConfig(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : us::components::ComponentBase(config, context),
      queryPartLengthMaxValue(config["query-part-length-max"].As<size_t>()),
      s3BucketName(String::fromBytesThrow(config["s3-bucket"].As<std::string>())),
      s3EndpointUrl(String::fromBytesThrow(config["s3-endpoint"].As<std::string>())),
      s3RegionName(String::fromBytesThrow(config["s3-region"].As<std::string>())),
      publicBaseUrlValue(String::fromBytesThrow(config["public-base-url"].As<std::string>())),
      s3TimeoutDuration(std::chrono::milliseconds(config["s3-timeout-ms"].As<int>()))
{
}

us::yaml_config::Schema WebshotConfig::GetStaticConfigSchema()
{
    return us::yaml_config::MergeSchemas<us::components::ComponentBase>(R"(
type: object
description: Webshot service static configuration
additionalProperties: false
properties:
  query-part-length-max:
    type: integer
    minimum: 1
    description: Maximum allowed length of the query part in the URL
    defaultDescription: "1024"
  s3-bucket:
    type: string
    description: Target bucket name
  s3-endpoint:
    type: string
    description: S3 HTTP endpoint (e.g., http://localhost:8333)
  s3-region:
    type: string
    description: Optional region label
  public-base-url:
    type: string
    description: Public HTTP base for stored objects (bucket path-style)
  s3-timeout-ms:
    type: integer
    minimum: 1
    description: HTTP timeout to S3 in milliseconds
)");
}
} // namespace v1
