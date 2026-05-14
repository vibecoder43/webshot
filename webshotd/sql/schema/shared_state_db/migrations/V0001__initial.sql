create extension if not exists ltree;

create table host_denylist (
    prefix_key text collate "C" not null,
    prefix_tree ltree not null,
    created_at timestamptz not null default now(),
    reason text not null,
    constraint host_denylist_pk primary key (prefix_key),
    constraint host_denylist_prefix_tree_uniq unique (prefix_tree)
);

create index if not exists host_denylist_created_at_idx
on host_denylist (created_at desc);
create index if not exists host_denylist_prefix_tree_gist_idx
on host_denylist using gist (prefix_tree);

create table host_allowlist (
    prefix_key text collate "C" not null,
    prefix_tree ltree not null,
    created_at timestamptz not null default now(),
    reason text not null,
    constraint host_allowlist_pk primary key (prefix_key),
    constraint host_allowlist_prefix_tree_uniq unique (prefix_tree)
);

create index if not exists host_allowlist_created_at_idx
on host_allowlist (created_at desc);
create index if not exists host_allowlist_prefix_tree_gist_idx
on host_allowlist using gist (prefix_tree);

create table crawl_job (
    id uuid primary key,
    link text collate "C" not null,
    status text not null
    check (status in ('pending', 'running', 'succeeded', 'failed')),
    error_category text
    check (
        error_category is null or error_category in (
            'size_limit',
            'crawler_failed',
            's3_upload_failed',
            'db_insert_failed',
            'internal_server_error'
        )
    ),
    error_message text,
    created_at timestamptz not null default now(),
    started_at timestamptz,
    finished_at timestamptz,
    result_created_at timestamptz,
    result_capture_id uuid,
    constraint crawl_job_succeeded_result
    check (
        status <> 'succeeded' or (result_created_at is not null and result_capture_id is not null)
    )
);

create index if not exists crawl_job_status_created_idx
on crawl_job (status, created_at asc, id);
create index if not exists crawl_job_link_created_idx
on crawl_job (link, created_at desc, id desc);

create table client_ip_ratelimit (
    client_ip inet primary key,
    expires_at timestamptz not null,
    updated_at timestamptz not null default now()
);

create index if not exists client_ip_ratelimit_expires_at_idx
on client_ip_ratelimit (expires_at asc, client_ip);
