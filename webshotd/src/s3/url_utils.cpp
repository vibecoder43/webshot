#include "s3/url_utils.hpp"

#include "character_type.hpp"
#include "try.hpp"

#include <stddef.h>
#include <string>

#include <ada/unicode.h>

namespace ws::s3 {

using namespace text::literals;

namespace {

[[nodiscard]] bool HasExplicitScheme(std::string_view text)
{
    auto scheme_pos = text.find("://");
    if (scheme_pos == std::string_view::npos || scheme_pos == 0)
        return false;
    if (!ctype::IsAsciiAlpha(text.front()))
        return false;
    for (const char c : text.substr(1, scheme_pos - 1)) {
        if (!(ctype::IsAsciiAlnum(c) || c == '+' || c == '-' || c == '.'))
            return false;
    }
    return true;
}

} // namespace

std::optional<Url> ParseUrlWithDefaultHttpScheme(const String &text)
{
    if (auto url = Url::FromText(text))
        return url;
    if (HasExplicitScheme(text.View()))
        return {};
    auto with_scheme = "http://"_t + text;
    return Url::FromText(with_scheme);
}

Expected<std::vector<std::pair<String, String>>, QueryStringError> DecodeQueryString(String search)
{
    std::vector<std::pair<String, String>> query;
    if (search.Empty())
        return query;

    std::string search_copy(search.View());
    if (!search_copy.empty() && search_copy.front() == '?')
        search_copy.erase(search_copy.begin());

    size_t pos = 0;
    while (pos < search_copy.size()) {
        auto amp = search_copy.find('&', pos);
        auto eq = search_copy.find('=', pos);
        if (eq == std::string::npos)
            break;
        auto key_part = search_copy.substr(pos, eq - pos);
        auto val_part = search_copy.substr(
            eq + 1, amp == std::string::npos ? std::string::npos : amp - eq - 1
        );
        auto key_percent = key_part.find('%');
        auto val_percent = val_part.find('%');
        std::string key = ada::unicode::percent_decode(
            key_part, key_percent == std::string::npos ? std::string::npos : key_percent
        );
        std::string value = ada::unicode::percent_decode(
            val_part, val_percent == std::string::npos ? std::string::npos : val_percent
        );
        auto key_text = TRY_ERR_AS(String::FromBytes(key), QueryStringError::kInvalidUtf8Key);
        auto value_text = TRY_ERR_AS(String::FromBytes(value), QueryStringError::kInvalidUtf8Value);
        query.emplace_back(key_text, value_text);
        if (amp == std::string::npos)
            break;
        pos = amp + 1;
    }
    return query;
}

} // namespace ws::s3
