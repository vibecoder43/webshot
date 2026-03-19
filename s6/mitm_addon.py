from __future__ import annotations

from typing import TYPE_CHECKING
from urllib.error import HTTPError, URLError
from urllib.parse import urlsplit, urlunsplit
from urllib.request import Request, urlopen

from mitmproxy import ctx, exceptions, http

if TYPE_CHECKING:
    from mitmproxy.proxy import server_hooks

DENYLIST_TIMEOUT_SEC = 5.0
LOCAL_HOSTS = frozenset({"test-target", "asset.test-target"})
LOCAL_HTTP_PORTS = frozenset({80, 18080})
LOCAL_HTTPS_PORTS = frozenset({443, 18443})
LOCAL_HTTP_ADDRESS = ("127.0.0.1", 18080)
LOCAL_HTTPS_ADDRESS = ("127.0.0.1", 18443)


class WebshotProxy:
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

    def configure(self, updates) -> None:
        if "webshot_mode" in updates and ctx.options.webshot_mode not in {"dev", "prodlike"}:
            raise exceptions.OptionsError("webshot_mode must be dev or prodlike")
        if "webshot_denylist_url" in updates and ctx.options.webshot_denylist_url.strip() == "":
            raise exceptions.OptionsError("webshot_denylist_url must not be empty")

    def requestheaders(self, flow: http.HTTPFlow) -> None:
        self._normalize_local_request(flow)
        self._rewrite_local_upstream(flow)

    def request(self, flow: http.HTTPFlow) -> None:
        self._normalize_local_request(flow)
        self._rewrite_local_upstream(flow)
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
