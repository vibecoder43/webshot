from __future__ import annotations

import datetime as dt
import gzip
import hashlib
import io
import json
import pathlib
import re
import shutil
import tempfile
import time
import zipfile
from collections import deque

from aiohttp import web
from playwright.async_api import Error as PlaywrightError
from playwright.async_api import TimeoutError as PlaywrightTimeoutError
from playwright.async_api import async_playwright

_PAGE_LOAD_TIMEOUT = 20.0
_EXPECTED_WACZ_VERSION = "1.1.1"
_REQUIRED_WACZ_FILES = {
    "archive/data.warc.gz",
    "datapackage.json",
    "indexes/index.cdxj",
    "logs/stderr.log",
    "logs/stdout.log",
    "pages/pages.jsonl",
}
_REPLAY_HTML = """<!doctype html>
<html lang="en">
  <head>
    <meta charset="utf-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1" />
    <title>Webshot Replay QA</title>
    <style>
      html, body {
        width: 100%;
        height: 100%;
        margin: 0;
      }

      replay-web-page {
        display: block;
        width: 100%;
        height: 100%;
      }
    </style>
    <script src="/vendor/replaywebpage/ui.js"></script>
  </head>
  <body>
    <replay-web-page
      id="replay"
      replayBase="/vendor/replaywebpage/"
      source="/source.wacz"
      config='{"sourceType":"wacz"}'
      embed="replayonly"
    ></replay-web-page>
  </body>
</html>
"""

_REPLAY_INDEX_HTML = """<!doctype html>
<html lang="en">
  <head>
    <meta charset="utf-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1" />
    <title>ReplayWeb.page</title>
    <script src="./ui.js"></script>
  </head>
  <body>
    <replay-app-main></replay-app-main>
  </body>
</html>
"""


def _parse_replay_range(header: str, total: int) -> tuple[int, int] | None:
    match = re.fullmatch(r"bytes=(\d*)-(\d*)", header.strip())
    if not match:
        return None

    start_text, end_text = match.groups()
    if not start_text and not end_text:
        return None

    if not start_text:
        length = int(end_text)
        if length <= 0:
            return None
        length = min(length, total)
        return total - length, total - 1

    start = int(start_text)
    end = total - 1 if not end_text else int(end_text)
    if start > end or start >= total:
        return None
    return start, min(end, total - 1)


def _to_replay_timestamp(ts: str) -> str:
    value = ts
    if value.endswith("Z"):
        value = value[:-1] + "+00:00"
    parsed = dt.datetime.fromisoformat(value)
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=dt.timezone.utc)
    return parsed.astimezone(dt.timezone.utc).strftime("%Y%m%d%H%M%S")


def _parse_rfc3339(value: str, *, field_name: str) -> None:
    text = value[:-1] + "+00:00" if value.endswith("Z") else value
    try:
        parsed = dt.datetime.fromisoformat(text)
    except ValueError as exc:
        raise AssertionError(f"{field_name}: invalid RFC3339 timestamp {value!r}") from exc
    if parsed.tzinfo is None:
        raise AssertionError(f"{field_name}: timezone offset is required in {value!r}")


def _require_object(value, *, label: str) -> dict[str, object]:
    if not isinstance(value, dict):
        raise AssertionError(f"{label}: expected JSON object")
    return value


def _require_string(value, *, label: str) -> str:
    if not isinstance(value, str) or not value:
        raise AssertionError(f"{label}: expected non-empty string")
    return value


def _require_bool(value, *, label: str) -> bool:
    if not isinstance(value, bool):
        raise AssertionError(f"{label}: expected boolean")
    return value


