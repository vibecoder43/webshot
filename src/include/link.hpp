#pragma once

#include <string>

#include <ada.h>
#include <ada/url_aggregator.h>

namespace v1 {
struct InvalidLinkException : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct [[nodiscard]] Link {
    ada::url_aggregator url;
    std::string scheme_less;

    [[nodiscard]] static Link fromUserInput(std::string in, size_t queryPartLengthMax);

    [[nodiscard]] std::string host() const { return std::string(url.get_hostname()); }
    [[nodiscard]] std::string httpUrl() const { return std::string("http://") + scheme_less; }
    [[nodiscard]] const std::string &normalized() const { return scheme_less; }
};

} // namespace v1
