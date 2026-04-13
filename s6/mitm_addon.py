from __future__ import annotations

import asyncio
import base64
import binascii
import inspect
from typing import TYPE_CHECKING
from urllib.error import HTTPError, URLError
from urllib.parse import urlsplit, urlunsplit
from urllib.request import Request, urlopen

from mitmproxy import ctx, exceptions, http

if TYPE_CHECKING:
    from mitmproxy.proxy import server_hooks

DENYLIST_TIMEOUT_SEC = 5
LOCAL_HOSTS = frozenset({"test-target", "asset.test-target"})
LOCAL_HTTP_PORTS = frozenset({80, 18080})
LOCAL_HTTPS_PORTS = frozenset({443, 18443})
LOCAL_HTTP_ADDRESS = ("127.0.0.1", 18080)
LOCAL_HTTPS_ADDRESS = ("127.0.0.1", 18443)

MAX_RUN_ID_BYTES = 128


def _maybe_await(value):
    if inspect.isawaitable(value):
        return value
    return None


class _BudgetedWriter:
    def __init__(self, inner, proxy: WebshotProxy, client_id: str) -> None:
        self.inner = inner
        self.proxy = proxy
        self.clientId = client_id

    def write(self, data: bytes) -> None:
        limited = self.proxy._limit_client_write(self.clientId, data)
        if not limited:
            return
        self.inner.write(limited)

    async def drain(self) -> None:
        maybe = _maybe_await(self.inner.drain())
        if maybe is not None:
            await maybe

    def close(self) -> None:
        self.inner.close()

    def is_closing(self) -> bool:
        return self.inner.is_closing()

    def write_eof(self) -> None:
        self.inner.write_eof()

    def __getattr__(self, name: str):
        return getattr(self.inner, name)


