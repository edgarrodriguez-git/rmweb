#!/usr/bin/env python3
"""Fake captive portal for the Wi-Fi Sign-in tests (spec section 9). Standard library only.

One FakePortal opens six listeners on consecutive ports (or six free ports when base_port is 0):

    http         probes (/generate_204, /hotspot-detect.html), /raw, /_control/*, /confirm, the portal pages
    alt          a second HTTP origin with the same content (the `cross-origin` variant redirects to it)
    tls_valid    TLS, certificate signed by the test CA (ca.pem in the cert dir)
    tls_self     TLS, self-signed certificate
    tls_expired  TLS, CA-signed certificate that expired a day ago
    tls_notyet   TLS, CA-signed certificate that becomes valid in a year

All listeners serve the same content and share one grant flag. Until the session is granted, the probes
answer with the variant's capture response; afterwards G (/generate_204) answers 204 and A
(/hotspot-detect.html) answers Apple's success page. Every response carries `Connection: close`, and a
`Date` header of now + date_offset_s.

    python3 tools/fake-portal/fake_portal.py --bind 127.0.0.1 --base-port 8080 --variant plain-form --cert-dir DIR
"""
from __future__ import annotations

import argparse
import email.utils
import http.server
import json
import os
import shutil
import signal
import socket
import socketserver
import ssl
import sys
import tempfile
import threading
import time
import traceback
from dataclasses import dataclass, field
from html import escape
from urllib.parse import parse_qs, urlsplit

import certs
import pages

VARIANTS = pages.VARIANTS
PORT_NAMES = ("http", "alt", "tls_valid", "tls_self", "tls_expired", "tls_notyet")
TLS_LEAF = {"tls_valid": "valid", "tls_self": "selfsigned", "tls_expired": "expired", "tls_notyet": "notyet"}
APPLE_SUCCESS = b"<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>\n"
WILDCARD_BINDS = ("", "0.0.0.0", "::")
NEVER_GRANT = ("long-select", "non-ascii")  # documented failures: only /_control/grant opens the network
MAX_RAW_DELAY_S = 60.0
MAX_RAW_LEN = 10_000_000
NO_STORE = [("Cache-Control", "no-store")]


@dataclass
class Request:
    method: str
    path: str
    query: dict
    form: dict
    host: str  # URL-ready host that reaches this machine from the client's side


@dataclass
class Response:
    status: int
    body: bytes = b""
    ctype: str = "text/html; charset=utf-8"
    headers: list = field(default_factory=list)
    delay_s: float = 0.0


def url_host(bind: str, local_addr: str) -> str:
    """Host to put in absolute URLs: the bind address, or for a wildcard bind the address the client reached."""
    host = local_addr if bind in WILDCARD_BINDS else bind
    host = host.split("%", 1)[0]
    if host.startswith("::ffff:"):
        host = host[len("::ffff:"):]
    return f"[{host}]" if ":" in host else host


def _html(body: str, status: int = 200) -> Response:
    return Response(status, body.encode("utf-8"))


def _text(status: int, message: str, headers=()) -> Response:
    return Response(status, (message + "\n").encode(), "text/plain; charset=utf-8", list(headers))


def _redirect(location: str, status: int = 302) -> Response:
    page = f'<!doctype html><title>Redirect</title><a href="{escape(location)}">Continue</a>'
    return Response(status, page.encode(), headers=[("Location", location)])


