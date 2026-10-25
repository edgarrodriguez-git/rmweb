"""Tests for the fake captive portal. Standard library only.

Run:  python3 -m unittest discover -s tools/fake-portal -p 'test_*.py' -v

Nothing here follows redirects. TLS cases skip when no OpenSSL 3 binary is found.
"""
import contextlib
import email.utils
import html
import http.client
import io
import json
import os
import random
import re
import shutil
import signal
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import urllib.parse
from unittest import mock

import fake_portal
import certs
from fake_portal import FakePortal

HERE = os.path.dirname(os.path.abspath(__file__))

try:
    certs.find_openssl()
    TLS_SKIP_REASON = None
except certs.OpenSSLUnavailable as exc:
    TLS_SKIP_REASON = str(exc)
needs_tls = unittest.skipIf(TLS_SKIP_REASON is not None, f"TLS tests need OpenSSL 3: {TLS_SKIP_REASON}")

CERT_DIR = ""  # shared by every test so the CA is generated once


def setUpModule():
    global CERT_DIR
    CERT_DIR = tempfile.mkdtemp(prefix="fake-portal-test-")


def tearDownModule():
    shutil.rmtree(CERT_DIR, ignore_errors=True)


def read_bytes(path):
    with open(path, "rb") as f:
        return f.read()


class Reply:
    def __init__(self, resp, body):
        self.status = resp.status
        self.headers = resp.headers
        self.body = body


def fetch(port, path, method="GET", form=None, conn_cls=http.client.HTTPConnection, **conn_kw):
    """One request on a fresh connection; redirects are never followed."""
    conn = conn_cls("127.0.0.1", port, timeout=5, **conn_kw)
    try:
        headers, body = {}, None
        if form is not None:
            body = urllib.parse.urlencode(form)
            headers["Content-Type"] = "application/x-www-form-urlencoded"
        conn.request(method, path, body=body, headers=headers)
        resp = conn.getresponse()
        return Reply(resp, resp.read())
    finally:
        conn.close()


def fetch_tls(port, path, context, method="GET"):
    return fetch(port, path, method=method, conn_cls=http.client.HTTPSConnection, context=context)


def unverified_context():
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    return ctx


def tls_handshake(port, cafile):
    ctx = ssl.create_default_context(cafile=cafile)
    with socket.create_connection(("127.0.0.1", port), timeout=5) as raw:
        with ctx.wrap_socket(raw, server_hostname="127.0.0.1"):
            pass


class PortalCase(unittest.TestCase):
    def start(self, **kw):
        kw.setdefault("cert_dir", CERT_DIR)
        portal = FakePortal(**kw)
        portal.start()
        self.addCleanup(portal.stop)
        return portal

    def g(self, portal, path="/generate_204"):
        return fetch(portal.ports["http"], path)

    def get(self, portal, path):
        return fetch(portal.ports["http"], path)

    def post(self, portal, path, form=None):
        return fetch(portal.ports["http"], path, method="POST", form=form if form is not None else {})


