-- $4: prefix_tree as text (ltree input)
-- $5: content_sha256 (sha256 bytes, 32 bytes)
-- $6: replay_url (absolute URL that ReplayWeb should open for this capture)
insert into capture (id, created_at, link, prefix_key, prefix_tree, content_sha256, replay_url)
values ($1, default, $2, $3, text2ltree($4), $5, $6)
returning id, created_at
