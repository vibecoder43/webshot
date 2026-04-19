create extension if not exists ltree;

create table capture (
    id uuid primary key,
    created_at timestamptz not null default now(),
    link text collate "C" not null,
    replay_url text collate "C" not null,
    prefix_key text collate "C" not null,
    prefix_tree ltree not null,
    content_sha256 bytea not null,
    constraint capture_content_sha256_len check (octet_length(content_sha256) = 32)
);

-- For prefix/paged scans by link
create index if not exists capture_link_idx on capture (link, created_at desc, id);

-- For dedup lookup (same link + same content)
create index if not exists capture_link_hash_idx
on capture (link, content_sha256, created_at desc, id);

-- For purges and denylist checks via prefix_tree
create index if not exists capture_prefix_tree_gist_idx on capture using gist (prefix_tree);
