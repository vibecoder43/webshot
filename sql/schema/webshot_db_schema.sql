create table webshot (
    id uuid primary key default gen_random_uuid(),
    created_at timestamptz not null default now(),
    link text collate "C" not null
);

create index items_url_prefix_idx on webshot(link, created_at desc, id);