class FakePortal:
    def __init__(self, bind="127.0.0.1", base_port=0, variant="plain-form", grant_delay_s=8.0, date_offset_s=0,
                 token="TOKEN123", portal_cert="selfsigned", cert_dir=None, san=("127.0.0.1",)):
        if variant not in VARIANTS:
            raise ValueError(f"unknown variant {variant!r}; choose from {', '.join(VARIANTS)}")
        if portal_cert not in ("selfsigned", "expired"):
            raise ValueError("portal_cert must be 'selfsigned' or 'expired'")
        self.bind, self.base_port, self.variant = bind, base_port, variant
        self.grant_delay_s, self.date_offset_s, self.token = grant_delay_s, date_offset_s, token
        self.portal_cert, self.cert_dir, self.san = portal_cert, cert_dir, tuple(san)
        self.ports: dict[str, int] = {}
        self.ca_pem: str | None = None  # set by start() when the certificates exist
        self.tls_error: str | None = None  # why the TLS ports cannot serve, when OpenSSL 3 is missing
        self.log = None  # optional callable(str): one line per request
        self._root = f"/portal/{variant}/"
        self._lock = threading.Lock()
        self._granted = False
        self._wait_hits = 0
        self._timers: list[threading.Timer] = []
        self._servers: dict[str, _Server] = {}
        self._threads: list[threading.Thread] = []
        self._temp_cert_dir: str | None = None

    # ---- lifecycle -------------------------------------------------------------------------------------

    def start(self) -> None:
        if self._servers:
            raise RuntimeError("already started")
        try:
            contexts = self._tls_contexts()
            for i, name in enumerate(PORT_NAMES):
                addr = (self.bind, self.base_port + i if self.base_port else 0)
                if name in TLS_LEAF:
                    server = _TlsServer(addr, self, contexts.get(name))
                else:
                    server = _Server(addr, self)
                self._servers[name] = server
                self.ports[name] = server.server_address[1]
        except BaseException:
            self.stop()
            self.ports = {}
            raise
        for name, server in self._servers.items():
            thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.02},
                                      name=f"fake-portal-{name}", daemon=True)
            thread.start()
            self._threads.append(thread)
            server.running = True

    def stop(self) -> None:
        with self._lock:
            timers, self._timers = self._timers, []
        for timer in timers:
            timer.cancel()
        # shutdown() waits for the serve loop's next poll; do all six at once. It would wait forever on a
        # server that never served, hence the `running` check.
        stoppers = [threading.Thread(target=server.shutdown) for server in self._servers.values() if server.running]
        for stopper in stoppers:
            stopper.start()
        for stopper in stoppers:
            stopper.join()
        for server in self._servers.values():
            server.server_close()
        for thread in self._threads:
            thread.join(timeout=2)
        self._servers, self._threads = {}, []
        if self._temp_cert_dir:
            shutil.rmtree(self._temp_cert_dir, ignore_errors=True)
            self._temp_cert_dir = None

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, *exc):
        self.stop()

    def _tls_contexts(self) -> dict:
        cert_dir = self.cert_dir
        if cert_dir is None:
            self._temp_cert_dir = cert_dir = tempfile.mkdtemp(prefix="fake-portal-certs-")
        try:
            made = certs.make_certs(cert_dir, self.san)
        except certs.OpenSSLUnavailable as exc:
            self.tls_error = str(exc)
            return {}  # the TLS ports still bind, but drop every connection
        self.ca_pem = made["ca"][0]
        contexts = {}
        for port_name, leaf in TLS_LEAF.items():
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            ctx.minimum_version = ssl.TLSVersion.TLSv1_2
            ctx.load_cert_chain(*made[leaf])
            contexts[port_name] = ctx
        return contexts

    # ---- state -----------------------------------------------------------------------------------------

    @property
    def granted(self) -> bool:
        return self._granted

    def grant(self) -> None:
        with self._lock:
            self._granted = True

    def reset(self) -> None:
        with self._lock:
            self._granted, self._wait_hits = False, 0
            timers, self._timers = self._timers, []
        for timer in timers:
            timer.cancel()

    def _grant_later(self) -> None:
        with self._lock:
            self._timers = [t for t in self._timers if t.is_alive()]
            if self._timers:
                return  # one pending grant is enough
            timer = threading.Timer(self.grant_delay_s, self.grant)
            timer.daemon = True
            self._timers.append(timer)
            timer.start()

    # ---- routing ---------------------------------------------------------------------------------------

    def handle(self, req: Request) -> Response:
        path = req.path
        if path == "/generate_204":
            return self._probe("G", req)
        if path == "/hotspot-detect.html":
            return self._probe("A", req)
        if path == "/confirm":
            return Response(204, headers=list(NO_STORE))
        if path == "/raw":
            return self._raw(req)
        if path.startswith("/_control/"):
            return self._control(req)
        if path == self._root.rstrip("/"):
            return _redirect(self._root)
        if path.startswith(self._root):
            return self._portal(req, path[len(self._root):])
        return _text(404, "not found")

    def _probe(self, which: str, req: Request) -> Response:
        if not self.granted:
            resp = self._capture(req)
        elif which == "G":
            resp = Response(204)
        else:
            resp = Response(200, APPLE_SUCCESS)
        resp.headers += NO_STORE
        return resp

    def _capture(self, req: Request) -> Response:
        v = self.variant
        if v == "relative-location":
            return _redirect(f"portal/{v}/")
        if v == "cross-origin":
            return _redirect(f"http://{req.host}:{self.ports['alt']}{self._root}")
        if v == "status-511":
            return _html(pages.status_511(v), 511)
        if v == "status-403":
            return _html(pages.status_403(v), 403)
        if v in ("tls-portal", "tls-two-hosts"):
            port = "tls_expired" if (v == "tls-portal" and self.portal_cert == "expired") else "tls_self"
            return _redirect(f"https://{req.host}:{self.ports[port]}{self._root}")
        return _redirect(self._root)

    def _control(self, req: Request) -> Response:
        action = req.path[len("/_control/"):]
        if action == "state" and req.method in ("GET", "HEAD"):
            pass
        elif action in ("grant", "reset"):
            if req.method != "POST":
                return _text(405, "POST only", [("Allow", "POST")])
            self.grant() if action == "grant" else self.reset()
        else:
            return _text(404, "not found")
        state = json.dumps({"granted": self.granted, "variant": self.variant}, separators=(",", ":"))
        return Response(200, state.encode(), "application/json", list(NO_STORE))

    def _raw(self, req: Request) -> Response:
        """/raw?status=&body=&len=&location=&delay_s=&ctype= : a literal response. `len` pads (with 'x') or cuts
        the body to exactly that many bytes. 1xx/204/304 carry no body."""
        q = {k: v[0] for k, v in req.query.items()}
        try:
            status = int(q.get("status", "200"))
            delay = float(q.get("delay_s") or 0)
            length = int(q["len"]) if q.get("len") else None
        except ValueError:
            return _text(400, "status, len and delay_s must be numbers")
        location, ctype = q.get("location"), q.get("ctype", "text/html; charset=utf-8")
        bad_header = any(c in value for value in (location or "", ctype) for c in "\r\n\0")
        if (not 200 <= status <= 599 or not 0 <= delay <= MAX_RAW_DELAY_S
                or (length is not None and not 0 <= length <= MAX_RAW_LEN) or bad_header):
            return _text(400, "parameter out of range")
        body = q.get("body", "").encode("utf-8")
        if length is not None:
            body = body[:length].ljust(length, b"x")
        headers = [("Location", location)] if location else []
        return Response(status, body, ctype, headers + NO_STORE, delay)

    # ---- the portal pages ------------------------------------------------------------------------------

    def _host_b_url(self) -> str:
        host = self.san[1] if len(self.san) > 1 else self.san[0]
        host = f"[{host}]" if ":" in host else host
        return f"https://{host}:{self.ports['tls_self']}{self._root}b"

    def _login(self, notice: str = "") -> Response:
        return _html(pages.login(self.variant, token=self.token, host_b=self._host_b_url(), notice=notice))

    def _portal(self, req: Request, sub: str) -> Response:
        v = self.variant
        if sub == "accept" and v != "window-open":  # window-open signs in through accept-get
            if v == "get-token":
                return self._accept_token(req)
            if req.method != "POST":
                return _text(405, "POST only", [("Allow", "POST")])
            return self._accept_post(req)
        if sub == "accept-get" and v == "window-open":
            self.grant()
            return _html(pages.connected())
        if sub == "wait" and v == "meta-refresh":
            with self._lock:
                self._wait_hits += 1
                hits = self._wait_hits
            if hits >= 2:  # grant on the second request
                self.grant()
            return _html(pages.connected() if self.granted else pages.waiting())
        if self.granted:
            return _html(pages.connected())
        if sub == "":
            return self._login()
        if sub == "terms" and v == "terms-link":
            return _html(pages.terms(v))
        if sub == "b" and v == "tls-two-hosts":
            return _html(pages.tls_two_hosts_b(v))
        return _text(404, "not found")

    def _accept_post(self, req: Request) -> Response:
        v = self.variant
        if v in NEVER_GRANT:
            return self._login("Documented failure: this page cannot sign you in.")
        if v == "terms-link" and req.form.get("agree", [""])[0] != "on":
            return self._login("Please accept the terms first.")
        if v == "delayed-grant":
            self._grant_later()  # the page itself never navigates again
            return _html(pages.connecting())
        if v == "meta-refresh":
            return _redirect("wait", 303)
        self.grant()
        return _html(pages.connected())

    def _accept_token(self, req: Request) -> Response:
        if req.method not in ("GET", "HEAD"):
            return _text(405, "GET only", [("Allow", "GET")])
        if req.query.get("room", [""])[0] != self.token:
            return self._login("That room was not recognised.")
        self.grant()
        return _html(pages.connected())


