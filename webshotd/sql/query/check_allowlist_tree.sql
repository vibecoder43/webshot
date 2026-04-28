-- $1: request prefix_tree as text (ltree input)
select exists(
    select 1
    from host_allowlist
    where prefix_tree @> text2ltree($1)
)
