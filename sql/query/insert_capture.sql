-- $4: prefix_tree as text (ltree input)
insert into capture (id, created_at, link, prefix_key, prefix_tree, location)
values ($1, default, $2, $3, text2ltree($4), $5)
returning id, created_at
