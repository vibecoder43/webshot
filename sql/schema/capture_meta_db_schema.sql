create table webshot (
    id uuid primary key,
    created_at timestamptz not null default now(),
    link text collate "C" not null,
    prefix_key text collate "C" not null,
    location text not null
);

-- For prefix/paged scans by link
create index if not exists webshot_link_idx on webshot (link, created_at desc, id);

-- For purges and denylist-prefix scans via prefix_key
create index if not exists webshot_prefix_key_like_idx on webshot (prefix_key text_pattern_ops);
