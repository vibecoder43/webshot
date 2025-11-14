#include "include/webshot_config.hpp"

#include <userver/components/component.hpp>
#include <userver/yaml_config/merge_schemas.hpp>

namespace v1 {
WebshotConfig::WebshotConfig(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : us::components::ComponentBase(config, context),
      queryPartLengthMax_(config["query-part-length-max"].As<size_t>(1024))
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
)");
}
} // namespace v1
