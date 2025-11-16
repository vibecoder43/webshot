create table webshot (
    id uuid primary key,
    created_at timestamptz not null default now(),
    link text collate "C" not null,
    host_rev text collate "C" not null,
    location text not null
);

-- For prefix/paged scans by link
create index if not exists webshot_link_idx on webshot(link, created_at desc, id);

-- For exact host match purges (via host_rev equality) and fast LIKE prefix on reversed host
create index if not exists webshot_host_rev_like_idx on webshot (host_rev text_pattern_ops);
