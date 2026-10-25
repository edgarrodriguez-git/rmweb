# Fake captive portal

A local test server that imitates a hotel or cafe Wi-Fi network's sign-in page, for the Wi-Fi Sign-in
(captive mode) work. Python 3 standard library only, plus an OpenSSL 3 command line for the TLS listeners.
Design: `docs/wifi-signin/design.md` section 9.

Used by:

- the C++ probe-runner integration test on the Mac (`tests/probe_runner_test.sh`);
- the on-device checks over USB (`tools/device-checks/`).

## Run it

```bash
python3 tools/fake-portal/fake_portal.py --bind 127.0.0.1 --base-port 8080 --variant plain-form --cert-dir build/certs
```

| Option | Default | Meaning |
|---|---|---|
| `--bind IP` | `127.0.0.1` | Address to listen on. Use `0.0.0.0` for device runs. |
| `--base-port N` | `8080` | Six listeners on N to N+5. `0` picks six free ports (read them from the `ports:` line). |
| `--variant NAME` | `plain-form` | One of the variants below. |
| `--grant-delay-s S` | `8` | `delayed-grant`: seconds between the form post and the grant. |
| `--date-offset-s S` | `0` | Added to the `Date` header of every response on every listener (clock-skew tests). |
| `--token TOKEN` | `TOKEN123` | `get-token`: the `room` value that grants. |
| `--portal-cert selfsigned\|expired` | `selfsigned` | `tls-portal`: which TLS listener the capture redirect points at. |
| `--cert-dir DIR` | required | Receives `ca.pem` (the CA to trust) plus the keys and certificates. |
| `--san NAME [NAME ...]` | `127.0.0.1` | Names and IPs put into every certificate. Repeat the option or list several. Addresses become `IP:`, the rest `DNS:`. |
| `--quiet` | off | No per-request log on stderr. |

It prints its ports, the CA path and `fake-portal: ready` on stdout, logs one line per request on stderr, and
stops cleanly on SIGINT or SIGTERM.

Device run (the tablet reaches the Mac over USB or Wi-Fi):

```bash
python3 tools/fake-portal/fake_portal.py --bind 0.0.0.0 --base-port 8080 --variant <v> --cert-dir build/certs \
    --san <mac-ip> --san <mac-ip-with-dashes>.sslip.io
```

With a wildcard bind, absolute URLs in redirects use the address the client connected to.

## Listeners

| Port | Name in `FakePortal.ports` | What |
|---|---|---|
| N | `http` | HTTP |
| N+1 | `alt` | second HTTP origin (the `cross-origin` variant redirects here) |
| N+2 | `tls_valid` | TLS, signed by the test CA |
| N+3 | `tls_self` | TLS, self-signed |
| N+4 | `tls_expired` | TLS, CA-signed, expired one day ago |
| N+5 | `tls_notyet` | TLS, CA-signed, valid from one year ahead |

All six serve the same content and share one grant flag.

## HTTP endpoints

- `/generate_204` (G): 204 with an empty body once granted, otherwise the variant's capture response (by
  default 302 to `/portal/<variant>/`).
- `/hotspot-detect.html` (A): once granted, 200 with Apple's `<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>`
  plus `\n`; otherwise the same capture response as G.
- `/confirm`: 204 (the HTTPS confirmation endpoint; point it at a TLS port).
- `/portal/<variant>/...`: the variant's pages. Form actions are relative to `/portal/<variant>/`. Once
  granted, every page shows "Connected".
- `/raw?status=&body=&len=&location=&delay_s=&ctype=`: a literal response. `status` 200-599 (default 200),
  `body` text, `len` pads the body with `x` or cuts it to exactly that many bytes (`body=&len=0` is an empty
  200 with `Content-Length: 0`), `location` adds a header, `delay_s` waits first (max 60), `ctype` sets the
  type (default `text/html; charset=utf-8`). 204 and 304 carry no body. Anything out of range gives 400.
- `/_control/grant`, `/_control/reset` (POST), `/_control/state` (GET, `{"granted":bool,"variant":str}`).
  `reset` also forgets pending timers and the `meta-refresh` count.

Every response carries `Connection: close`; probe responses and `/raw` also carry `Cache-Control: no-store`.
The control endpoints and `/raw` have no authentication, so keep `--bind` on loopback unless a device needs
it.

## Variants

