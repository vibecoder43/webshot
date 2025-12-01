insert into webshot (id, created_at, link, prefix_key, location)
values ($1, default, $2, $3, $4)
returning id, created_at
