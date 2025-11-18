create table host_denylist (
    host text collate "C" not null
        check (host = lower(host))
        check (position('.' in host) > 0),
    created_at timestamptz not null default now(),
    reason text not null,
    constraint host_denylist_pk primary key (host)
);

create index if not exists host_denylist_created_at_idx
    on host_denylist (created_at desc);

