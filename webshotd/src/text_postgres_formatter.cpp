#include "text_postgres_formatter.hpp"

#include <userver/storages/postgres/exceptions.hpp>

namespace userver::storages::postgres::io {

BufferFormatter<String>::BufferFormatter(const String &val) : value(val) {}

void BufferFormatter<String>::operator()(const UserTypes &, std::vector<char> &buf) const
{
    const auto sv = value.view();
    using CharFormatter = BufferFormatter<const char *>;
    CharFormatter::WriteN(buf, sv.data(), sv.size());
}

void BufferFormatter<String>::operator()(const UserTypes &, std::string &buf) const
{
    const auto sv = value.view();
    using CharFormatter = BufferFormatter<const char *>;
    CharFormatter::WriteN(buf, sv.data(), sv.size());
}

BufferParser<String>::BufferParser(String &val) : value(val) {}

void BufferParser<String>::operator()(const FieldBuffer &buffer)
{
    const auto tmp = buffer.ToString();
    auto parsed = String::fromBytes(tmp);
    if (!parsed)
        throw ::userver::storages::postgres::InvalidInputFormat(
            "invalid UTF-8 in PostgreSQL text field"
        );
    value = parsed.value();
}

} // namespace userver::storages::postgres::io
