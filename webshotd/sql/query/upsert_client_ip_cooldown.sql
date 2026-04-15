insert into client_ip_cooldown (client_ip, expires_at, updated_at)
values ($1, $2, now())
on conflict (client_ip) do update
    set
        expires_at = excluded.expires_at,
        updated_at = now();
