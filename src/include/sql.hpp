#pragma once
#include <string_view>

namespace sql {

inline constexpr std::string_view kInsertWebshot = R"~(
insert into webshot(id, created_at, link, host_rev, location)
values($1, default, $2, $3, $4)
returning id
)~";

inline constexpr std::string_view kSelectWebshot = R"~(
select location from webshot where id = $1
)~";

inline constexpr std::string_view kSelectWebshotByLinkFirst = R"~(
select id, created_at
from webshot
where link = $1
order by created_at desc, id asc
limit $2
)~";

inline constexpr std::string_view kSelectWebshotByLinkNext = R"~(
select id, created_at
from webshot
where link = $1
  and (
    created_at < $3
    or (created_at = $3 and id > $4)
  )
order by created_at desc, id asc
limit $2
)~";

inline constexpr std::string_view kSelectDistinctLinksByPrefixFirst = R"~(
select link
from webshot
where link >= $1 and ($2 is null or link < $2)
group by link
order by link asc
limit $3
)~";

inline constexpr std::string_view kSelectDistinctLinksByPrefixNext = R"~(
select link
from webshot
where link >= $1 and ($2 is null or link < $2) and link > $3
group by link
order by link asc
limit $4
)~";

inline constexpr std::string_view kCheckDenylist = R"~(
select 1
from domain_denylist
where $1 = domain or $1 like ('%.' || domain)
order by length(domain) desc
limit 1
)~";

inline constexpr std::string_view kSelectIdsByHostOrSubdomainsPaged = R"~(
select id
from webshot
where host_rev = $1 or host_rev like ($1 || '.%')
order by created_at desc, id asc
limit $2
)~";

inline constexpr std::string_view kDeleteWebshotsByIds = R"~(
delete from webshot where id = any($1::uuid[])
)~";

}; // namespace sql
