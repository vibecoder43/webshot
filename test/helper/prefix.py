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


def _encode_segment_to_ltree_labels(seg: str) -> list[str]:
    # Mirrors C++ chunking: 127 bytes max per label chunk.
    b = seg.encode("utf-8")
    if not b:
        return ["x"]
    out: list[str] = []
    for pos in range(0, len(b), 127):
        out.append("x" + b[pos : pos + 127].hex())
    return out


def prefix_tree_from_prefix_key(prefix_key: str) -> str:
    # Mirrors `prefix::makePrefixTree` in `src/prefix_utils.cpp`.
    if not prefix_key:
        return "h"
    if "/" in prefix_key:
        host_part, path_part = prefix_key.split("/", 1)
    else:
        host_part, path_part = prefix_key, ""

    labels: list[str] = ["h"]
    for host_label in host_part.split("."):
        labels.extend(_encode_segment_to_ltree_labels(host_label))

    if path_part:
        labels.append("p")
        for seg in path_part.split("/"):
            labels.extend(_encode_segment_to_ltree_labels(seg))

    return ".".join(labels)


def prefix_tree_from_link(link: str) -> str:
    return prefix_tree_from_prefix_key(prefix_key_from_link(link))
