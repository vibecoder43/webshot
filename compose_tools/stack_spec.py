from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from compose_tools.common import die


@dataclass(frozen=True)
class ServiceSpec:
    serviceKey: str
    containerName: str
    hasHealthcheck: bool


@dataclass(frozen=True)
class StackSpec:
    mode: str
    compose_file: Path
    services: tuple[ServiceSpec, ...]

    def containerNamesAll(self) -> list[str]:
        return [svc.containerName for svc in self.services]

    def containerNamesHealthchecked(self) -> list[str]:
        return [svc.containerName for svc in self.services if svc.hasHealthcheck]

    def containerNamesNonHealthchecked(self) -> list[str]:
        return [svc.containerName for svc in self.services if not svc.hasHealthcheck]

    def serviceContainerName(self, service_key: str) -> str:
        for svc in self.services:
            if svc.serviceKey == service_key:
                return svc.containerName
        die(
            f"Service '{service_key}' not found in {self.compose_file} (mode='{self.mode}')",
            exit_code=2,
        )
        raise AssertionError("unreachable")


def _compose_file_for_mode(*, mode: str) -> str:
    if mode == "dev":
        return "infra_dev.yaml"
    if mode == "prodlike":
        return "infra_prodlike.yaml"
    die("mode must be 'dev' or 'prodlike'", exit_code=2)
    raise AssertionError("unreachable")


def loadStackSpec(*, mode: str, compose_dir: Path) -> StackSpec:
    compose_file = compose_dir / _compose_file_for_mode(mode=mode)
    if not compose_file.is_file():
        die(f"Missing compose file: {compose_file}", exit_code=2)
    raw = compose_file.read_text(encoding="utf-8")
    services = _parse_services(raw, source=str(compose_file))

    container_names: set[str] = set()
    for svc in services:
        if svc.containerName in container_names:
            die(
                f"Duplicate container_name in {compose_file}: '{svc.containerName}'",
                exit_code=2,
            )
        container_names.add(svc.containerName)

    return StackSpec(mode=mode, compose_file=compose_file, services=tuple(services))


def _parse_services(raw: str, *, source: str) -> list[ServiceSpec]:
    in_services = False
    cur_key: str | None = None
    cur_container_name: str | None = None
    cur_has_healthcheck = False
    out: list[ServiceSpec] = []

    def finish_current() -> None:
        nonlocal cur_key, cur_container_name, cur_has_healthcheck
        if cur_key is None:
            return
        if not cur_container_name:
            die(f"Missing container_name for service '{cur_key}' in {source}", exit_code=2)
        out.append(
            ServiceSpec(
                serviceKey=cur_key,
                containerName=cur_container_name,
                hasHealthcheck=cur_has_healthcheck,
            )
        )
        cur_key = None
        cur_container_name = None
        cur_has_healthcheck = False

    for line_no, raw_line in enumerate(raw.splitlines(), start=1):
        if "\t" in raw_line:
            die(f"Tabs are not supported in compose YAML: {source}:{line_no}", exit_code=2)
        line = raw_line.split("#", 1)[0].rstrip()
        if not line.strip():
            continue

        indent = len(line) - len(line.lstrip(" "))
        if indent % 2 != 0:
            die(
                f"Unexpected indentation (must be multiple of 2): {source}:{line_no}",
                exit_code=2,
            )
        content = line.lstrip(" ")

        if indent == 0 and content == "services:":
            in_services = True
            continue

        if not in_services:
            continue

        if indent == 0:
            finish_current()
            break

        if indent == 2 and content.endswith(":"):
            finish_current()
            cur_key = content[:-1].strip()
            if not cur_key:
                die(f"Empty service key in {source}:{line_no}", exit_code=2)
            continue

        if cur_key is None:
            die(f"Expected service key under services: {source}:{line_no}", exit_code=2)

        if indent == 4 and content.startswith("container_name:"):
            _, _, value = content.partition(":")
            value = value.strip()
            if not value:
                die(
                    f"Empty container_name for service '{cur_key}': {source}:{line_no}", exit_code=2
                )
            cur_container_name = value
            continue

        if indent == 4 and content == "healthcheck:":
            cur_has_healthcheck = True
            continue

    finish_current()

    if not out:
        die(f"Failed to parse any services from compose YAML: {source}", exit_code=2)
    return out