class ProbeTests(PortalCase):
    def test_probe_redirects_until_granted(self):
        p = self.start(variant="plain-form")
        r = self.g(p)
        self.assertEqual(r.status, 302)
        self.assertEqual(r.headers["Location"], "/portal/plain-form/")
        self.assertFalse(p.granted)
        r = self.post(p, "/portal/plain-form/accept", {"name": "Zed", "room": "4711"})
        self.assertEqual(r.status, 200)
        self.assertTrue(p.granted)
        r = self.g(p)
        self.assertEqual(r.status, 204)
        self.assertEqual(r.body, b"")

    def test_apple_probe_success_body(self):
        p = self.start()
        self.assertEqual(self.g(p, "/hotspot-detect.html").status, 302)
        self.post(p, "/portal/plain-form/accept", {"name": "Zed", "room": "4711"})
        r = self.g(p, "/hotspot-detect.html")
        self.assertEqual(r.status, 200)
        self.assertEqual(r.body, b"<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>\n")

    def test_apple_probe_gets_the_same_capture_as_google(self):
        p = self.start(variant="relative-location")
        r = self.g(p, "/hotspot-detect.html")
        self.assertEqual((r.status, r.headers["Location"]), (302, "portal/relative-location/"))

    def test_probe_responses_close_the_connection(self):
        p = self.start()
        for path in ("/generate_204", "/hotspot-detect.html"):
            self.assertEqual(self.g(p, path).headers["Connection"], "close", path)
        p.grant()
        for path in ("/generate_204", "/hotspot-detect.html"):
            self.assertEqual(self.g(p, path).headers["Connection"], "close", path)

    def test_relative_location(self):
        p = self.start(variant="relative-location")
        r = self.g(p)
        self.assertEqual(r.status, 302)
        self.assertEqual(r.headers["Location"], "portal/relative-location/")

    def test_cross_origin_location(self):
        p = self.start(variant="cross-origin")
        r = self.g(p)
        self.assertEqual(r.status, 302)
        self.assertTrue(
            r.headers["Location"].startswith(f"http://127.0.0.1:{p.ports['alt']}/portal/cross-origin/"),
            r.headers["Location"],
        )

    def test_cross_origin_second_origin_serves_the_portal_and_shares_the_grant(self):
        p = self.start(variant="cross-origin")
        alt = p.ports["alt"]
        self.assertEqual(fetch(alt, "/portal/cross-origin/").status, 200)
        self.assertEqual(fetch(alt, "/portal/cross-origin/accept", method="POST", form={}).status, 200)
        self.assertEqual(self.g(p).status, 204)

    def test_511_has_login_link(self):
        p = self.start(variant="status-511")
        r = self.g(p)
        self.assertEqual(r.status, 511)
        self.assertIn(b'href="/portal/status-511/"', r.body)
        self.assertIn(b'<a href="/portal/status-511/">Sign in</a>', r.body)
        self.assertEqual(self.g(p, "/hotspot-detect.html").status, 511)

    def test_403_capture_is_an_html_login_form(self):
        p = self.start(variant="status-403")
        r = self.g(p)
        self.assertEqual(r.status, 403)
        self.assertIn("text/html", r.headers["Content-Type"])
        # served from /generate_204, so the form action cannot be relative
        self.assertIn(b'action="/portal/status-403/accept"', r.body)
        self.assertEqual(self.post(p, "/portal/status-403/accept", {"name": "Zed"}).status, 200)
        self.assertEqual(self.g(p).status, 204)

    def test_unknown_path_is_404(self):
        p = self.start()
        self.assertEqual(self.get(p, "/nope").status, 404)
        self.assertEqual(self.get(p, "/portal/other-variant/").status, 404)

    def test_portal_root_without_slash_redirects(self):
        p = self.start()
        r = self.get(p, "/portal/plain-form")
        self.assertEqual((r.status, r.headers["Location"]), (302, "/portal/plain-form/"))


