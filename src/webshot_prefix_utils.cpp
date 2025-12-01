#include "webshot_prefix_utils.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace v1::prefix {

[[nodiscard]] String makePrefixKey(const Link &link)
{
    auto host = link.host();
    auto hostView = host.view();
    std::string hostStr(hostView);
    std::vector<std::string> labels;
    for (size_t start = 0; true;) {
        auto dot = hostStr.find('.', start);
        if (dot == std::string::npos) {
            labels.emplace_back(hostStr.substr(start));
            break;
        }
        labels.emplace_back(hostStr.substr(start, dot - start));
        start = dot + 1;
    }
    std::reverse(std::begin(labels), std::end(labels));
    std::string hostRev;
    for (size_t i = 0; i < labels.size(); i++) {
        if (i != 0)
            hostRev.push_back('.');
        hostRev += labels[i];
    }
    auto normalized = link.normalized();
    auto normView = normalized.view();
    auto slashPos = normView.find('/');
    auto key = hostRev;
    if (slashPos != std::string::npos) {
        auto pathAndQuery = normView.substr(slashPos);
        auto qPos = pathAndQuery.find('?');
        auto path = qPos == std::string::npos ? pathAndQuery : pathAndQuery.substr(0, qPos);
        key.append(std::begin(path), std::end(path));
    }
    return String::fromBytesThrow(key);
}

[[nodiscard]] std::vector<std::string> expandPrefixCandidates(const String &prefixKey)
{
    std::vector<std::string> out;
    auto view = prefixKey.view();
    if (view.empty())
        return out;
    auto firstSlash = view.find('/');
    if (firstSlash == std::string::npos) {
        out.emplace_back(view);
        return out;
    }
    out.emplace_back(view.substr(0, firstSlash));
    for (size_t pos = firstSlash; pos < view.size();) {
        auto next = view.find('/', pos + 1);
        if (next == std::string::npos) {
            out.emplace_back(view);
            break;
        }
        out.emplace_back(view.substr(0, next));
        pos = next;
    }
    return out;
}

} // namespace v1::prefix
