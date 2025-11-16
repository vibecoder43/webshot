#pragma once
#include <memory>
#include <string>

#include <userver/components/component_base.hpp>

namespace v1 {

class [[nodiscard]] WebshotDenylist : public userver::components::ComponentBase {
public:
    static constexpr std::string_view kName = "webshot-denylist";

    explicit WebshotDenylist(
        const userver::components::ComponentConfig &config,
        const userver::components::ComponentContext &context
    );

    ~WebshotDenylist();

    [[nodiscard]] bool isAllowedHost(const std::string &hostLowerPunycode) noexcept;

    void purgeHostAndSubdomains(const std::string &hostLowerPunycode);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace v1
