This file records binding requirements and settled architecture decisions.
webshot runs only inside a working delegated cgroup v2 layout managed by webshot.
Browser isolation is mandatory.
Browser resource limits are mandatory.
Runtime design does not couple to systemd integration.
Captures obey the denylist.
A successful crawl emits WACZ.
Nothing must proxy S3
