-- name: update_crawl_job_succeeded
update crawl_job
set
    status = 'succeeded',
    finished_at = now(),
    error_category = null,
    error_message = null,
    result_created_at = $2
where id = $1
returning id;
