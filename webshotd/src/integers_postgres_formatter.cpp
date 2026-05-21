#include "integers_postgres_formatter.hpp"

#include <userver/storages/postgres/exceptions.hpp>

namespace userver::storages::postgres::io {

BufferFormatter<i64>::BufferFormatter(i64 value) : value(value) {}

BufferParser<i64>::BufferParser(i64 &value) : value(value) {}

void BufferParser<i64>::operator()(const FieldBuffer &buffer)
{
    Bigint raw;
    BufferParser<Bigint>{raw}(buffer);
    value = i64{raw};
}

} // namespace userver::storages::postgres::io
