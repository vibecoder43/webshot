#pragma once

#include "text.hpp"
#include <format>
#include <string>
#include <userver/storages/postgres/io/io_fwd.hpp>
#include <userver/storages/postgres/io/pg_types.hpp>
#include <userver/storages/postgres/io/traits.hpp>

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

    explicit BufferFormatter(const String &val);

    void operator()(const UserTypes &userTypes, std::vector<char> &buf) const;
    void operator()(const UserTypes &userTypes, std::string &buf) const;
};

template <> struct BufferParser<String> {
    String &value;

    explicit BufferParser(String &val);

    void operator()(const FieldBuffer &buffer);
};

template <> struct CppToSystemPg<String> : PredefinedOid<PredefinedOids::kText> {};

} // namespace userver::storages::postgres::io