class WebshotProxy:
    def __init__(self) -> None:
        self.wrappedClientIds: set[str] = set()
        self.clientIdToRunId: dict[str, str] = {}
        self.runIdToClientIds: dict[str, set[str]] = {}
        self.runIdRemainingBytes: dict[str, int] = {}
        self.runIdTimers: dict[str, asyncio.Handle] = {}
        self.runIdClosed: set[str] = set()

    def load(self, loader) -> None:
        loader.add_option(
            name="webshot_mode",
            typespec=str,
            default="prodlike",
            help="webshot runtime mode",
        )
        loader.add_option(
            name="webshot_denylist_url",
            typespec=str,
            default="http://127.0.0.1:8080/v1/denylist/check",
            help="denylist authority URL",
        )
        loader.add_option(
            name="crawler_size_limit_mib",
            typespec=int,
            default=0,
            help="Max browser downstream bytes (MiB) per crawl run, enforced at mitmproxy",
        )
        loader.add_option(
            name="crawler_run_timeout_sec",
            typespec=int,
            default=0,
            help="Max wall-time (sec) per crawl run, enforced at mitmproxy",
        )

    def configure(self, updates) -> None:
        if "webshot_mode" in updates and ctx.options.webshot_mode not in {"dev", "prodlike"}:
            raise exceptions.OptionsError("webshot_mode must be dev or prodlike")
        if "webshot_denylist_url" in updates and ctx.options.webshot_denylist_url.strip() == "":
            raise exceptions.OptionsError("webshot_denylist_url must not be empty")
        # These limits are mandatory; fail hard if left unset or invalid.
        if ctx.options.crawler_size_limit_mib <= 0:
            raise exceptions.OptionsError("crawler_size_limit_mib must be positive")
        if ctx.options.crawler_run_timeout_sec <= 0:
            raise exceptions.OptionsError("crawler_run_timeout_sec must be positive")

    def client_connected(self, client) -> None:
        proxyserver = ctx.master.addons.get("proxyserver")
        if proxyserver is None:
            return
        handler = proxyserver.connections.get(client.id)
        if handler is None:
            return
        conn_io = handler.transports.get(handler.client)
        if conn_io is None:
            return
        writer = conn_io.writer
        if writer is None:
            return
        if client.id in self.wrappedClientIds:
            return
        conn_io.writer = _BudgetedWriter(writer, self, client.id)
        self.wrappedClientIds.add(client.id)

    def client_disconnected(self, client) -> None:
        self.wrappedClientIds.discard(client.id)
        run_id = self.clientIdToRunId.pop(client.id, None)
        if run_id is None:
            return
        client_ids = self.runIdToClientIds.get(run_id)
        if client_ids is not None:
            client_ids.discard(client.id)
            if not client_ids:
                self.runIdToClientIds.pop(run_id, None)
                self.runIdRemainingBytes.pop(run_id, None)
                self.runIdClosed.discard(run_id)
                timer = self.runIdTimers.pop(run_id, None)
                if timer is not None:
                    timer.cancel()

    def requestheaders(self, flow: http.HTTPFlow) -> None:
        self._normalize_local_request(flow)
        self._rewrite_local_upstream(flow)
        if flow.response is not None:
            return
        self._ensure_client_wrapped(flow.client_conn.id)
        self._attach_run_budget_or_challenge(flow)

    def request(self, flow: http.HTTPFlow) -> None:
        self._normalize_local_request(flow)
        self._rewrite_local_upstream(flow)
        if flow.response is not None:
            return
        self._enforce_denylist(flow)

    def responseheaders(self, flow: http.HTTPFlow) -> None:
        self._normalize_local_response_headers(flow)

    def server_connect(self, data: server_hooks.ServerConnectionHookData) -> None:
        if ctx.options.webshot_mode != "dev":
            return
        if data.server.address is None:
            return

        original_host, original_port = data.server.address
        if original_host not in LOCAL_HOSTS:
            return

        if original_port in LOCAL_HTTP_PORTS:
            data.server.address = LOCAL_HTTP_ADDRESS
            data.server.sni = None
            return

        if original_port in LOCAL_HTTPS_PORTS:
            data.server.address = LOCAL_HTTPS_ADDRESS
            data.server.sni = original_host

    def _normalize_local_request(self, flow: http.HTTPFlow) -> None:
        if ctx.options.webshot_mode != "dev":
            return

        local_host = self._resolve_local_host(flow)
        if local_host is None:
            return

        if flow.request.host != local_host:
            flow.request.host = local_host

        if flow.request.port == 18080:
            flow.request.port = 80
        elif flow.request.port == 18443:
            flow.request.port = 443

        referer = flow.request.headers.get("referer")
        if referer:
            flow.request.headers["referer"] = self._rewrite_local_fixture_port_in_url(referer)

        origin = flow.request.headers.get("origin")
        if origin and origin != "null":
            flow.request.headers["origin"] = self._rewrite_local_fixture_port_in_url(origin)

    def _rewrite_local_upstream(self, flow: http.HTTPFlow) -> None:
        if ctx.options.webshot_mode != "dev":
            return

        host = self._resolve_local_host(flow)
        if host is None:
            return

        port = flow.request.port
        if port in LOCAL_HTTP_PORTS:
            flow.server_conn.address = LOCAL_HTTP_ADDRESS
            flow.server_conn.sni = None
            return

        if port in LOCAL_HTTPS_PORTS:
            flow.server_conn.address = LOCAL_HTTPS_ADDRESS
            flow.server_conn.sni = host

    def _resolve_local_host(self, flow: http.HTTPFlow) -> str | None:
        request_host = flow.request.host
        if request_host in LOCAL_HOSTS:
            return request_host

        server_sni = flow.server_conn.sni
        if server_sni in LOCAL_HOSTS:
            return server_sni

        return None

    def _attach_run_budget_or_challenge(self, flow: http.HTTPFlow) -> None:
        run_id = self._extract_run_id_from_proxy_auth(flow)
        if run_id is None:
            flow.response = http.Response.make(
                407,
                b"Proxy authentication required\n",
                {
                    "Content-Type": "text/plain; charset=utf-8",
                    "Proxy-Authenticate": 'Basic realm="webshot"',
                },
            )
            return
        self._attach_client_to_run(flow.client_conn.id, run_id)

    def _ensure_client_wrapped(self, client_id: str) -> None:
        if client_id in self.wrappedClientIds:
            return
        proxyserver = ctx.master.addons.get("proxyserver")
        if proxyserver is None:
            return
        handler = proxyserver.connections.get(client_id)
        if handler is None:
            return
        conn_io = handler.transports.get(handler.client)
        if conn_io is None:
            return
        writer = conn_io.writer
        if writer is None:
            return
        conn_io.writer = _BudgetedWriter(writer, self, client_id)
        self.wrappedClientIds.add(client_id)

    def _extract_run_id_from_proxy_auth(self, flow: http.HTTPFlow) -> str | None:
        header = flow.request.headers.get("proxy-authorization")
        if header is None:
            header = flow.request.headers.get("Proxy-Authorization")
        if not header:
            return None

        parts = header.split(" ", 1)
        if len(parts) != 2 or parts[0].lower() != "basic":
            return None
        try:
            raw = base64.b64decode(parts[1], validate=True)
        except (binascii.Error, ValueError):
            return None
        if len(raw) > MAX_RUN_ID_BYTES + 64:
            return None
        try:
            decoded = raw.decode("utf-8")
        except UnicodeDecodeError:
            return None
        username, sep, _password = decoded.partition(":")
        if sep != ":":
            return None
        username = username.strip()
        if not username:
            return None
        if len(username.encode("utf-8")) > MAX_RUN_ID_BYTES:
            return None
        return username

    def _attach_client_to_run(self, client_id: str, run_id: str) -> None:
        previous = self.clientIdToRunId.get(client_id)
        if previous == run_id:
            return

        if previous is not None:
            old_set = self.runIdToClientIds.get(previous)
            if old_set is not None:
                old_set.discard(client_id)

        self.clientIdToRunId[client_id] = run_id
        self.runIdToClientIds.setdefault(run_id, set()).add(client_id)
        if run_id not in self.runIdRemainingBytes:
            self.runIdRemainingBytes[run_id] = ctx.options.crawler_size_limit_mib * 1024 * 1024
        if run_id not in self.runIdTimers:
            loop = asyncio.get_running_loop()
            timeout_sec = ctx.options.crawler_run_timeout_sec
            self.runIdTimers[run_id] = loop.call_later(
                timeout_sec, lambda: self._close_run(run_id, "run timeout exceeded")
            )

    def _limit_client_write(self, client_id: str, data: bytes) -> bytes:
        run_id = self.clientIdToRunId.get(client_id)
        if run_id is None:
            return data
        if run_id in self.runIdClosed:
            return b""

        remaining = self.runIdRemainingBytes.get(run_id)
        if remaining is None:
            remaining = ctx.options.crawler_size_limit_mib * 1024 * 1024
            self.runIdRemainingBytes[run_id] = remaining
        if remaining <= 0:
            self._close_run(run_id, "crawler_size_limit_mib exceeded")
            return b""

        size = len(data)
        if size <= remaining:
            self.runIdRemainingBytes[run_id] = remaining - size
            return data

        allowed = data[:remaining]
        self.runIdRemainingBytes[run_id] = 0
        self._close_run(run_id, "crawler_size_limit_mib exceeded")
        return allowed

    def _close_run(self, run_id: str, reason: str) -> None:
        if run_id in self.runIdClosed:
            return
        self.runIdClosed.add(run_id)
        client_ids = self.runIdToClientIds.get(run_id)
        if not client_ids:
            return

        proxyserver = ctx.master.addons.get("proxyserver")
        if proxyserver is None:
            return
        for client_id in list(client_ids):
            handler = proxyserver.connections.get(client_id)
            if handler is None:
                continue
            conn_io = handler.transports.get(handler.client)
            if conn_io is None:
                continue
            writer = conn_io.writer
            task = conn_io.handler
            if task is not None:
                task.cancel(reason)
            try:
                if writer is not None and not writer.is_closing():
                    writer.close()
            except OSError:
                pass

    def _rewrite_local_fixture_port_in_url(self, url: str) -> str:
        try:
            parts = urlsplit(url)
        except ValueError:
            return url

        if parts.scheme not in {"http", "https"}:
            return url
        if parts.username or parts.password:
            return url

        hostname = parts.hostname
        if hostname not in LOCAL_HOSTS:
            return url

        port = parts.port
        if port is None:
            return url

        if port == 18080:
            port = 80
        elif port == 18443:
            port = 443

        default_port = 80 if parts.scheme == "http" else 443
        netloc = hostname if port == default_port else f"{hostname}:{port}"
        return urlunsplit((parts.scheme, netloc, parts.path, parts.query, parts.fragment))

    def _normalize_local_response_headers(self, flow: http.HTTPFlow) -> None:
        if ctx.options.webshot_mode != "dev":
            return
        if flow.response is None:
            return

        location = flow.response.headers.get("location")
        if location:
            rewritten = self._rewrite_local_fixture_port_in_url(location)
            flow.response.headers["location"] = self._maybe_make_local_location_relative(
                flow, rewritten
            )

    def _maybe_make_local_location_relative(self, flow: http.HTTPFlow, location: str) -> str:
        local_host = self._resolve_local_host(flow)
        if local_host is None:
            return location

        try:
            parts = urlsplit(location)
        except ValueError:
            return location

        if parts.scheme not in {"http", "https"}:
            return location
        if parts.username or parts.password:
            return location
        if parts.hostname != local_host:
            return location

        default_port = 80 if parts.scheme == "http" else 443
        if parts.port is not None and parts.port != default_port:
            return location
        if parts.scheme != flow.request.scheme:
            return location

        path = parts.path or "/"
        if not path.startswith("/"):
            path = "/" + path

        if parts.query:
            path = f"{path}?{parts.query}"
        if parts.fragment:
            path = f"{path}#{parts.fragment}"
        return path

    def _canonical_denylist_url(self, flow: http.HTTPFlow) -> str:
        request = flow.request
        host = request.host
        port = request.port

        if request.method == "CONNECT" or request.first_line_format == "authority":
            scheme = "https" if port in LOCAL_HTTPS_PORTS else "http"
            path = "/"
        else:
            scheme = request.scheme
            path = request.path

        local_host = self._resolve_local_host(flow)
        if local_host is not None:
            host = local_host

        if path == "*":
            path = "/"
        elif not path.startswith("/"):
            path = "/" + path

        if scheme == "http":
            default_port = 80
        elif scheme == "https":
            default_port = 443
        else:
            default_port = None

        authority = host if default_port is not None and port == default_port else f"{host}:{port}"
        return f"{scheme}://{authority}{path}"

    def _enforce_denylist(self, flow: http.HTTPFlow) -> None:
        denylist_url = self._canonical_denylist_url(flow)
        request = Request(
            url=ctx.options.webshot_denylist_url,
            data=denylist_url.encode("utf-8"),
            method="POST",
            headers={"Content-Type": "text/plain; charset=utf-8"},
        )
        try:
            with urlopen(request, timeout=DENYLIST_TIMEOUT_SEC) as response:
                status = response.getcode()
                body = response.read()
                content_type = response.headers.get("Content-Type")
        except HTTPError as error:
            status = error.code
            body = error.read()
            content_type = error.headers.get("Content-Type")
        except URLError as error:
            self._fail_closed(flow, f"denylist authority unavailable: {error.reason}")
            return
        except Exception as error:
            self._fail_closed(flow, f"denylist authority error: {error}")
            return

        if status == 204:
            return
        if status == 403:
            headers = {} if content_type is None else {"Content-Type": content_type}
            flow.response = http.Response.make(status, body, headers)
            return
        self._fail_closed(flow, f"denylist authority returned unexpected status {status}")

    def _fail_closed(self, flow: http.HTTPFlow, message: str) -> None:
        flow.response = http.Response.make(
            502,
            message.encode("utf-8"),
            {"Content-Type": "text/plain; charset=utf-8"},
        )


addons = [WebshotProxy()]
