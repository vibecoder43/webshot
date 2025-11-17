#pragma once
#include <string_view>

namespace sql {

/** Insert a new capture row and return its id. Args: id, link, host_rev, location. */
inline constexpr std::string_view kInsertWebshot = R"~(
insert into webshot(id, created_at, link, host_rev, location)
values($1, default, $2, $3, $4)
returning id
)~";

/** Fetch `location` for a capture by id. Args: id. */
inline constexpr std::string_view kSelectWebshot = R"~(
select location from webshot where id = $1
)~";

/**
 * List capture ids for a link, newest first. Args: link, limit.
 * First page variant.
 */
inline constexpr std::string_view kSelectWebshotByLinkFirst = R"~(
select id, created_at
from webshot
where link = $1
order by created_at desc, id asc
limit $2
)~";

/**
 * List capture ids for a link after a cursor. Args: link, limit, created_at, id.
 */
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

/**
 * First page of distinct normalized links by prefix. Args: lower, upper, limit.
 */
inline constexpr std::string_view kSelectDistinctLinksByPrefixFirst = R"~(
select link
from webshot
where link >= $1 and ($2 is null or link < $2)
group by link
order by link asc
limit $3
)~";

/**
 * Next page of distinct normalized links by prefix. Args: lower, upper, after_link, limit.
 */
inline constexpr std::string_view kSelectDistinctLinksByPrefixNext = R"~(
select link
from webshot
where link >= $1 and ($2 is null or link < $2) and link > $3
group by link
order by link asc
limit $4
)~";

/** Check if a domain or its parents are deny‑listed. Arg: domain. */
inline constexpr std::string_view kCheckDenylist = R"~(
select 1
from domain_denylist
where $1 = domain or $1 like ('%.' || domain)
order by length(domain) desc
limit 1
)~";

/** Insert domain into denylist if not present. Arg: domain. */
inline constexpr std::string_view kInsertDenylistDomain = R"~(
insert into domain_denylist(domain)
values ($1)
on conflict (domain) do nothing
)~";

/**
 * Page ids by reversed host for fast prefix match on subdomains. Args: host_rev, limit.
 */
inline constexpr std::string_view kSelectIdsByHostOrSubdomainsPaged = R"~(
select id
from webshot
where host_rev = $1 or host_rev like ($1 || '.%')
order by created_at desc, id asc
limit $2
)~";

/** Delete captures by a list of ids. Arg: uuid[]. */
inline constexpr std::string_view kDeleteWebshotsByIds = R"~(
delete from webshot where id = any($1::uuid[])
)~";

}; // namespace sql
