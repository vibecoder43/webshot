create table domain_denylist (
    domain text collate "C" not null
        check (domain = lower(domain))
        check (position('.' in domain) > 0),
    created_at timestamptz not null default now(),
    reason text not null,
    constraint domain_denylist_pk primary key (domain)
);

create index if not exists domain_denylist_created_at_idx
    on domain_denylist (created_at desc);

