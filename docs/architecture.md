# settled architecture decisions.

- `webshotd` runs only inside a working delegated cgroup v2 layout.
- Browser isolation is mandatory.
- Browser resource limits are mandatory.
- Runtime design does not couple to systemd integration.
- Captures obey the denylist and allowlist access policies.
- Nothing must proxy S3.
