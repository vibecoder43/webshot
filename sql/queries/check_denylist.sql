-- $1: text[] of prefix_key values to check
select 1
from host_denylist
where prefix_key = any($1)
order by prefix_key
limit 1
