#include "prefix_utils.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include <userver/utils/encoding/hex.hpp>

namespace v1::prefix {

namespace {

void appendEncodedSegment(std::string &out, std::string_view bytes)
{
    constexpr size_t kMaxBytesPerLabel = 127UL;
    if (bytes.empty()) {
        out.append(".x");
        return;
    }
    for (size_t pos = 0; pos < bytes.size(); pos += kMaxBytesPerLabel) {
        const auto chunk = bytes.substr(pos, std::min(kMaxBytesPerLabel, bytes.size() - pos));
        out.push_back('.');
        out.push_back('x');
        out.append(userver::utils::encoding::ToHex(chunk));
    }
}

} // namespace

[[nodiscard]] String makePrefixKey(const Link &link)
{
    auto host = link.url.hostname();
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

[[nodiscard]] std::string makePrefixTree(const String &prefixKey)
{
    auto view = prefixKey.view();
    std::string out("h");
    const auto firstSlash = view.find('/');
    const auto hostPart = firstSlash == std::string_view::npos
                              ? std::string_view(view)
                              : std::string_view(view).substr(0, firstSlash);

    const auto appendSplitSegments = [&out](std::string_view input, const char sep) {
        for (size_t start = 0;;) {
            const auto next = input.find(sep, start);
            const auto seg = next == std::string_view::npos ? input.substr(start)
                                                            : input.substr(start, next - start);
            appendEncodedSegment(out, seg);
            if (next == std::string_view::npos)
                break;
            start = next + 1;
        }
    };

    appendSplitSegments(hostPart, '.');

    if (firstSlash == std::string_view::npos)
        return out;

    out.append(".p");
    const auto path = std::string_view(view).substr(firstSlash + 1);
    appendSplitSegments(path, '/');
    return out;
}

} // namespace v1::prefix
