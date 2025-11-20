insert into host_denylist(host)
values ($1)
on conflict (host) do nothing

