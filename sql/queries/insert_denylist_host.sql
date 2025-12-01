insert into host_denylist (prefix_key, reason)
values ($1, $2)
on conflict (prefix_key) do nothing
