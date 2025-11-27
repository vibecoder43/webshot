select link
from webshot
where link >= $1 and ($2 is null or link < $2)
group by link
order by link asc
limit $3
