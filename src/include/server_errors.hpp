#pragma once
#include <stdexcept>

namespace v1::errors {

/**
 * @brief Pagination token could not be decoded or mismatched the request.
 */
struct InvalidPageTokenException : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

} // namespace v1::errors