# ---- HTTP plumbing -------------------------------------------------------------------------------------


class _Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "fake-portal/1"
    sys_version = ""
    timeout = 30

    def version_string(self):
        return self.server_version

    def date_time_string(self, timestamp=None):
        return email.utils.formatdate(time.time() + self.server.portal.date_offset_s, usegmt=True)

    def log_message(self, fmt, *args):
        log = self.server.portal.log
        if log:
            log(f"{self.client_address[0]} :{self.server.server_port} {fmt % args}")

    def do_GET(self):
        self._serve(True)

    def do_HEAD(self):
        self._serve(False)

    def do_POST(self):
        self._serve(True)

    def _read_form(self) -> dict:
        if self.command != "POST":
            return {}
        try:
            length = max(0, min(int(self.headers.get("Content-Length") or 0), 1_000_000))
        except ValueError:
            length = 0
        raw = self.rfile.read(length) if length else b""
        if self.headers.get("Content-Type", "").startswith("application/x-www-form-urlencoded"):
            return parse_qs(raw.decode("utf-8", "replace"), keep_blank_values=True)
        return {}

    def _serve(self, send_body: bool) -> None:
        url = urlsplit(self.path)
        host = url_host(self.server.portal.bind, self.connection.getsockname()[0])
        req = Request(self.command, url.path, parse_qs(url.query, keep_blank_values=True), self._read_form(), host)
        try:
            resp = self.server.portal.handle(req)
        except Exception:  # a bug here should read as a 500 in the test, not as a dropped connection
            traceback.print_exc()
            resp = _text(500, "fake portal internal error")
        if resp.delay_s:
            time.sleep(resp.delay_s)
        bodiless = resp.status in (204, 304)
        self.send_response(resp.status)
        self.send_header("Connection", "close")
        self.close_connection = True
        if not bodiless:
            self.send_header("Content-Type", resp.ctype)
            self.send_header("Content-Length", str(len(resp.body)))
        for name, value in resp.headers:
            self.send_header(name, value)
        self.end_headers()
        if send_body and not bodiless:
            self.wfile.write(resp.body)


