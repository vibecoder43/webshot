select expires_at
from client_ip_cooldown
where client_ip = $1;
