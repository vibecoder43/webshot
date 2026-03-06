delete from capture
where id = any($1::uuid [])
