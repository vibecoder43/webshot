select id
from webshot
where host_rev = $1 or host_rev like ($1 || '.%')
order by created_at desc, id asc
limit $2

