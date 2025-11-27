select link
from webshot
where link >= $1 and ($2 is null or link < $2) and link > $3
group by link
order by link asc
limit $4
