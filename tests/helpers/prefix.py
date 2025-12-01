from urllib.parse import urlparse


def prefix_key_from_link(link: str) -> str:
    parsed = urlparse(link if "://" in link else f"https://{link}")
    host = parsed.hostname or ""
    path = parsed.path or ""
    labels = host.split(".") if host else []
    labels.reverse()
    host_rev = ".".join(labels) if labels else ""
    if not path:
        return host_rev
    return f"{host_rev}{path}"