class SignInFlowTests(PortalCase):
    def test_delayed_grant(self):
        p = self.start(variant="delayed-grant", grant_delay_s=0.5)
        self.assertEqual(self.g(p).status, 302)
        r = self.post(p, "/portal/delayed-grant/accept", {"name": "Zed", "room": "4711"})
        self.assertEqual(r.status, 200)
        self.assertIn("Connecting…".encode(), r.body)
        self.assertEqual(self.g(p).status, 302)
        time.sleep(0.7)
        self.assertEqual(self.g(p).status, 204)

    def test_reset_cancels_a_pending_delayed_grant(self):
        p = self.start(variant="delayed-grant", grant_delay_s=0.3)
        self.post(p, "/portal/delayed-grant/accept", {})
        self.post(p, "/_control/reset")
        time.sleep(0.5)
        self.assertEqual(self.g(p).status, 302)

    def test_meta_refresh_grants_on_second_wait(self):
        p = self.start(variant="meta-refresh")
        r = self.post(p, "/portal/meta-refresh/accept", {"name": "Zed"})
        self.assertEqual((r.status, r.headers["Location"]), (303, "wait"))
        self.assertEqual(self.g(p).status, 302)
        r = self.get(p, "/portal/meta-refresh/wait")
        self.assertEqual(r.status, 200)
        self.assertIn(b'http-equiv="refresh" content="3"', r.body)
        self.assertEqual(self.g(p).status, 302)
        self.get(p, "/portal/meta-refresh/wait")
        self.assertEqual(self.g(p).status, 204)

    def test_window_open_grants_on_get(self):
        p = self.start(variant="window-open")
        index = self.get(p, "/portal/window-open/")
        self.assertIn(b"window.open('accept-get')", index.body)
        self.assertEqual(self.post(p, "/portal/window-open/accept", {}).status, 404)
        self.assertEqual(self.g(p).status, 302)
        self.assertEqual(self.get(p, "/portal/window-open/accept-get").status, 200)
        self.assertEqual(self.g(p).status, 204)

    def test_get_token_grants_only_with_the_configured_token(self):
        p = self.start(variant="get-token", token="T0K3N9")
        index = self.get(p, "/portal/get-token/")
        self.assertIn(b'method="get"', index.body)
        self.assertIn(b'value="T0K3N9"', index.body)
        self.get(p, "/portal/get-token/accept?room=WRONG&name=Smith")
        self.assertEqual(self.g(p).status, 302)
        self.assertEqual(self.get(p, "/portal/get-token/accept?room=T0K3N9&name=Smith").status, 200)
        self.assertEqual(self.g(p).status, 204)

    def test_terms_link_needs_agree_and_links_out(self):
        p = self.start(variant="terms-link")
        self.assertIn(b'name="agree"', self.get(p, "/portal/terms-link/").body)
        terms = self.get(p, "/portal/terms-link/terms")
        self.assertEqual(terms.status, 200)
        text = html.unescape(terms.body.decode())
        for needle in ("history.back()", "/raw?status=200&body=PK&ctype=application/zip",
                       'href="rmweb:settings"', 'href="file:///etc/passwd"'):
            self.assertIn(needle, text)
        self.post(p, "/portal/terms-link/accept", {"name": "Zed"})
        self.assertEqual(self.g(p).status, 302, "no agree=on, no grant")
        self.post(p, "/portal/terms-link/accept", {"agree": "on"})
        self.assertEqual(self.g(p).status, 204)

    def test_documented_failures_grant_only_through_control(self):
        for variant in ("long-select", "non-ascii"):
            with self.subTest(variant=variant):
                p = self.start(variant=variant)
                r = self.post(p, f"/portal/{variant}/accept", {"surname": "Müller", "country": "Country 001"})
                self.assertEqual(r.status, 200)
                self.assertEqual(self.g(p).status, 302)
                self.assertEqual(self.post(p, "/_control/grant").status, 200)
                self.assertEqual(self.g(p).status, 204)

    def test_long_select_has_200_options(self):
        p = self.start(variant="long-select")
        self.assertEqual(self.get(p, "/portal/long-select/").body.count(b"<option"), 200)

    def test_non_ascii_page_is_utf8_and_asks_for_the_surname(self):
        p = self.start(variant="non-ascii")
        r = self.get(p, "/portal/non-ascii/")
        self.assertIn("charset=utf-8", r.headers["Content-Type"].lower())
        self.assertIn("Müller".encode("utf-8"), r.body)

    def test_page_markers_per_variant(self):
        markers = {
            "plain-form": [b'method="post"', b'name="name"', b'name="room"'],
            "room-name": [b'name="last_name"', b'name="room_number"'],
            "js-checkbox": [b'type="checkbox"', b"disabled", b".submit()"],
            "fetch-dom": [b"fetch('accept',{method:'POST'})", b"Connected"],
            "target-blank-post": [b'target="_blank"', b'method="post"'],
            "anchor-hash": [b'href="#"', b"addEventListener('click'", b"fetch('accept'", b"location='done'"],
            "div-role-button": [b'role="button"', b'tabindex="0"', b"addEventListener('click'", b"location='done'"],
            "tls-portal": [b'method="post"'],
        }
        for variant, needles in markers.items():
            with self.subTest(variant=variant):
                p = self.start(variant=variant)
                body = self.get(p, f"/portal/{variant}/").body
                for needle in needles:
                    self.assertIn(needle, body)

    def test_every_variant_captures_and_grants(self):
        expected = {"status-511": 511, "status-403": 403}
        never = {"long-select", "non-ascii", "get-token", "window-open"}
        self.assertEqual(len(fake_portal.VARIANTS), 20)
        for variant in fake_portal.VARIANTS:
            with self.subTest(variant=variant):
                p = self.start(variant=variant)
                r = self.g(p)
                self.assertEqual(r.status, expected.get(variant, 302))
                if variant not in expected:
                    self.assertTrue(r.headers["Location"].endswith(f"portal/{variant}/"), r.headers["Location"])
                self.assertEqual(self.get(p, f"/portal/{variant}/").status, 200)
                if variant not in never:
                    # every POST-based variant answers the form post; grant-by-timer/refresh variants stay captive
                    self.post(p, f"/portal/{variant}/accept", {"agree": "on", "name": "Zed", "room": "4711"})
                self.post(p, "/_control/grant")
                self.assertEqual(self.g(p).status, 204)
                self.assertIn(b"Connected", self.get(p, f"/portal/{variant}/").body)


