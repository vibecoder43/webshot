select
    created_at,
    link
from capture
where id = $1
