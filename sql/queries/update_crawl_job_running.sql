-- name: update_crawl_job_running
update crawl_job
set
    status = 'running',
    started_at = now(),
    error_category = null,
    error_message = null,
    finished_at = null,
    result_created_at = null
where id = $1 and status = 'pending'
returning id;
