import asyncio
import base64
import contextlib
import pathlib
import time

import pytest


def _basic_proxy_auth(username: str, password: str) -> str:
    token = base64.b64encode(f"{username}:{password}".encode()).decode("ascii")
    return f"Basic {token}"


async def _read_http_request_headers(reader: asyncio.StreamReader) -> bytes:
    # Minimal parser: read until end of headers.
    return await reader.readuntil(b"\r\n\r\n")


def _parse_content_length(headers: bytes) -> int:
    for line in headers.split(b"\r\n"):
        if line.lower().startswith(b"content-length:"):
            _, value = line.split(b":", 1)
            try:
                return int(value.strip() or b"0")
            except ValueError:
                return 0
    return 0


async def _serve_denylist(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
    try:
        try:
            headers = await _read_http_request_headers(reader)
        except (asyncio.IncompleteReadError, asyncio.LimitOverrunError):
            return
        length = _parse_content_length(headers)
        if length:
            await reader.readexactly(length)
        writer.write(b"HTTP/1.1 204 No Content\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
        await writer.drain()
    finally:
        writer.close()
        with contextlib.suppress(Exception):
            await writer.wait_closed()


async def _serve_upstream(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
    # Two endpoints:
    # - /large: returns a 2 MiB body.
    # - /hang: holds the connection open without responding.
    try:
        try:
            headers = await _read_http_request_headers(reader)
        except (asyncio.IncompleteReadError, asyncio.LimitOverrunError):
            return
        first_line = headers.split(b"\r\n", 1)[0]
        try:
            _method, target, _version = first_line.split(b" ", 2)
        except ValueError:
            return

        path = b"/"
        if target.startswith(b"http://") or target.startswith(b"https://"):
            # Absolute-form requests (when requested through a proxy).
            try:
                _, rest = target.split(b"://", 1)
                _, path = rest.split(b"/", 1)
                path = b"/" + path
            except ValueError:
                path = b"/"
        elif target.startswith(b"/"):
            path = target

        if path.startswith(b"/hang"):
            await asyncio.sleep(60)
            return

        body = b"x" * (2 * 1024 * 1024)
        writer.write(
            b"HTTP/1.1 200 OK\r\n"
            + f"Content-Length: {len(body)}\r\n".encode("ascii")
            + b"Content-Type: application/octet-stream\r\n"
            + b"Connection: close\r\n"
            + b"\r\n"
        )
        writer.write(body)
        await writer.drain()
    finally:
        writer.close()
        with contextlib.suppress(Exception):
            await writer.wait_closed()


async def _wait_for_tcp_ready(host: str, port: int, *, timeout_sec: int = 10) -> None:
    deadline = time.monotonic() + timeout_sec
    while True:
        try:
            reader, writer = await asyncio.open_connection(host, port)
            writer.close()
            await writer.wait_closed()
            return
        except OSError:
            if time.monotonic() >= deadline:
                raise
            await asyncio.sleep(0.05)


async def _proxy_get(proxy_port: int, url: str, *, run_id: str) -> bytes:
    auth = _basic_proxy_auth(run_id, "x")
    reader, writer = await asyncio.open_connection("127.0.0.1", proxy_port)
    try:
        authority = url.split("://", 1)[1].split("/", 1)[0]
        req = (
            f"GET {url} HTTP/1.1\r\n"
            f"Host: {authority}\r\n"
            f"Proxy-Authorization: {auth}\r\n"
            f"Connection: close\r\n"
            f"\r\n"
        ).encode("ascii")
        writer.write(req)
        await writer.drain()
        return await reader.read()
    finally:
        writer.close()
        with contextlib.suppress(Exception):
            await writer.wait_closed()


async def _start_mitmdump(
    *,
    proxy_port: int,
    denylist_port: int,
    crawler_size_limit_mib: int,
    crawler_run_timeout_sec: int,
) -> asyncio.subprocess.Process:
    repo_root = pathlib.Path(__file__).resolve().parents[2]
    addon = repo_root / "s6" / "mitm_addon.py"
    assert addon.is_file()

    proc = await asyncio.create_subprocess_exec(
        "mitmdump",
        "--mode",
        "regular",
        "--listen-host",
        "127.0.0.1",
        "--listen-port",
        str(proxy_port),
        "--set",
        "connection_strategy=lazy",
        "--set",
        "upstream_cert=false",
        "--set",
        "showhost=false",
        "--set",
        "validate_inbound_headers=true",
        "--set",
        "webshot_mode=prodlike",
        "--set",
        f"webshot_denylist_url=http://127.0.0.1:{denylist_port}/check",
        "--set",
        f"crawler_size_limit_mib={crawler_size_limit_mib}",
        "--set",
        f"crawler_run_timeout_sec={crawler_run_timeout_sec}",
        "-s",
        str(addon),
        stdout=asyncio.subprocess.DEVNULL,
        stderr=asyncio.subprocess.DEVNULL,
    )
    await _wait_for_tcp_ready("127.0.0.1", proxy_port)
    return proc


async def _stop_proc(proc: asyncio.subprocess.Process) -> None:
    if proc.returncode is not None:
        return
    proc.terminate()
    try:
        await asyncio.wait_for(proc.wait(), timeout=5)
    except TimeoutError:
        proc.kill()
        await proc.wait()


@pytest.mark.asyncio
async def test_proxy_limits_enforces_crawler_size_limit_mib(choose_free_port):
    denylist_server = await asyncio.start_server(_serve_denylist, "127.0.0.1", choose_free_port())
    upstream_server = await asyncio.start_server(_serve_upstream, "127.0.0.1", choose_free_port())
    proxy_port = choose_free_port()
    try:
        denylist_port = denylist_server.sockets[0].getsockname()[1]
        upstream_port = upstream_server.sockets[0].getsockname()[1]
        mitm = await _start_mitmdump(
            proxy_port=proxy_port,
            denylist_port=denylist_port,
            crawler_size_limit_mib=1,
            crawler_run_timeout_sec=30,
        )
        try:
            data = await asyncio.wait_for(
                _proxy_get(
                    proxy_port,
                    f"http://127.0.0.1:{upstream_port}/large",
                    run_id="test-run-size",
                ),
                timeout=10,
            )
        finally:
            await _stop_proc(mitm)

        # The budget is enforced at the transport writer: count raw bytes proxy -> client.
        assert data.startswith(b"HTTP/"), data[:64]
        assert len(data) <= 1 * 1024 * 1024
    finally:
        upstream_server.close()
        denylist_server.close()
        await upstream_server.wait_closed()
        await denylist_server.wait_closed()


@pytest.mark.asyncio
async def test_proxy_limits_enforces_crawler_run_timeout_sec(choose_free_port):
    denylist_server = await asyncio.start_server(_serve_denylist, "127.0.0.1", choose_free_port())
    upstream_server = await asyncio.start_server(_serve_upstream, "127.0.0.1", choose_free_port())
    proxy_port = choose_free_port()
    try:
        denylist_port = denylist_server.sockets[0].getsockname()[1]
        upstream_port = upstream_server.sockets[0].getsockname()[1]
        mitm = await _start_mitmdump(
            proxy_port=proxy_port,
            denylist_port=denylist_port,
            crawler_size_limit_mib=64,
            crawler_run_timeout_sec=1,
        )
        try:
            started = time.monotonic()
            data = await asyncio.wait_for(
                _proxy_get(
                    proxy_port,
                    f"http://127.0.0.1:{upstream_port}/hang",
                    run_id="test-run-timeout",
                ),
                timeout=5,
            )
            elapsed_ms = int((time.monotonic() - started) * 1000)
        finally:
            await _stop_proc(mitm)

        assert elapsed_ms < 4000
        # The upstream never responds; we only care that mitmproxy closed the client connection.
        assert data == b"" or data.startswith(b"HTTP/")
    finally:
        upstream_server.close()
        denylist_server.close()
        await upstream_server.wait_closed()
        await denylist_server.wait_closed()
