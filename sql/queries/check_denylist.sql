select 1
from host_denylist
where $1 = host or $1 like ('%.' || host)
order by length(host) desc
limit 1