| Variant | Capture response | How it grants |
|---|---|---|
| `plain-form` | 302 to the portal | POST `accept` (fields `name`, `room`; any POST grants) |
| `room-name` | 302 | POST `accept` (`last_name`, `room_number`) |
| `js-checkbox` | 302 | the checkbox enables a button whose JS calls `form.submit()` |
| `fetch-dom` | 302 | `fetch('accept',{method:'POST'})`, then only the DOM text changes to `Connected` |
| `delayed-grant` | 302 | POST shows `Connecting…`; the grant happens `--grant-delay-s` later, with no navigation |
| `meta-refresh` | 302 | POST redirects (303) to `wait`, which refreshes itself every 3 s; the second `wait` request grants |
| `cross-origin` | 302 to `http://<host>:<alt>/portal/cross-origin/` | POST on the second origin |
| `relative-location` | 302 with `Location: portal/relative-location/` | POST |
| `status-511` | 511 with `<a href="/portal/status-511/">Sign in</a>` | POST |
| `status-403` | 403 with an HTML login form (action `/portal/status-403/accept`) | POST |
| `target-blank-post` | 302 | `<form target="_blank" method="post">` |
| `window-open` | 302 | a button runs `window.open('accept-get')`; GET `accept-get` grants |
| `anchor-hash` | 302 | `<a href="#">` with a click listener that fetches `accept`, then sets `location='done'` |
| `div-role-button` | 302 | `<div role="button" tabindex="0">`, same listener |
| `terms-link` | 302 | the form needs `agree=on`; the `terms` page goes back with `history.back()` and links to `/raw?status=200&body=PK&ctype=application/zip`, `rmweb:settings` and `file:///etc/passwd` |
| `tls-portal` | 302 to `https://<host>:<tls_self>/portal/tls-portal/` (`tls_expired` with `--portal-cert expired`) | POST |
| `tls-two-hosts` | 302 to the self-signed host A | host A only links to host B, `https://<san[1]>:<tls_self>/portal/tls-two-hosts/b` (`san[0]` when there is one `--san`); the form is on B |
| `get-token` | 302 | GET `accept?room=<token>&name=Smith`, only when `room` equals `--token` (the form is pre-filled) |
| `long-select` | 302 | documented failure: 200 options; grants only through `/_control/grant` |
| `non-ascii` | 302 | documented failure: surname `Müller`; grants only through `/_control/grant` |

`<host>` is the bind address, or the address the client connected to when binding `0.0.0.0` or `::`.

## Certificates

`certs.make_certs(cert_dir, san)` writes `ca.pem`/`ca.key` and `valid`, `selfsigned`, `expired` and `notyet`
leaves (`<name>.pem`/`<name>.key`), all EC P-256 and all carrying every SAN entry.

- OpenSSL is resolved from `$OPENSSL`, then `$(brew --prefix openssl@3)/bin/openssl`, then `openssl` on PATH.
  Only a binary whose `version` starts with `OpenSSL 3.` is accepted, so macOS's LibreSSL is rejected.
- Only OpenSSL 3.0 features are used (the expired and not-yet-valid leaves come from `openssl ca
  -startdate/-enddate`), so the same code runs on the Mac (3.5) and on GitHub's ubuntu-latest (3.0).
- The CA is kept while it has more than 30 days left, so `ca.pem` stays the same across restarts and a copy
  already pushed to the tablet keeps working. The leaves are reissued on every start, which keeps the
  "expired" and "not yet valid" dates relative to now.
- Run one portal per `--cert-dir` at a time: two starting together overwrite each other's key files.
- Without OpenSSL 3, `FakePortal.start()` still serves HTTP; the TLS ports exist but drop every connection,
  and `FakePortal.tls_error` says why (the CLI prints a warning).

## From Python

```python
from fake_portal import FakePortal

with FakePortal(variant="plain-form", base_port=0, cert_dir="build/certs") as portal:
    print(portal.ports)     # {'http': ..., 'alt': ..., 'tls_valid': ..., ...}
    print(portal.ca_pem)    # path of the CA, None without OpenSSL 3
    portal.grant()          # or POST /_control/grant; portal.granted tells the state
```

## Tests

```bash
python3 -m unittest discover -s tools/fake-portal -p 'test_*.py' -v
```

Part of `bash scripts/run-tests.sh`, which prints the skip count. TLS cases skip, with the reason, when no OpenSSL 3
binary is found. With `WIFI_SIGNIN_REQUIRE_TLS=1`, which `run-tests.sh` exports when `CI` or `GITHUB_ACTIONS` is set,
the module fails instead, so a CI run cannot pass without the TLS cases.
The unit tests cannot run the JavaScript in the pages; check those by loading a variant in a browser.
