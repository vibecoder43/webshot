#include "s3/s3_url_utils.hpp"

#include <stdexcept>
#include <string>

#include <ada/unicode.h>

namespace v1::s3v4 {

std::vector<std::pair<String, String>> decodeQueryString(String search)
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
        const auto keyText = String::fromBytes(key);
        if (!keyText)
            throw std::runtime_error("invalid UTF-8 in S3 query key");
        const auto valueText = String::fromBytes(value);
        if (!valueText)
            throw std::runtime_error("invalid UTF-8 in S3 query value");
        query.emplace_back(keyText.value(), valueText.value());
        if (amp == std::string::npos)
            break;
        pos = amp + 1;
    }
    return query;
}

} // namespace v1::s3v4