class RawAndControlTests(PortalCase):
    def test_raw_endpoint(self):
        p = self.start()
        r = self.get(p, "/raw?status=200&body=&len=0")
        self.assertEqual(r.status, 200)
        self.assertEqual(r.headers["Content-Length"], "0")
        self.assertEqual(r.body, b"")
        started = time.monotonic()
        r = self.get(p, "/raw?status=204&delay_s=0.3")
        self.assertGreaterEqual(time.monotonic() - started, 0.3)
        self.assertEqual(r.status, 204)

    def test_raw_body_len_location_and_ctype(self):
        p = self.start()
        self.assertEqual(self.get(p, "/raw?status=200&len=3").body, b"xxx")
        self.assertEqual(self.get(p, "/raw?status=200&body=abcdef&len=2").body, b"ab")
        self.assertEqual(self.get(p, "/raw?body=PK").body, b"PK")
        r = self.get(p, "/raw?status=200&body=PK&ctype=application/zip")
        self.assertEqual(r.headers["Content-Type"], "application/zip")
        r = self.get(p, "/raw?status=302&location=http%3A%2F%2Fexample.test%2Fx")
        self.assertEqual((r.status, r.headers["Location"]), (302, "http://example.test/x"))

    def test_raw_204_has_no_content_length_or_body(self):
        p = self.start()
        r = self.get(p, "/raw?status=204")
        self.assertEqual((r.status, r.body), (204, b""))
        self.assertIsNone(r.headers["Content-Length"])

    def test_client_hanging_up_during_a_delay_is_quiet_and_harmless(self):
        # Task 7's "slow" probe case times out on purpose against /raw?delay_s=7.
        p = self.start()
        noise = io.StringIO()
        with contextlib.redirect_stderr(noise):
            raw = socket.create_connection(("127.0.0.1", p.ports["http"]))
            raw.sendall(b"GET /raw?status=204&delay_s=0.4 HTTP/1.1\r\nHost: x\r\n\r\n")
            raw.close()
            time.sleep(0.8)
        self.assertEqual(noise.getvalue(), "")
        self.assertEqual(self.g(p).status, 302)

    def test_raw_rejects_bad_input(self):
        p = self.start()
        for query in ("status=abc", "status=99", "status=700", "delay_s=-1", "delay_s=3600", "len=99999999999",
                      "location=a%0d%0aX-Evil%3A1"):
            with self.subTest(query=query):
                self.assertEqual(self.get(p, f"/raw?{query}").status, 400)

    def test_control_state_grant_and_reset(self):
        p = self.start(variant="plain-form")
        state = json.loads(self.get(p, "/_control/state").body)
        self.assertEqual(state, {"granted": False, "variant": "plain-form"})
        self.assertEqual(self.get(p, "/_control/grant").status, 405, "grant is POST only")
        self.assertEqual(self.post(p, "/_control/grant").status, 200)
        self.assertEqual(json.loads(self.get(p, "/_control/state").body)["granted"], True)
        self.assertEqual(self.g(p).status, 204)

    def test_reset(self):
        p = self.start()
        self.post(p, "/_control/grant")
        self.assertEqual(self.g(p).status, 204)
        self.assertEqual(self.post(p, "/_control/reset").status, 200)
        self.assertEqual(self.g(p).status, 302)
        self.assertFalse(p.granted)

    def test_reset_restarts_the_meta_refresh_count(self):
        p = self.start(variant="meta-refresh")
        self.get(p, "/portal/meta-refresh/wait")
        self.post(p, "/_control/reset")
        self.get(p, "/portal/meta-refresh/wait")
        self.assertEqual(self.g(p).status, 302)

    def test_head_request_has_headers_and_no_body(self):
        p = self.start()
        r = fetch(p.ports["http"], "/portal/plain-form/", method="HEAD")
        self.assertEqual(r.status, 200)
        self.assertEqual(r.body, b"")
        self.assertGreater(int(r.headers["Content-Length"]), 0)

    def test_date_offset(self):
        p = self.start(date_offset_s=3600)
        r = self.g(p)
        stamp = email.utils.parsedate_to_datetime(r.headers["Date"]).timestamp()
        self.assertAlmostEqual(stamp, time.time() + 3600, delta=5)


