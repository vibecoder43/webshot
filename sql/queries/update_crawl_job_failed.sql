update crawl_job
set
    status = 'failed',
    finished_at = now(),
    error_category = $2,
    error_message = $3
where id = $1
returning id;
