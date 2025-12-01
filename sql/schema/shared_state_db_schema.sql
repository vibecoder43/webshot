create table host_denylist (
    prefix_key text collate "C" not null,
    created_at timestamptz not null default now(),
    reason text not null,
    constraint host_denylist_pk primary key (prefix_key)
);

create index if not exists host_denylist_created_at_idx
on host_denylist (created_at desc);
create index if not exists host_denylist_prefix_key_like_idx
on host_denylist (prefix_key text_pattern_ops);

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
    result_created_at timestamptz
);

create index if not exists crawl_job_status_created_idx
on crawl_job (status, created_at asc, id);
