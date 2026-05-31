def _require_components(config_yaml) -> dict:
    try:
        components = config_yaml["components_manager"]["components"]
    except KeyError as exc:
        raise RuntimeError("config_yaml missing components_manager.components") from exc
    if not isinstance(components, dict):
        raise RuntimeError("config_yaml components_manager.components must be a dict")
    return components


def set_component_field(config_yaml, component: str, field: str, value) -> None:
    components = _require_components(config_yaml)
    cfg = components.get(component)
    if not isinstance(cfg, dict):
        raise RuntimeError(f"component {component!r} config must be a dict")
    cfg[field] = value


def set_config_field(config_yaml, field: str, value) -> None:
    set_component_field(config_yaml, "config", field, value)


def set_config_var(config_vars, name: str, value) -> None:
    config_vars[name] = value


def set_config_var_and_field(config_yaml, config_vars, var_name: str, field: str, value) -> None:
    # Prefer config_vars, but also patch config_yaml for robustness: some testsuite paths
    # may write config_vars once per session, while per-test oneshot hooks still need to apply.
    set_config_var(config_vars, var_name, value)
    set_config_field(config_yaml, field, value)


def enable_allowlist_only(config_yaml, config_vars) -> None:
    set_config_var_and_field(config_yaml, config_vars, "allowlist_only", "allowlist_only", True)


def enable_https_only(config_yaml, config_vars) -> None:
    set_config_var_and_field(config_yaml, config_vars, "https_only", "https_only", True)


def set_url_bytes_max(config_yaml, config_vars, value: int) -> None:
    set_config_var_and_field(config_yaml, config_vars, "url_bytes_max", "url_bytes_max", value)
