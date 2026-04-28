-- $1: prefix_key (text)
-- $2: prefix_tree as text (ltree input)
-- $3: reason (text)
insert into host_allowlist (prefix_key, prefix_tree, reason)
values ($1, text2ltree($2), $3)
on conflict (prefix_key) do nothing
