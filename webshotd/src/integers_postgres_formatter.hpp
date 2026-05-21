#pragma once

/**
 * @file
 * @brief PostgreSQL I/O customization for integers.hpp
 */

#include "integers.hpp"

#include <userver/storages/postgres/io/integral_types.hpp>

namespace userver::storages::postgres::io {

template <> struct BufferFormatter<i64> {
    i64 value;

    explicit BufferFormatter(const i64 value);

    template <typename Buffer> void operator()(const UserTypes &types, Buffer &buf) const
    {
        BufferFormatter<Bigint>{Raw(value)}(types, buf);
    }
};

template <> struct BufferParser<i64> {
    i64 &value;

    explicit BufferParser(i64 &value);

    void operator()(const FieldBuffer &buffer);
};

template <> struct CppToSystemPg<i64> : PredefinedOid<PredefinedOids::kInt8> {};

} // namespace userver::storages::postgres::io
