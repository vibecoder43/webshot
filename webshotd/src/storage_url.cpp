#include "storage_url.hpp"

/**
 * @file
 * @brief Helpers for externally visible direct  object URLs.
 */

#include "text.hpp"
#include "try.hpp"

#include <format>
#include <string>

#include <userver/utils/boost_uuid4.hpp>

using namespace text::literals;

namespace ws {
namespace {

using enum StorageUrlError;

[[nodiscard]] String AppendCaptureFilename(const Url &base_url, ws::uuid::Uuid uuid)
{
    std::string path{base_url.Pathname().View()};
    if (path.empty())
        path = "/";
    while (path.size() > 1 && path.back() == '/')
        path.pop_back();
    if (path.back() != '/')
        path.push_back('/');
    return text::Format("{}{}.wacz", path, us::utils::ToString(uuid));
}

[[nodiscard]] Expected<String, StorageUrlError>
ParseRequestHostname(const std::optional<String> &request_host)
{
    const auto request_host_value = TRY_OK_OR(request_host, kMissingRequestHost);
    ENSURE(!request_host_value.Empty(), kMissingRequestHost);

    const auto parsed = TRY_OK_OR(
        Url::FromText(text::Format("http://{}", request_host_value)), kInvalidRequestHost
    );
    ENSURE(parsed.HasHostname(), kInvalidRequestHost);
    ENSURE(parsed.Pathname() == "/"_t, kInvalidRequestHost);
    ENSURE(!parsed.HasSearch(), kInvalidRequestHost);

    return parsed.Hostname();
}

[[nodiscard]] Expected<Url, StorageUrlError>
BuildConfiguredCaptureDownloadUrl(ws::uuid::Uuid uuid, const String &public_base_url)
{
    const auto download_url_text = text::Format(
        "{}/{}.wacz", public_base_url, us::utils::ToString(uuid)
    );
    return TRY_OK_OR(Url::FromText(download_url_text), kInvalidPublicBaseUrl);
}

} // namespace

Expected<Url, StorageUrlError> BuildCaptureDownloadUrl(
    ws::uuid::Uuid uuid, Mode s3_mode, const String &public_base_url,
    const std::optional<String> &request_host
)
{
    using enum Mode;

    if (s3_mode == kExternal)
        return BuildConfiguredCaptureDownloadUrl(uuid, public_base_url);

    const auto base_url = TRY_OK_OR(Url::FromText(public_base_url), kInvalidPublicBaseUrl);
    ENSURE(base_url.IsHttpOrHttps(), kInvalidPublicBaseUrl);

    const auto hostname = TRY(ParseRequestHostname(request_host));

    auto download_url = base_url.WithHostname(hostname)
                            .WithPathname(AppendCaptureFilename(base_url, uuid))
                            .Stripped(Url::StripOptions::kQuery | Url::StripOptions::kHash);
    return download_url;
}

String StorageUrlErrorMessage(StorageUrlError error)
{
    using enum StorageUrlError;

    switch (error) {
    case kInvalidPublicBaseUrl:
        return "invalid public_base_url"_t;
    case kMissingRequestHost:
        return "missing request Host header"_t;
    case kInvalidRequestHost:
        return "invalid request Host header"_t;
    }
}

} // namespace ws
