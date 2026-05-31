_S3_GATE_HOST = "127.0.0.1"
_S3_GATE_TIMEOUT_MS = 1000
_s3_gate_port: int | None = None


def set_s3_gate_port(port: int) -> None:
    global _s3_gate_port
    if port <= 0:
        raise RuntimeError(f"invalid S3 gate port: {port}")
    _s3_gate_port = port


def enable_s3_gate(config_yaml, config_vars) -> None:
    if _s3_gate_port is None:
        raise RuntimeError("S3 gate port must be selected before enabling S3 gate config")

    from helper.config_hooks import set_component_field, set_config_field, set_config_var

    set_config_var(config_vars, "s3_endpoint", f"http://{_S3_GATE_HOST}:{_s3_gate_port}")
    set_config_field(config_yaml, "s3_timeout_ms", _S3_GATE_TIMEOUT_MS)

    set_component_field(config_yaml, "http-client-core", "testsuite-timeout", "1s")
