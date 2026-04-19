select
    created_at,
    link,
    replay_url
from capture
where id = $1
