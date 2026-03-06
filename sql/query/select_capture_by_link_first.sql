select
    id,
    created_at
from capture
where link = $1
order by created_at desc, id asc
limit $2
