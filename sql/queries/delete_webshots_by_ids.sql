delete from webshot
where id = any($1::uuid [])