class UrlHostTests(unittest.TestCase):
    def test_url_host(self):
        cases = [
            (("127.0.0.1", "ignored"), "127.0.0.1"),
            (("0.0.0.0", "10.11.99.2"), "10.11.99.2"),
            (("::", "::ffff:10.11.99.2"), "10.11.99.2"),
            (("::", "fe80::1%en0"), "[fe80::1]"),
            (("::1", "::1"), "[::1]"),
            (("localhost", "127.0.0.1"), "localhost"),
        ]
        for (bind, local), want in cases:
            with self.subTest(bind=bind, local=local):
                self.assertEqual(fake_portal.url_host(bind, local), want)


class LifecycleTests(PortalCase):
    def test_base_port_layout(self):
        for _ in range(30):
            base = random.randrange(20000, 30000)
            p = FakePortal(base_port=base, cert_dir=CERT_DIR)
            try:
                p.start()
            except OSError:
                p.stop()
                continue
            self.addCleanup(p.stop)
            names = ["http", "alt", "tls_valid", "tls_self", "tls_expired", "tls_notyet"]
            self.assertEqual(p.ports, {name: base + i for i, name in enumerate(names)})
            return
        self.skipTest("no free block of six ports found")

    def test_ephemeral_ports_are_distinct(self):
        p = self.start()
        self.assertEqual(set(p.ports), {"http", "alt", "tls_valid", "tls_self", "tls_expired", "tls_notyet"})
        self.assertEqual(len(set(p.ports.values())), 6)

    def test_stop_releases_the_ports_and_is_idempotent(self):
        p = FakePortal(cert_dir=CERT_DIR)
        p.start()
        port = p.ports["http"]
        p.stop()
        p.stop()
        with self.assertRaises(OSError):
            socket.create_connection(("127.0.0.1", port), timeout=1).close()

    def test_without_cert_dir_a_private_one_is_used_and_removed(self):
        p = FakePortal()
        p.start()
        ca = p.ca_pem
        if TLS_SKIP_REASON is None:
            self.assertTrue(os.path.exists(ca))
        p.stop()
        if ca:
            self.assertFalse(os.path.exists(ca))

    def test_http_still_works_when_openssl_3_is_missing(self):
        with mock.patch.object(certs, "find_openssl", side_effect=certs.OpenSSLUnavailable("no openssl here")):
            p = self.start()
        self.assertIn("no openssl here", p.tls_error)
        self.assertEqual(self.g(p).status, 302)
        with self.assertRaises((ssl.SSLError, OSError)):
            tls_handshake(p.ports["tls_valid"], None)

    def test_bad_arguments(self):
        with self.assertRaises(ValueError):
            FakePortal(variant="no-such-variant")
        with self.assertRaises(ValueError):
            FakePortal(portal_cert="valid")


