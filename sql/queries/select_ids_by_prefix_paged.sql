select id
from webshot
where prefix_key >= $1 and ($2 is null or prefix_key < $2)
order by created_at desc, id asc
limit $3
