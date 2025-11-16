#pragma once

#include "link.hpp"
#include "schemas/webshot.hpp"
#include "webshot.hpp"

#include <string>
#include <string_view>
#include <vector>

#include <boost/uuid/uuid.hpp>

#include <userver/components/component_base.hpp>
#include <userver/yaml_config/schema.hpp>

namespace us = userver;
using Uuid = boost::uuids::uuid;

namespace v1 {
class [[nodiscard]] WebshotCrud : public us::components::ComponentBase {
public:
    static constexpr std::string_view kName = "webshot-crud";
    explicit WebshotCrud(
        const us::components::ComponentConfig &config,
        const us::components::ComponentContext &context
    );

    ~WebshotCrud();

    void createWebshot(Link link);
    [[nodiscard]] std::optional<Webshot> findWebshot(Uuid uuid);

    [[nodiscard]] std::vector<dto::UuidWithTime> findWebshotByLink(const Link &link);
    [[nodiscard]] dto::PagedFindWebshotByUrlResponse
    findWebshotByLinkPage(const Link &link, const std::optional<std::string> &pageToken);
    [[nodiscard]] dto::PagedFindWebshotByPrefixResponse findWebshotsByPrefixPage(
        const std::string &normalizedPrefix, const std::optional<std::string> &pageToken
    );
    [[nodiscard]] static us::yaml_config::Schema GetStaticConfigSchema();

private:
    class Impl;
    std::unique_ptr<Impl> impl;
};
}; // namespace v1
