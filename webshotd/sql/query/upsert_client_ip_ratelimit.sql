insert into client_ip_ratelimit (client_ip, expires_at, updated_at)
values ($1::inet, $2, now())
on conflict (client_ip) do update
    set
        expires_at = excluded.expires_at,
        updated_at = now();
