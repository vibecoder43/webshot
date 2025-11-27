insert into webshot (id, created_at, link, host_rev, location)
values ($1, default, $2, $3, $4)
returning id, created_at
