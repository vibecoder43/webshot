#include "storage_url.hpp"

/**
 * @file
 * @brief Helpers for externally visible direct  object URLs.
 */

#include "text.hpp"
#include "try.hpp"

#include <format>
#include <string>
#include <string_view>

#include <userver/utils/boost_uuid4.hpp>
#include <userver/utils/text_light.hpp>

using namespace text::literals;

namespace ws {
namespace {

namespace us = userver;
namespace utext = us::utils::text;
using enum StorageUrlError;

struct [[nodiscard]] ParsedHostHeader final {
    String hostname;
    std::optional<String> port; // absent means "no explicit port in header"
};

[[nodiscard]] bool ICaseEqual(std::string_view lhs, std::string_view rhs) noexcept
{
    return lhs.size() == rhs.size() && utext::ICaseStartsWith(lhs, rhs);
}

[[nodiscard]] std::string_view TakeFirstCommaSeparatedValue(std::string_view text) noexcept
{
    const auto parts = utext::SplitIntoStringViewVector(text, ",");
    if (parts.empty())
        return {};
    return utext::TrimView(parts.front());
}

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

[[nodiscard]] Expected<ParsedHostHeader, StorageUrlError> ParseHostHeader(
    const std::optional<String> &header, StorageUrlError missing_error,
    StorageUrlError invalid_error
)
{
    const auto header_value = TRY_OK_OR(header, missing_error);
    ENSURE(!header_value.Empty(), missing_error);

    const std::string_view first = TakeFirstCommaSeparatedValue(header_value.View());
    ENSURE(!first.empty(), missing_error);
    const auto parsed = TRY_OK_OR(Url::FromText(text::Format("http://{}", first)), invalid_error);
    ENSURE(parsed.HasHostname(), invalid_error);
    ENSURE(parsed.Pathname() == "/"_t, invalid_error);
    ENSURE(!parsed.HasSearch(), invalid_error);

    ParsedHostHeader out{
        .hostname = parsed.Hostname(),
        .port = {},
    };
    if (parsed.HasPort())
        out.port = parsed.Port();
    return out;
}

[[nodiscard]] Expected<Url, StorageUrlError>
MakeConfiguredCaptureDownloadUrl(ws::uuid::Uuid uuid, const String &public_base_url)
{
    const auto download_url_text = text::Format(
        "{}/{}.wacz", public_base_url, us::utils::ToString(uuid)
    );
    return TRY_OK_OR(Url::FromText(download_url_text), kInvalidPublicBaseUrl);
}

} // namespace

Expected<Url, StorageUrlError> MakeCaptureDownloadUrl(
    ws::uuid::Uuid uuid, Mode s3_mode, const String &public_base_url,
    const std::optional<String> &request_host, const std::optional<String> &forwarded_host,
    const std::optional<String> &forwarded_proto, bool https_only
)
{
    using enum Mode;

    if (s3_mode == kExternal)
        return MakeConfiguredCaptureDownloadUrl(uuid, public_base_url);

    const auto base_url = TRY_OK_OR(Url::FromText(public_base_url), kInvalidPublicBaseUrl);
    ENSURE(base_url.IsHttpOrHttps(), kInvalidPublicBaseUrl);

    auto download_url = base_url.WithPathname(AppendCaptureFilename(base_url, uuid))
                            .Stripped(Url::StripOptions::kQuery | Url::StripOptions::kHash);

    const auto header_host =
        forwarded_host
            ? TRY(ParseHostHeader(forwarded_host, kInvalidForwardedHost, kInvalidForwardedHost))
            : TRY(ParseHostHeader(request_host, kMissingRequestHost, kInvalidRequestHost));
    download_url = download_url.WithHostname(header_host.hostname);

    // If proxy provides X-Forwarded-Host without a port, do not leak internal port from config.
    if (forwarded_host) {
        if (header_host.port) {
            download_url = download_url.WithPort(*header_host.port);
        } else {
            download_url = download_url.Stripped(Url::StripOptions::kPort);
        }
    }

    if (forwarded_proto) {
        const std::string_view raw = forwarded_proto->View();
        const auto first = TakeFirstCommaSeparatedValue(raw);
        ENSURE(!first.empty(), kInvalidForwardedProto);
        ENSURE(ICaseEqual(first, "http") || ICaseEqual(first, "https"), kInvalidForwardedProto);
        download_url = download_url.WithProtocol(ICaseEqual(first, "https") ? "https"_t : "http"_t);
    } else if (forwarded_host && https_only) {
        // When deployed behind TLS-terminating proxy, https_only indicates public scheme should be
        // https.
        download_url = download_url.WithProtocol("https"_t);
    }

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
    case kInvalidForwardedHost:
        return "invalid X-Forwarded-Host header"_t;
    case kInvalidForwardedProto:
        return "invalid X-Forwarded-Proto header"_t;
    }
}

} // namespace ws
