#pragma once

/**
 * @file
 * @brief PostgreSQL I/O customization for String.
 *
 * Provides userver Postgres formatters/parsers and type mapping so that
 * String can be used directly as a parameter and result type
 * (mapped to PostgreSQL TEXT).
 */

#include "text.hpp"

#include <string>
#include <vector>

#include <userver/storages/postgres/io/string_types.hpp>

namespace userver::storages::postgres::io {

template <> struct BufferFormatter<String> {
    const String &value;

    explicit BufferFormatter(const String &value);

    void operator()(const UserTypes &userTypes, std::vector<char> &buf) const;
    void operator()(const UserTypes &userTypes, std::string &buf) const;
};

template <> struct BufferParser<String> {
    String &value;

    explicit BufferParser(String &value);

    void operator()(const FieldBuffer &buffer);
};

template <> struct CppToSystemPg<String> : PredefinedOid<PredefinedOids::kText> {};

} // namespace userver::storages::postgres::io
