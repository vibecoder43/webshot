#pragma once

#include <cstddef>
#include <string_view>

#include <userver/components/component_base.hpp>
#include <userver/yaml_config/schema.hpp>

namespace us = userver;

namespace v1 {
class [[nodiscard]] WebshotConfig final : public us::components::ComponentBase {
public:
    static constexpr std::string_view kName = "webshot-config";

    WebshotConfig(
        const us::components::ComponentConfig &config,
        const us::components::ComponentContext &context
    );

    [[nodiscard]] static us::yaml_config::Schema GetStaticConfigSchema();

    [[nodiscard]] size_t queryPartLengthMax() const noexcept { return queryPartLengthMax_; }

private:
    size_t queryPartLengthMax_;
};
} // namespace v1
