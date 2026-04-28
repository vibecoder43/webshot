-- $1: prefix_key (text)
delete from host_allowlist
where prefix_key = $1