@needs_tls
class TlsTests(PortalCase):
    def test_tls_listeners(self):
        p = self.start()
        ca = p.ca_pem
        self.assertEqual(os.path.basename(ca), "ca.pem")
        tls_handshake(p.ports["tls_valid"], ca)
        with self.assertRaises(ssl.SSLCertVerificationError) as cm:
            tls_handshake(p.ports["tls_self"], ca)
        self.assertIn("CERTIFICATE_VERIFY_FAILED", str(cm.exception))
        with self.assertRaises(ssl.SSLCertVerificationError) as cm:
            tls_handshake(p.ports["tls_expired"], ca)
        self.assertEqual(cm.exception.verify_code, 10)
        with self.assertRaises(ssl.SSLCertVerificationError) as cm:
            tls_handshake(p.ports["tls_notyet"], ca)
        self.assertEqual(cm.exception.verify_code, 9)

    def test_tls_listeners_serve_confirm_and_portal_pages(self):
        p = self.start()
        trusted = ssl.create_default_context(cafile=p.ca_pem)
        r = fetch_tls(p.ports["tls_valid"], "/confirm", trusted)
        self.assertEqual((r.status, r.body), (204, b""))
        loose = unverified_context()
        for name in ("tls_valid", "tls_self", "tls_expired", "tls_notyet"):
            with self.subTest(port=name):
                self.assertEqual(fetch_tls(p.ports[name], "/confirm", loose).status, 204)
                page = fetch_tls(p.ports[name], "/portal/plain-form/", loose)
                self.assertEqual(page.status, 200)
                self.assertIn(b'action="accept"', page.body)

    def test_grant_over_tls_is_shared_with_http(self):
        p = self.start(variant="tls-portal")
        loose = unverified_context()
        r = fetch_tls(p.ports["tls_self"], "/portal/tls-portal/accept", loose, method="POST")
        self.assertEqual(r.status, 200)
        self.assertEqual(self.g(p).status, 204)

    def test_date_offset_applies_to_tls_too(self):
        p = self.start(date_offset_s=3600)
        r = fetch_tls(p.ports["tls_valid"], "/confirm", ssl.create_default_context(cafile=p.ca_pem))
        stamp = email.utils.parsedate_to_datetime(r.headers["Date"]).timestamp()
        self.assertAlmostEqual(stamp, time.time() + 3600, delta=5)

    def test_tls_portal_points_at_the_self_signed_port(self):
        p = self.start(variant="tls-portal")
        r = self.g(p)
        self.assertEqual(r.status, 302)
        self.assertEqual(r.headers["Location"], f"https://127.0.0.1:{p.ports['tls_self']}/portal/tls-portal/")

    def test_portal_cert_expired(self):
        p = self.start(variant="tls-portal", portal_cert="expired")
        r = self.g(p)
        self.assertEqual(r.status, 302)
        self.assertEqual(r.headers["Location"], f"https://127.0.0.1:{p.ports['tls_expired']}/portal/tls-portal/")

    def test_tls_two_hosts(self):
        p = self.start(variant="tls-two-hosts", san=("127.0.0.1", "10-0-0-2.sslip.io"))
        r = self.g(p)
        self.assertEqual(r.headers["Location"], f"https://127.0.0.1:{p.ports['tls_self']}/portal/tls-two-hosts/")
        link = f'href="https://10-0-0-2.sslip.io:{p.ports["tls_self"]}/portal/tls-two-hosts/b"'
        self.assertIn(link.encode(), self.get(p, "/portal/tls-two-hosts/").body)
        self.assertNotIn(b'<form', self.get(p, "/portal/tls-two-hosts/").body, "host A only links to B")
        b_page = self.get(p, "/portal/tls-two-hosts/b")
        self.assertEqual(b_page.status, 200)
        self.assertIn(b'action="accept"', b_page.body)
        self.assertEqual(self.g(p).status, 302)
        self.post(p, "/portal/tls-two-hosts/accept", {"name": "Zed"})
        self.assertEqual(self.g(p).status, 204)