class _Server(http.server.ThreadingHTTPServer):
    request_queue_size = 64
    running = False

    def __init__(self, addr, portal):
        if ":" in addr[0]:
            self.address_family = socket.AF_INET6
        self.portal = portal
        super().__init__(addr, _Handler)

    def server_bind(self):
        socketserver.TCPServer.server_bind(self)  # HTTPServer's own version does a slow reverse-DNS lookup
        self.server_name, self.server_port = self.server_address[0], self.server_address[1]

    def handle_error(self, request, client_address):
        if isinstance(sys.exc_info()[1], (ConnectionError, TimeoutError, ssl.SSLError)):
            return  # clients hanging up or rejecting our certificate is routine here
        super().handle_error(request, client_address)


class _TlsServer(_Server):
    """TLS is negotiated in the per-connection thread, so a stalled handshake blocks only itself.
    With no context (OpenSSL 3 missing) the port exists but drops every connection."""

    def __init__(self, addr, portal, context):
        self.context = context
        super().__init__(addr, portal)

    def finish_request(self, request, client_address):
        if self.context is None:
            return
        request.settimeout(10)
        try:
            tls = self.context.wrap_socket(request, server_side=True)
        except (ssl.SSLError, OSError):
            return
        try:
            super().finish_request(tls, client_address)
        finally:
            tls.close()


# ---- command line --------------------------------------------------------------------------------------


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Fake captive portal for rmweb Wi-Fi Sign-in tests.")
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--base-port", type=int, default=8080,
                    help="six listeners on N..N+5: http, alt, tls_valid, tls_self, tls_expired, tls_notyet (0 = any free)")
    ap.add_argument("--variant", default="plain-form", choices=VARIANTS)
    ap.add_argument("--grant-delay-s", type=float, default=8.0)
    ap.add_argument("--date-offset-s", type=float, default=0, help="added to the Date header of every response")
    ap.add_argument("--token", default="TOKEN123", help="room value the get-token variant accepts")
    ap.add_argument("--portal-cert", choices=("selfsigned", "expired"), default="selfsigned",
                    help="certificate of the tls-portal page")
    ap.add_argument("--cert-dir", required=True, help="receives ca.pem and the leaf certificates")
    ap.add_argument("--san", nargs="+", action="extend", metavar="NAME",
                    help="name or IP in every certificate; repeat or list several (default 127.0.0.1)")
    ap.add_argument("--quiet", action="store_true", help="no per-request log on stderr")
    args = ap.parse_args(argv)

    portal = FakePortal(args.bind, args.base_port, args.variant, args.grant_delay_s, args.date_offset_s, args.token,
                        args.portal_cert, args.cert_dir, tuple(args.san or ("127.0.0.1",)))
    if not args.quiet:
        portal.log = lambda line: print(line, file=sys.stderr, flush=True)
    stop = threading.Event()
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, lambda *_: stop.set())
    try:
        portal.start()
    except (OSError, certs.CertError) as exc:
        print(f"fake-portal: cannot start: {exc}", file=sys.stderr)
        return 1
    print(f"fake-portal: variant={args.variant} bind={args.bind}", flush=True)
    print("ports: " + " ".join(f"{name}={port}" for name, port in portal.ports.items()), flush=True)
    if portal.ca_pem:
        print(f"ca: {os.path.abspath(portal.ca_pem)}", flush=True)
    if portal.tls_error:
        print(f"fake-portal: WARNING, TLS ports are dead: {portal.tls_error}", file=sys.stderr, flush=True)
    print("fake-portal: ready", flush=True)
    try:
        stop.wait()
    finally:
        portal.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
