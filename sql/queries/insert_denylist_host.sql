insert into host_denylist (host, host_rev, reason)
values ($1, $2, $3)
on conflict (host) do nothing
