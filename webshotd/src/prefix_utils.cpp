#include "prefix_utils.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include <userver/utils/encoding/hex.hpp>

namespace ws::prefix {

namespace us = userver;

namespace {

void AppendEncodedSegment(std::string &out, std::string_view bytes)
{
    constexpr size_t max_bytes_per_label = 127UL;
    if (bytes.empty()) {
        out.append(".x");
        return;
    }
    for (size_t pos = 0; pos < bytes.size(); pos += max_bytes_per_label) {
        auto chunk = bytes.substr(pos, std::min(max_bytes_per_label, bytes.size() - pos));
        out.push_back('.');
        out.push_back('x');
        out.append(us::utils::encoding::ToHex(chunk));
    }
}

} // namespace

[[nodiscard]] String MakePrefixKey(const Link &link)
{
    auto host = link.Hostname();
    auto host_view = host.View();
    std::string host_str{host_view};
    std::vector<std::string> labels;
    for (size_t start = 0; true;) {
        auto dot = host_str.find('.', start);
        if (dot == std::string::npos) {
            labels.emplace_back(host_str.substr(start));
            break;
        }
        labels.emplace_back(host_str.substr(start, dot - start));
        start = dot + 1;
    }
    std::ranges::reverse(labels);
    std::string host_rev;
    for (size_t i = 0; i < labels.size(); i++) {
        if (i != 0)
            host_rev.push_back('.');
        host_rev += labels[i];
    }
    auto path = link.Pathname().ToBytes();
    if (path == "/")
        path.clear();
    else if (!path.empty() && path.back() == '/')
        path.pop_back();
    auto key = host_rev;
    key.append(std::begin(path), std::end(path));
    return *String::FromBytes(key);
}

[[nodiscard]] std::string MakePrefixTree(const String &prefix_key)
{
    auto view = prefix_key.View();
    std::string out("h");
    auto first_slash = view.find('/');
    auto host_part = first_slash == std::string_view::npos
                         ? std::string_view(view)
                         : std::string_view(view).substr(0, first_slash);

    auto append_split_segments = [&out](std::string_view input, const char sep) {
        for (size_t start = 0;;) {
            auto next = input.find(sep, start);
            auto seg = next == std::string_view::npos ? input.substr(start)
                                                      : input.substr(start, next - start);
            AppendEncodedSegment(out, seg);
            if (next == std::string_view::npos)
                break;
            start = next + 1;
        }
    };

    append_split_segments(host_part, '.');

    if (first_slash == std::string_view::npos)
        return out;

    out.append(".p");
    auto path = std::string_view(view).substr(first_slash + 1);
    append_split_segments(path, '/');
    return out;
}

} // namespace ws::prefix
