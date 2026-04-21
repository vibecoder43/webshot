#include "s3/s3_url_utils.hpp"

#include "try.hpp"

#include <cctype>
#include <expected>
#include <stddef.h>
#include <string>

#include <ada/unicode.h>

namespace v1::s3v4 {

using namespace text::literals;

namespace {

[[nodiscard]] bool hasExplicitScheme(std::string_view text)
{
    const auto schemePos = text.find("://");
    if (schemePos == std::string_view::npos || schemePos == 0)
        return false;
    if (!std::isalpha(static_cast<unsigned char>(text.front())))
        return false;
    for (const char c : text.substr(1, schemePos - 1)) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '-' || c == '.'))
            return false;
    }
    return true;
}

} // namespace

std::optional<Url> parseUrlWithDefaultHttpScheme(const String &text)
{
    if (const auto url = Url::fromText(text))
        return url;
    if (hasExplicitScheme(text.view()))
        return {};
    const auto withScheme = "http://"_t + text;
    return Url::fromText(withScheme);
}

Expected<std::vector<std::pair<String, String>>, QueryStringError> decodeQueryString(String search)
{
    std::vector<std::pair<String, String>> query;
    if (search.empty())
        return query;

    std::string searchCopy(search.view());
    if (!searchCopy.empty() && searchCopy.front() == '?')
        searchCopy.erase(searchCopy.begin());

    size_t pos = 0;
    while (pos < searchCopy.size()) {
        auto amp = searchCopy.find('&', pos);
        auto eq = searchCopy.find('=', pos);
        if (eq == std::string::npos)
            break;
        auto keyPart = searchCopy.substr(pos, eq - pos);
        auto valPart = searchCopy.substr(
            eq + 1, amp == std::string::npos ? std::string::npos : amp - eq - 1
        );
        auto keyPercent = keyPart.find('%');
        auto valPercent = valPart.find('%');
        std::string key = ada::unicode::percent_decode(
            keyPart, keyPercent == std::string::npos ? std::string::npos : keyPercent
        );
        std::string value = ada::unicode::percent_decode(
            valPart, valPercent == std::string::npos ? std::string::npos : valPercent
        );
        const auto keyText = TRY_ERR_AS(String::fromBytes(key), QueryStringError::kInvalidUtf8Key);
        const auto valueText = TRY_ERR_AS(
            String::fromBytes(value), QueryStringError::kInvalidUtf8Value
        );
        query.emplace_back(keyText, valueText);
        if (amp == std::string::npos)
            break;
        pos = amp + 1;
    }
    return query;
}

} // namespace v1::s3v4
