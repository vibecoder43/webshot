select
    id,
    created_at
from capture
where
    link = $1
    and (
        created_at < $3
        or (created_at = $3 and id > $4)
    )
order by created_at desc, id asc
limit $2