def _require_int(value, *, label: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise AssertionError(f"{label}: expected integer")
    return value


def _load_zip_entries(path: pathlib.Path) -> tuple[list[str], dict[str, bytes], zipfile.ZipFile]:
    zf = zipfile.ZipFile(path)
    names = zf.namelist()
    entries = {name: zf.read(name) for name in names}
    return names, entries, zf


def _validate_pages_jsonl(raw: bytes, *, path_name: str) -> list[dict[str, str]]:
    lines = raw.decode("utf-8").splitlines()
    if not lines:
        raise AssertionError(f"{path_name}: file is empty")

    header = _require_object(json.loads(lines[0]), label=f"{path_name}: header")
    if header.get("format") != "json-pages-1.0":
        raise AssertionError(f"{path_name}: unexpected header format {header.get('format')!r}")
    _require_string(header.get("id"), label=f"{path_name}: header.id")
    _require_string(header.get("title"), label=f"{path_name}: header.title")
    _require_bool(header.get("hasText"), label=f"{path_name}: header.hasText")

    pages: list[dict[str, str]] = []
    for index, line in enumerate(lines[1:], start=2):
        if not line:
            raise AssertionError(f"{path_name}:{index}: blank line not allowed")
        entry = _require_object(json.loads(line), label=f"{path_name}:{index}")
        url = _require_string(entry.get("url"), label=f"{path_name}:{index}: url")
        ts = _require_string(entry.get("ts"), label=f"{path_name}:{index}: ts")
        _parse_rfc3339(ts, field_name=f"{path_name}:{index}: ts")
        pages.append({"url": url, "ts": ts, "mime": str(entry.get("mime", ""))})
    return pages


def _validate_cdxj(raw: bytes, warc_gz: bytes) -> None:
    for index, line in enumerate(raw.splitlines(), start=1):
        if not line:
            continue
        json_pos = line.find(b"{")
        if json_pos <= 0:
            raise AssertionError(f"indexes/index.cdxj:{index}: missing JSON payload")
        prefix = line[:json_pos].decode("utf-8").strip()
        try:
            key, timestamp = prefix.split(" ", 1)
        except ValueError as exc:
            raise AssertionError(f"indexes/index.cdxj:{index}: invalid prefix {prefix!r}") from exc
        if not key:
            raise AssertionError(f"indexes/index.cdxj:{index}: empty key")
        if not timestamp.isdigit():
            raise AssertionError(f"indexes/index.cdxj:{index}: invalid timestamp {timestamp!r}")

        record = _require_object(
            json.loads(line[json_pos:].decode("utf-8")),
            label=f"indexes/index.cdxj:{index}",
        )
        _require_string(record.get("url"), label=f"indexes/index.cdxj:{index}: url")
        _require_string(record.get("filename"), label=f"indexes/index.cdxj:{index}: filename")
        _require_string(record.get("mime"), label=f"indexes/index.cdxj:{index}: mime")
        _require_string(record.get("status"), label=f"indexes/index.cdxj:{index}: status")
        offset = int(
            _require_string(record.get("offset"), label=f"indexes/index.cdxj:{index}: offset")
        )
        length = int(
            _require_string(record.get("length"), label=f"indexes/index.cdxj:{index}: length")
        )
        if record["filename"] != "archive/data.warc.gz":
            raise AssertionError(
                f"indexes/index.cdxj:{index}: unexpected filename {record['filename']!r}"
            )
        if offset < 0 or length <= 0 or offset + length > len(warc_gz):
            raise AssertionError(f"indexes/index.cdxj:{index}: invalid byte range")
        chunk = warc_gz[offset : offset + length]
        if chunk[:2] != b"\x1f\x8b":
            raise AssertionError(
                f"indexes/index.cdxj:{index}: range does not start with gzip header"
            )
        decompressed = gzip.decompress(chunk)
        if not decompressed.startswith(b"WARC/1.1\r\n"):
            raise AssertionError(f"indexes/index.cdxj:{index}: range is not a WARC record")


def _validate_datapackage(entries: dict[str, bytes]) -> None:
    datapackage = _require_object(
        json.loads(entries["datapackage.json"].decode("utf-8")),
        label="datapackage.json",
    )
    if datapackage.get("profile") != "data-package":
        raise AssertionError(f"datapackage.json: unexpected profile {datapackage.get('profile')!r}")
    if datapackage.get("wacz_version") != _EXPECTED_WACZ_VERSION:
        raise AssertionError(
            f"datapackage.json: unexpected wacz_version {datapackage.get('wacz_version')!r}"
        )
    _require_string(datapackage.get("software"), label="datapackage.json: software")
    _require_string(datapackage.get("title"), label="datapackage.json: title")
    created = _require_string(datapackage.get("created"), label="datapackage.json: created")
    _parse_rfc3339(created, field_name="datapackage.json: created")

    resources = datapackage.get("resources")
    if not isinstance(resources, list) or not resources:
        raise AssertionError("datapackage.json: resources must be a non-empty array")

    seen_paths: set[str] = set()
    listed_paths: set[str] = set()
    for index, item in enumerate(resources, start=1):
        resource = _require_object(item, label=f"datapackage.json: resources[{index}]")
        _require_string(resource.get("name"), label=f"datapackage.json: resources[{index}].name")
        path_name = _require_string(
            resource.get("path"), label=f"datapackage.json: resources[{index}].path"
        )
        bytes_value = _require_int(
            resource.get("bytes"), label=f"datapackage.json: resources[{index}].bytes"
        )
        if path_name in seen_paths:
            raise AssertionError(f"datapackage.json: duplicate resource path {path_name!r}")
        seen_paths.add(path_name)
        listed_paths.add(path_name)
        if path_name not in entries:
            raise AssertionError(f"datapackage.json: missing listed file {path_name!r}")
        if bytes_value != len(entries[path_name]):
            raise AssertionError(
                f"datapackage.json: incorrect bytes for {path_name!r}: "
                f"{bytes_value} != {len(entries[path_name])}"
            )

    actual_paths = set(entries) - {"datapackage.json"}
    if listed_paths != actual_paths:
        raise AssertionError(
            f"datapackage.json: resource paths mismatch: listed={sorted(listed_paths)!r} "
            f"actual={sorted(actual_paths)!r}"
        )


def _validate_wacz_structure(path: pathlib.Path) -> None:
    names, entries, zf = _load_zip_entries(path)
    try:
        if len(names) != len(set(names)):
            raise AssertionError(f"{path.name}: duplicate ZIP entry names are not allowed")
        missing = _REQUIRED_WACZ_FILES - set(entries)
        if missing:
            raise AssertionError(f"{path.name}: missing required files {sorted(missing)!r}")
        if zf.getinfo("archive/data.warc.gz").compress_type != zipfile.ZIP_STORED:
            raise AssertionError(f"{path.name}: archive/data.warc.gz must use ZIP store mode")

        _validate_datapackage(entries)
        pages = _validate_pages_jsonl(entries["pages/pages.jsonl"], path_name="pages/pages.jsonl")
        _validate_cdxj(entries["indexes/index.cdxj"], entries["archive/data.warc.gz"])
        if not gzip.decompress(entries["archive/data.warc.gz"]).startswith(b"WARC/1.1\r\n"):
            raise AssertionError(
                f"{path.name}: archive/data.warc.gz is not a valid WARC gzip stream"
            )

        for page in pages:
            mime = page["mime"]
            if mime and not mime.startswith("text/html"):
                continue
            _to_replay_timestamp(page["ts"])
    finally:
        zf.close()


def _parse_wacz_pages(wacz: bytes) -> list[dict[str, str]]:
    with zipfile.ZipFile(io.BytesIO(wacz)) as zf:
        pages = _validate_pages_jsonl(zf.read("pages/pages.jsonl"), path_name="pages/pages.jsonl")

    return [
        {"url": page["url"], "ts": page["ts"]}
        for page in pages
        if not page["mime"] or page["mime"].startswith("text/html")
    ]


def _validate_wacz_bytes(path: pathlib.Path, expected: bytes) -> None:
    actual = path.read_bytes()
    if actual != expected:
        expected_digest = hashlib.sha256(expected).hexdigest()
        actual_digest = hashlib.sha256(actual).hexdigest()
        raise AssertionError(
            f"{path.name}: WACZ bytes changed after write/read roundtrip: "
            f"{actual_digest} != {expected_digest}"
        )


class _ReplayServer:
    def __init__(self, wacz: bytes, ui_js: bytes, sw_js: bytes):
        self.wacz = wacz
        self.ui_js = ui_js
        self.sw_js = sw_js
        self.runner: web.AppRunner | None = None
        self.site: web.BaseSite | None = None
        self.base_url = ""
        self.home_url = ""
        self.replay_prefix = ""

    async def __aenter__(self) -> _ReplayServer:
        app = web.Application()
        app.router.add_get("/vendor/replaywebpage/", self._handle_index)
        app.router.add_get("/vendor/replaywebpage/index.html", self._handle_index)
        app.router.add_get("/vendor/replaywebpage/qa.html", self._handle_home)
        app.router.add_get("/vendor/replaywebpage/ui.js", self._handle_ui_js)
        app.router.add_get("/vendor/replaywebpage/sw.js", self._handle_sw_js)
        app.router.add_route("*", "/source.wacz", self._handle_source)
        self.runner = web.AppRunner(app)
        await self.runner.setup()
        self.site = web.TCPSite(self.runner, "127.0.0.1", 0)
        await self.site.start()
        sockets = getattr(self.site, "_server", None).sockets
        port = int(sockets[0].getsockname()[1])
        self.base_url = f"http://127.0.0.1:{port}"
        self.home_url = f"{self.base_url}/vendor/replaywebpage/qa.html"
        self.replay_prefix = f"{self.base_url}/vendor/replaywebpage/w/"
        return self

    async def __aexit__(self, exc_type, exc, tb) -> None:
        if self.runner is not None:
            await self.runner.cleanup()

    async def _handle_home(self, _request: web.Request) -> web.Response:
        return web.Response(text=_REPLAY_HTML, content_type="text/html")

    async def _handle_index(self, _request: web.Request) -> web.Response:
        return web.Response(text=_REPLAY_INDEX_HTML, content_type="text/html")

    async def _handle_ui_js(self, _request: web.Request) -> web.Response:
        return web.Response(body=self.ui_js, content_type="application/javascript")

    async def _handle_sw_js(self, _request: web.Request) -> web.Response:
        return web.Response(body=self.sw_js, content_type="application/javascript")

    async def _handle_source(self, request: web.Request) -> web.StreamResponse:
        total = len(self.wacz)
        headers = {
            "Accept-Ranges": "bytes",
            "Content-Type": "application/wacz+zip",
        }
        range_header = request.headers.get("Range")
        if range_header:
            byte_range = _parse_replay_range(range_header, total)
            if byte_range is None:
                return web.Response(status=416, headers=headers)
            start, end = byte_range
            body = self.wacz[start : end + 1]
            headers["Content-Length"] = str(len(body))
            headers["Content-Range"] = f"bytes {start}-{end}/{total}"
            if request.method == "HEAD":
                return web.Response(status=206, headers=headers)
            return web.Response(status=206, body=body, headers=headers)

        headers["Content-Length"] = str(total)
        if request.method == "HEAD":
            return web.Response(status=200, headers=headers)
        return web.Response(status=200, body=self.wacz, headers=headers)


class ReplayWaczValidator:
    def __init__(self, service_binary: pathlib.Path):
        vendor_dir = service_binary.parent.parent / "web_ui" / "vendor" / "replaywebpage"
        self.ui_js = vendor_dir.joinpath("ui.js").read_bytes()
        self.sw_js = vendor_dir.joinpath("sw.js").read_bytes()
        self.browser_path = self._require_browser_binary()
        self.playwright = None
        self.browser = None
        self.page = None
        self.console_messages = deque(maxlen=20)
        self.page_errors = deque(maxlen=20)

    async def __aenter__(self) -> ReplayWaczValidator:
        self.playwright = await async_playwright().start()
        self.browser = await self.playwright.chromium.launch(
            executable_path=self.browser_path,
            headless=True,
            args=["--disable-gpu"],
        )
        self.page = await self.browser.new_page()
        self.page.on("console", self._record_console_message)
        self.page.on("pageerror", self._record_page_error)
        return self

    async def __aexit__(self, exc_type, exc, tb) -> None:
        if self.page is not None:
            await self.page.close()
        if self.browser is not None:
            await self.browser.close()
        if self.playwright is not None:
            await self.playwright.stop()

    async def validate_bytes(self, wacz: bytes, *, object_name: str) -> None:
        with tempfile.TemporaryDirectory(prefix="wacz-qa-") as tmpdir:
            path = pathlib.Path(tmpdir) / f"{object_name}.wacz"
            path.write_bytes(wacz)
            _validate_wacz_structure(path)
            _validate_wacz_bytes(path, wacz)

        pages = _parse_wacz_pages(wacz)
        if not pages:
            return

        async with _ReplayServer(wacz, self.ui_js, self.sw_js) as server:
            await self._open_home(server)
            for page in pages:
                await self._validate_page(server, object_name, page["url"], page["ts"])

    def _require_browser_binary(self) -> str:
        path = shutil.which("chromium")
        if path:
            return path
        raise RuntimeError("unable to locate chromium binary for WACZ replay validation")

    def _record_console_message(self, message) -> None:
        location = message.location
        where = ""
        if location.get("url"):
            where = f" @{location['url']}:{location.get('lineNumber', 0)}"
        self.console_messages.append(f"{message.type}: {message.text}{where}")

    def _record_page_error(self, error) -> None:
        self.page_errors.append(str(error))

    def _format_replay_diagnostics(self) -> str:
        if self.page is None:
            return "page unavailable"

        frame_urls = [f"{index}:{frame.url!r}" for index, frame in enumerate(self.page.frames)]
        parts = [
            f"page_url={self.page.url!r}",
            f"frame_count={len(self.page.frames)}",
            f"frame_urls={frame_urls!r}",
        ]
        if self.console_messages:
            parts.append(f"console_tail={list(self.console_messages)[-5:]!r}")
        if self.page_errors:
            parts.append(f"page_errors={list(self.page_errors)[-5:]!r}")
        return ", ".join(parts)

    def _raise_replay_error(self, message: str) -> None:
        raise AssertionError(f"{message} ({self._format_replay_diagnostics()})")

    async def _get_replay_frame(self):
        assert self.page is not None
        replay = await self.page.query_selector("#replay")
        if replay is None:
            return None

        try:
            iframe_handle = await replay.evaluate_handle(
                "(node) => node.shadowRoot?.querySelector('iframe') ?? null"
            )
            try:
                iframe = iframe_handle.as_element()
                if iframe is None:
                    return None
                return await iframe.content_frame()
            finally:
                await iframe_handle.dispose()
        finally:
            await replay.dispose()

    async def _require_replay_frame(self):
        assert self.page is not None
        deadline = time.monotonic() + 10.0

        # ReplayWeb.page owns the iframe inside shadow DOM; frame ordering is not stable.
        while time.monotonic() < deadline:
            frame = await self._get_replay_frame()
            if frame is not None:
                return frame
            await self.page.wait_for_timeout(50)

        self._raise_replay_error("replay iframe was not initialized")

    async def _wait_for_replay_iframe_controller(self) -> None:
        assert self.page is not None
        deadline = time.monotonic() + 10.0

        while time.monotonic() < deadline:
            controller_ready = await self.page.evaluate(
                """
                () => {
                  const replay = document.getElementById("replay");
                  const iframe = replay?.shadowRoot?.querySelector("iframe");
                  const iframeWindow = iframe?.contentWindow;
                  return Boolean(
                    iframeWindow?.navigator?.serviceWorker?.controller
                  );
                }
                """
            )
            if controller_ready:
                return
            await self.page.wait_for_timeout(100)

        self._raise_replay_error("replay iframe was not controlled by service worker")

    async def _get_replay_content(
        self, replay_frame, *, object_name: str, url: str
    ) -> str:
        assert self.page is not None

        for attempt in range(2):
            try:
                return await replay_frame.content()
            except PlaywrightError as exc:
                is_navigation_race = "page is navigating and changing the content" in str(exc)
                if not is_navigation_race or attempt == 1:
                    raise AssertionError(
                        f"{object_name}: replay content read failed for {url!r}: {exc} "
                        f"({self._format_replay_diagnostics()})"
                    ) from exc
                await self.page.wait_for_timeout(100)
                replay_frame = await self._require_replay_frame()

        raise AssertionError(
            f"{object_name}: replay content read failed for {url!r} "
            f"({self._format_replay_diagnostics()})"
        )

    async def _open_home(self, server: _ReplayServer) -> None:
        assert self.page is not None
        try:
            await self.page.goto(
                server.home_url, wait_until="load", timeout=int(_PAGE_LOAD_TIMEOUT * 1000)
            )
            await self.page.evaluate(
                """
                () => new Promise((resolve, reject) => {
                  const deadline = Date.now() + 10000;
                  const poll = async () => {
                    await customElements.whenDefined("replay-web-page");
                    try {
                      await navigator.serviceWorker.ready;
                    } catch (error) {
                      reject(error);
                      return;
                    }
                    const replay = document.getElementById("replay");
                    const iframe = replay?.shadowRoot?.querySelector("iframe");
                    const controlled = Boolean(
                      iframe?.contentWindow?.navigator?.serviceWorker?.controller
                    );
                    if (replay && iframe && controlled) {
                      resolve(true);
                      return;
                    }
                    if (Date.now() >= deadline) {
                      reject(
                        new Error(
                          "replay iframe never became service-worker controlled"
                        )
                      );
                      return;
                    }
                    setTimeout(poll, 50);
                  };
                  poll();
                })
                """
            )
            await self._require_replay_frame()
            await self._wait_for_replay_iframe_controller()
        except PlaywrightTimeoutError as exc:
            raise AssertionError(
                f"replay home timed out ({self._format_replay_diagnostics()})"
            ) from exc
        except AssertionError:
            raise
        except Exception as exc:
            raise AssertionError(
                f"replay home failed to initialize: {exc} ({self._format_replay_diagnostics()})"
            ) from exc

    async def _validate_page(
        self, server: _ReplayServer, object_name: str, url: str, ts: str
    ) -> None:
        assert self.page is not None
        replay_frame = await self._get_replay_frame()
        if replay_frame is None:
            await self._open_home(server)
            replay_frame = await self._require_replay_frame()

        replay_timestamp = _to_replay_timestamp(ts)
        replay_url = f"{server.base_url}/vendor/replaywebpage/w/replay/{replay_timestamp}mp_/{url}"
        try:
            response = await replay_frame.goto(
                replay_url,
                wait_until="load",
                timeout=int(_PAGE_LOAD_TIMEOUT * 1000),
            )
        except PlaywrightTimeoutError as exc:
            raise AssertionError(
                f"{object_name}: replay timed out for {url!r} ({self._format_replay_diagnostics()})"
            ) from exc

        if response is None:
            self._raise_replay_error(
                f"{object_name}: replay produced no navigation response for {url!r}"
            )

        status = response.status
        mime_type = response.headers.get("content-type", "")
        replay_frame = await self._require_replay_frame()
        text = await self._get_replay_content(
            replay_frame, object_name=object_name, url=url
        )
        if status != 200:
            self._raise_replay_error(
                f"{object_name}: replay failed for {url!r}: expected HTTP 200, got {status}"
            )
        if not mime_type.startswith("text/html"):
            self._raise_replay_error(
                f"{object_name}: replay failed for {url!r}: expected HTML, got {mime_type!r}"
            )
        if "Archived Page Not Found" in text:
            self._raise_replay_error(
                f"{object_name}: replay failed for {url!r}: "
                "ReplayWeb.page reported Archived Page Not Found"
            )
        await self.page.wait_for_timeout(100)
        replay_frame = await self._get_replay_frame()
        if replay_frame is None:
            self._raise_replay_error(
                f"{object_name}: replay failed for {url!r}: "
                "replay frame disappeared after navigation"
            )
