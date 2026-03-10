#include "docs_static_handler.hpp"

#include <algorithm>
#include <exception>
#include <string>
#include <string_view>

#include <userver/fs/blocking/read.hpp>
#include <userver/http/content_type.hpp>
#include <userver/server/http/http_status.hpp>
#include <userver/yaml_config/merge_schemas.hpp>

namespace {

[[nodiscard]] bool IsAllowedPathPart(std::string_view part)
{
    return !part.empty() && part != "." && part != ".." &&
           part.find('\0') == std::string_view::npos;
}

[[nodiscard]] std::string RequestPathWithinRoot(const userver::server::http::HttpRequest &request)
{
    std::string relativePath;
    for (size_t i = 0; i < request.PathArgCount(); ++i) {
        const auto arg = request.GetPathArg(i);
        size_t start = 0;
        while (start <= arg.size()) {
            const auto slash = arg.find('/', start);
            const auto part = arg.substr(
                start, slash == std::string::npos ? std::string::npos : slash - start
            );
            if (!IsAllowedPathPart(part))
                return {};
            if (!relativePath.empty())
                relativePath.push_back('/');
            relativePath.append(part);
            if (slash == std::string::npos)
                break;
            start = slash + 1;
        }
    }
    return relativePath;
}

[[nodiscard]] std::string ContentTypeForPath(std::string_view path)
{
    const auto dot = path.rfind('.');
    const auto ext = dot == std::string_view::npos ? std::string_view{} : path.substr(dot);
    if (ext == ".css")
        return "text/css";
    if (ext == ".js")
        return "application/javascript";
    if (ext == ".yaml" || ext == ".yml")
        return "application/yaml";
    if (ext == ".json")
        return "application/json";
    if (ext == ".html")
        return "text/html";
    return "text/plain";
}

[[nodiscard]] userver::yaml_config::Schema StaticSchema()
{
    return userver::yaml_config::MergeSchemas<userver::server::handlers::HttpHandlerBase>(R"(
type: object
description: Static docs file handler
additionalProperties: false
properties: {}
)");
}

std::string
HandleStaticRequest(const userver::server::http::HttpRequest &request, const std::string &rootDir)
{
    const auto relativePath = RequestPathWithinRoot(request);
    auto &response = request.GetHttpResponse();
    if (relativePath.empty()) {
        response.SetStatus(userver::server::http::HttpStatus::kNotFound);
        return "not found";
    }

    try {
        response.SetContentType(ContentTypeForPath(relativePath));
        return userver::fs::blocking::ReadFileContents(rootDir + "/" + relativePath);
    } catch (const std::exception &) {
        response.SetStatus(userver::server::http::HttpStatus::kNotFound);
        return "not found";
    }
}

} // namespace

namespace v1 {

ScalarAssetsHandler::ScalarAssetsHandler(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : HttpHandlerBase(config, context), rootDir(WEBSHOT_SCALAR_ASSETS_DIR)
{
}

us::yaml_config::Schema ScalarAssetsHandler::GetStaticConfigSchema() { return StaticSchema(); }

std::string ScalarAssetsHandler::HandleRequestThrow(
    const server::http::HttpRequest &request, server::request::RequestContext &
) const
{
    return HandleStaticRequest(request, rootDir);
}

OpenApiHandler::OpenApiHandler(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : HttpHandlerBase(config, context), rootDir(std::string(WEBSHOT_REPO_ROOT) + "/schema")
{
}

us::yaml_config::Schema OpenApiHandler::GetStaticConfigSchema() { return StaticSchema(); }

std::string OpenApiHandler::HandleRequestThrow(
    const server::http::HttpRequest &request, server::request::RequestContext &
) const
{
    return HandleStaticRequest(request, rootDir);
}

} // namespace v1
