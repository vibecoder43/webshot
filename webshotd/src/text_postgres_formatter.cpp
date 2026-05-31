#include "text_postgres_formatter.hpp"

#include <userver/storages/postgres/exceptions.hpp>

namespace userver::storages::postgres::io {

BufferFormatter<String>::BufferFormatter(const String &value) : value(value) {}

void BufferFormatter<String>::operator()(const UserTypes &, std::vector<char> &buf) const
{
    auto sv = value.View();
    using CharFormatter = BufferFormatter<const char *>;
    CharFormatter::WriteN(buf, sv.data(), sv.size());
}

void BufferFormatter<String>::operator()(const UserTypes &, std::string &buf) const
{
    auto sv = value.View();
    using CharFormatter = BufferFormatter<const char *>;
    CharFormatter::WriteN(buf, sv.data(), sv.size());
}

BufferParser<String>::BufferParser(String &value) : value(value) {}

void BufferParser<String>::operator()(const FieldBuffer &buffer)
{
    auto tmp = buffer.ToString();
    auto parsed = String::FromBytes(tmp);
    if (!parsed)
        throw ::userver::storages::postgres::InvalidInputFormat(
            "invalid UTF-8 in PostgreSQL text field"
        );
    value = *parsed;
}

} // namespace userver::storages::postgres::io