@needs_tls
class CertTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="fake-portal-certs-")
        self.addCleanup(shutil.rmtree, self.dir, ignore_errors=True)

    def text(self, cert_path):
        out = subprocess.run([certs.find_openssl(), "x509", "-in", cert_path, "-noout", "-text"],
                             check=True, capture_output=True, text=True)
        return out.stdout

    def test_make_certs_returns_ca_and_four_leaves_with_every_san(self):
        san = ["127.0.0.1", "hotel.example", "10-0-0-2.sslip.io", "::1"]
        made = certs.make_certs(self.dir, san)
        self.assertEqual(set(made), {"ca", "valid", "selfsigned", "expired", "notyet"})
        self.assertEqual(os.path.basename(made["ca"][0]), "ca.pem")
        for name, (cert, key) in made.items():
            self.assertTrue(os.path.getsize(cert) > 0 and os.path.getsize(key) > 0, name)
        for name in ("valid", "selfsigned", "expired", "notyet"):
            text = self.text(made[name][0])
            for want in ("IP Address:127.0.0.1", "DNS:hotel.example", "DNS:10-0-0-2.sslip.io",
                         "IP Address:0:0:0:0:0:0:0:1"):
                self.assertIn(want, text, name)

    def test_leaf_validity_windows(self):
        made = certs.make_certs(self.dir, ["127.0.0.1"])
        now = time.time()
        def window(name):
            out = subprocess.run([certs.find_openssl(), "x509", "-in", made[name][0], "-noout", "-startdate", "-enddate"],
                                 check=True, capture_output=True, text=True).stdout
            before, after = (ssl.cert_time_to_seconds(line.split("=", 1)[1]) for line in out.strip().splitlines())
            return before, after
        before, after = window("expired")
        self.assertAlmostEqual(after, now - 86400, delta=120)
        before, after = window("notyet")
        self.assertAlmostEqual(before, now + 365 * 86400, delta=120)
        before, after = window("valid")
        self.assertLess(before, now)
        self.assertGreater(after, now + 86400)

    def test_ca_is_reused_but_leaves_are_reissued(self):
        first = certs.make_certs(self.dir, ["127.0.0.1"])
        ca_before, leaf_before = read_bytes(first["ca"][0]), read_bytes(first["valid"][0])
        second = certs.make_certs(self.dir, ["127.0.0.1", "other.example"])
        self.assertEqual(read_bytes(second["ca"][0]), ca_before)
        self.assertNotEqual(read_bytes(second["valid"][0]), leaf_before)
        self.assertIn("DNS:other.example", self.text(second["valid"][0]))

    def test_valid_leaf_verifies_against_the_ca(self):
        made = certs.make_certs(self.dir, ["127.0.0.1"])
        ok = subprocess.run([certs.find_openssl(), "verify", "-CAfile", made["ca"][0], made["valid"][0]],
                            capture_output=True, text=True)
        self.assertEqual(ok.returncode, 0, ok.stdout + ok.stderr)


class SanValidationTests(unittest.TestCase):
    def test_bad_san_entries_are_rejected_before_openssl_runs(self):
        for bad in ([], [""], ["a b"], ["a,b"], ["DNS:x"], ["x\nsubjectAltName=y"]):
            with self.subTest(san=bad), mock.patch.object(certs, "find_openssl", side_effect=AssertionError("ran")):
                with self.assertRaises(ValueError):
                    certs.make_certs(tempfile.gettempdir(), bad)


class FindOpenSSLTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="fake-openssl-")
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def fake(self, name, banner):
        path = os.path.join(self.tmp, name)
        with open(path, "w") as f:
            f.write(f'#!/bin/sh\necho "{banner}"\n')
        os.chmod(path, 0o755)
        return path

    @staticmethod
    def only_absolute_paths(name):
        return name if os.path.isabs(name) and os.access(name, os.X_OK) else None

    def test_env_var_comes_first(self):
        fake = self.fake("openssl-env", "OpenSSL 3.9.9 1 Jan 2099 (Library: OpenSSL 3.9.9 1 Jan 2099)")
        with mock.patch.dict(os.environ, {"OPENSSL": fake}):
            self.assertEqual(certs.find_openssl(use_cache=False), fake)

    def test_libressl_is_rejected(self):
        fake = self.fake("openssl-libre", "LibreSSL 3.3.6")
        with mock.patch.dict(os.environ, {"OPENSSL": fake}), \
                mock.patch.object(certs.shutil, "which", side_effect=self.only_absolute_paths):
            with self.assertRaises(certs.OpenSSLUnavailable) as cm:
                certs.find_openssl(use_cache=False)
        self.assertIn("LibreSSL", str(cm.exception))

    def test_openssl_2_and_1_are_rejected(self):
        for banner in ("OpenSSL 1.1.1w  11 Sep 2023", "OpenSSL 2.0.0"):
            fake = self.fake("openssl-old", banner)
            with mock.patch.dict(os.environ, {"OPENSSL": fake}), \
                    mock.patch.object(certs.shutil, "which", side_effect=self.only_absolute_paths):
                with self.assertRaises(certs.OpenSSLUnavailable):
                    certs.find_openssl(use_cache=False)

    def test_a_rejected_env_var_falls_through_to_the_next_candidate(self):
        libre = self.fake("openssl-libre", "LibreSSL 3.3.6")
        good = self.fake("openssl-path", "OpenSSL 3.0.13 30 Jan 2024")
        def which(name):
            if name == "openssl":
                return good
            return self.only_absolute_paths(name)
        with mock.patch.dict(os.environ, {"OPENSSL": libre}), mock.patch.object(certs.shutil, "which", side_effect=which):
            self.assertEqual(certs.find_openssl(use_cache=False), good)


class CliTests(unittest.TestCase):
    def test_cli_serves_and_stops_on_sigterm(self):
        cert_dir = tempfile.mkdtemp(prefix="fake-portal-cli-")
        self.addCleanup(shutil.rmtree, cert_dir, ignore_errors=True)
        proc = subprocess.Popen(
            [sys.executable, os.path.join(HERE, "fake_portal.py"), "--bind", "127.0.0.1", "--base-port", "0",
             "--variant", "relative-location", "--cert-dir", cert_dir, "--san", "127.0.0.1", "--san", "localhost",
             "--date-offset-s", "3600"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        watchdog = threading.Timer(60, proc.kill)
        watchdog.start()
        self.addCleanup(watchdog.cancel)
        self.addCleanup(proc.stdout.close)
        self.addCleanup(proc.stderr.close)
        self.addCleanup(lambda: proc.poll() is None and proc.kill())
        ports = {}
        for line in iter(proc.stdout.readline, ""):
            ports.update({k: int(v) for k, v in re.findall(r"\b(\w+)=(\d+)\b", line)})
            if "ready" in line:
                break
        self.assertIn("http", ports, "CLI never printed its ports")
        r = fetch(ports["http"], "/generate_204")
        self.assertEqual((r.status, r.headers["Location"]), (302, "portal/relative-location/"))
        stamp = email.utils.parsedate_to_datetime(r.headers["Date"]).timestamp()
        self.assertAlmostEqual(stamp, time.time() + 3600, delta=5)
        if TLS_SKIP_REASON is None:
            self.assertTrue(os.path.exists(os.path.join(cert_dir, "ca.pem")))
            self.assertIn("tls_valid", ports)
        proc.send_signal(signal.SIGTERM)
        self.assertEqual(proc.wait(timeout=15), 0)

    def test_cli_rejects_an_unknown_variant(self):
        out = subprocess.run([sys.executable, os.path.join(HERE, "fake_portal.py"), "--variant", "nope",
                              "--cert-dir", tempfile.gettempdir()], capture_output=True, text=True, timeout=30)
        self.assertEqual(out.returncode, 2)
        self.assertIn("invalid choice", out.stderr)


if __name__ == "__main__":
    unittest.main()
