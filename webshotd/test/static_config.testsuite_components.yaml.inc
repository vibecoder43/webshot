    browser_probe:
      path: /tests/browser-probe
      method: POST
      monitor-handler: true
      task_processor: main-task-processor
      request_timeout_ms: 30000
      max_request_size: *max-data-per-request
      devtools_startup_timeout_ms: 10000
      cdp_handshake_timeout_ms: 10000
      cdp_command_timeout_ms: 10000
      devtools_poll_interval_ms: 100
      browser_stop_timeout_ms: 2000
      cdp_max_remote_payload_bytes: 33554432
      local_fixture_rewrite: true
      testsuite_loopback_ports: []
