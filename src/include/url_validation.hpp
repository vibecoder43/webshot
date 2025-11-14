#pragma once

#include <stdexcept>
#include <string>

std::string tryNormalizeLink(std::string in, size_t queryPartLengthMax);

struct InvalidLinkException : public std::runtime_error {
    using std::runtime_error::runtime_error;
};
