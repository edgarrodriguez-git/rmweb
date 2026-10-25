# Wi-Fi Sign-in Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build "Wi-Fi Sign-in", an AppLoad app that lets a reMarkable Paper Pro, Paper Pro Move or Paper Pure owner complete a captive-portal sign-in page on the device. The app is built from a fork of rmweb and packaged for Vellum and reManager.

**Architecture:** The app is a captive mode inside a fork of exp78/rmweb v0.9.7. Pure-logic modules are header-only C++ under `engine/wpeqt/captive/`, unit-tested on the Mac with rmweb's existing `scripts/run-tests.sh` pattern. They cover probe classification, re-probe scheduling, the controller state machine, config, log redaction and exit codes. A libsoup probe runner is integration-tested on the Mac against a Python fake portal. A POSIX-sh takeover launcher runs as a transient systemd service and is tested on the Mac with stubbed commands and a fake `/proc` and `/sys/power`. Small, flag-guarded hooks in `main.cpp` wire it all into rmweb. Two Vellum packages ship the result: `rmweb-runtime`, a repackaged upstream engine, and `wifi-signin`, built from source.

**Tech Stack:**
- C++17, Qt 6 Quick, WPE WebKit 2.48.5 and libsoup 3.6 from the rmweb 0.9.7 runtime;
- the official reMarkable SDK 3.28.0.172 (image 5.8.203, ferrari, aarch64 host) in Docker on Colima;
- POSIX sh, Python 3 (stdlib only), OpenSSL 3 (test certificates);
- Vellum, with vbuild 0.0.36 run in GitHub Actions.

**Spec:** `/Volumes/2TB OWC 1M2/Developer/Remarkable/docs/superpowers/specs/2026-10-23-wifi-signin-design.md`, revision 2, approved 2026-10-25. It is copied into the repository as `docs/wifi-signin/design.md` in Task 1. Section numbers below (for example "spec §5") refer to it.

## Global Constraints

**Platform.**
- Target OS: `remarkable-os>=3.28 remarkable-os<3.30`. Architecture aarch64 only.
- Device order: Paper Pro first. The Move is enabled in Task 19 and the Pure in Task 20.
- Fork: `edgarrodriguez-git/rmweb`, branch `wifi-signin` from tag `v0.9.7`. Local clone: `/Volumes/2TB OWC 1M2/Developer/Remarkable/rmweb`. Every path in this plan is relative to that clone unless it is absolute.
- Upstream code must keep working with `RMWEB_KIOSK` unset. Every `main.cpp` change is guarded by `g_captive`.

**Installed and runtime paths.**

| What | Path |
|---|---|
| App (`$APP`) | `/home/root/xovi/exthome/appload/wifi-signin` |
| Runtime (`$R`) | `/home/root/.vellum/share/rmweb-runtime` |
| Runtime state (tmpfs) | `/run/wifi-signin` |
| Persistent user data | `/home/root/.wifi-signin` (`log`, `log.1`, optional `settings.conf`) |
| EPD lock | `/tmp/epframebuffer.lock` |
| Panel | `/dev/dri/card0` |
| Systemd units used | `wifi-signin` (transient), `xochitl`, `xovi-tripletap` |

**Probe defaults.**

| Endpoint | URL |
|---|---|
| HTTP probe G | `http://connectivitycheck.gstatic.com/generate_204` |
| HTTP probe A | `http://captive.apple.com/hotspot-detect.html` |
| HTTPS confirmation | `https://www.google.com/generate_204` |

The Apple success body is exactly `<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>`, compared after trimming trailing whitespace.

**Timings.**

| Item | Value |
|---|---|
| Probe timeout | 5 s |
| Minimum gap between probe cycles | 3 s |
| Re-probe after a form submit, keyboard Go or control tap | 5 s |
| Idle re-probe | every 10 s |
| Idle timeout | 600 s (exit 22) |
| "Connected" screen | 2 s |
| Start-up clock-wrong screen | 3 s |
| "This network is already open" | 2 s |
| No-network auto-close | 20 s |
| xochitl age guard | 30 s |
| Panel wait after stopping xochitl | up to 5 s |
| Settle before launch | 1.5 s |
| Settle before restarting xochitl | 2 s |
| Settle before a retry | 3 s |
| Ready-file timeout | 30 s |
| Retry window | 20 s |
| Session wakelock | `wifi-signin 900000000000`, rewritten every 60 s |
| Final timed wakelock | `wifi-signin 30000000000` |
| App TERM wait | 3 s |
| Helper wait | 10 s |
| Log cap | 262144 bytes, one rotation |
| Clock-skew threshold | 300 s |

**Exit codes.**
- Captive mode: 20 online, 21 cancelled (button or power key), 22 idle timeout, 23 no network or limited and closed, 24 use network as is.
- Inherited from rmweb: 0 SIGTERM drain, 1 touch grab failed, 2 and 3 start-up failure, 63 GUI watchdog, 128+n killed by signal n.
- Launcher: 11 already running, 12 display busy, 13 systemd-run missing.

**UI copy (exact).**
- Bar title: `Wi-Fi Sign-in`.
- Screens:
  - `This network is already open`
  - `Connected. The tablet clock looks wrong.`
  - `Not connected to Wi-Fi. Join the network in Settings first.`
  - `This network is not responding to sign-in checks`
  - `Web pages load but secure connections fail on this network`
  - `Connected`
- Status line: `Secure connections still failing`, `Tablet clock looks wrong`.
- Badge: `Test probe`.
- Buttons: `Back`, `Reload`, `Use network as is`, `Cancel`, `Retry`, `Close`.
- Certificate-page hint: `The tablet clock may be wrong.`
- Package messages: `Refresh AppLoad to see Wi-Fi Sign-in`, `Closing Wi-Fi Sign-in first`.

**App behaviour rules.**
- Every captive-mode exit is `WpeView::drainForExit()` followed by `std::_Exit(code)` on the GUI thread. The app never calls `QCoreApplication::exit()` or `quit()` and never returns from `app.exec()`.
- In captive mode nothing is written under `/home` except `/home/root/.wifi-signin/log*`.

**Repository rules.**
- Host tests run with `bash scripts/run-tests.sh`. New C++ tests are `tests/captive_*_test.cpp` and include headers by relative path; the runner compiles each one alone with `clang++ -std=c++17`. New shell tests are `tests/*_test.sh`.
- Commits go to the fork only. Every commit message ends with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` unless the owner says otherwise.
- Agents never open issues or PRs in, or push to, `vellum-dev/vellum`. Its rules require human-authored PRs and commits, so Task 21 is owner-only.
- Code ported from `jhasubhash/rmbrowser` (MIT) is credited in `NOTICE`.

**Device rules.**
- Device tasks need the tablet on 3.28.x with Developer Mode, connected over USB (`ssh root@10.11.99.1`), and the owner present.
- Run nothing on the device that this plan does not name.

## Review Focus

1. **Slow portals.** A portal or DNS server slower than 5 s must not hang a probe cycle; the cycle must finish within about 5.5 s as no network. Pinned in Task 7 by the `slow` case.
2. **Location header forms.** Relative, scheme-relative (`//host/p`), query-only (`?x=1`), fragment (`#f`) and dot-segment (`../a`) Location values must resolve correctly. Pinned in Task 3 by `resolveUrl` checks.
3. **Self-refreshing pages.** A portal page that refreshes itself every 500 ms must not flood probes: at most one cycle per 3 s. Pinned in Task 8 by `refresh_storm_is_rate_limited`.
4. **Wi-Fi dropping mid-session.** No network while the portal is shown must neither exit nor navigate; the page stays and probing continues. Pinned in Task 8 by `no_network_while_shown_keeps_page`.
5. **Power key during start-up screens.** A press while a start-up screen (no network, limited, partial) is shown must exit 21 cleanly. Pinned in Task 14 by the `power_on_start_screen` check.

---

## Part A: Host foundations (no device needed)

### Task 1: Fork, clone, host tools, baseline

**Files:**
- Create: `docs/wifi-signin/design.md` (copy of the spec), `docs/wifi-signin/plan.md` (copy of this plan)
- Modify: `NOTICE` (add a "Wi-Fi Sign-in" section: fork of rmweb 0.9.7 (MIT); ported pieces from jhasubhash/rmbrowser (MIT))

- [ ] **Step 1: Owner creates the fork.** With the owner's go-ahead, run `gh repo fork exp78/rmweb --clone=false`. Expected: `edgarrodriguez-git/rmweb` exists.
- [ ] **Step 2: Clone and branch.**

```bash
git clone https://github.com/edgarrodriguez-git/rmweb.git "/Volumes/2TB OWC 1M2/Developer/Remarkable/rmweb"
cd "/Volumes/2TB OWC 1M2/Developer/Remarkable/rmweb"
git remote add upstream https://github.com/exp78/rmweb.git && git fetch upstream --tags
git switch -c wifi-signin v0.9.7
```

- [ ] **Step 3: Install host tools.** Run `brew install pkgconf libsoup openssl@3 shellcheck ninja librsvg colima docker docker-buildx`. Then check: `pkg-config --modversion libsoup-3.0` prints 3.x, and `$(brew --prefix openssl@3)/bin/openssl version` prints `OpenSSL 3.`.
- [ ] **Step 4: Baseline the host tests.** Run `bash scripts/run-tests.sh`. Expected last line: `ALL HOST TESTS OK`.
- [ ] **Step 5: Add docs and NOTICE, then commit.** Commit message: `docs: wifi-signin design and plan`.

### Task 2: Fake portal

**Files:**
- Create: `tools/fake-portal/fake_portal.py`, `tools/fake-portal/certs.py`, `tools/fake-portal/test_fake_portal.py`, `tools/fake-portal/README.md`
- Modify: `scripts/run-tests.sh` (run `python3 -m unittest discover -s tools/fake-portal -p 'test_*.py'`; a failure sets `fail=1`), `.github/workflows/tests.yml` (apt install adds `python3 openssl`)

**Interfaces:**
- Produces a CLI: `python3 tools/fake-portal/fake_portal.py --bind IP --base-port N --variant NAME [--grant-delay-s 8] [--date-offset-s 0] [--token TOKEN] [--portal-cert selfsigned|expired] --cert-dir DIR [--san NAME ...]`. A base port of N opens six listeners:

  | Port | Listener |
  |---|---|
  | N | HTTP |
  | N+1 | second HTTP origin |
  | N+2 | TLS signed by the test CA |
  | N+3 | self-signed TLS |
  | N+4 | expired TLS |
  | N+5 | not-yet-valid TLS |

  `--cert-dir` receives `ca.pem`.
- Produces a Python class: `FakePortal(bind="127.0.0.1", base_port=0, variant="plain-form", grant_delay_s=8.0, date_offset_s=0, token="TOKEN123", portal_cert="selfsigned", cert_dir=None, san=("127.0.0.1",))` with `.start()`, `.stop()`, `.ports` (dict with keys `http alt tls_valid tls_self tls_expired tls_notyet`) and `.granted`. A base port of 0 picks free ports.
- HTTP endpoints:
  - `/generate_204` (G): 204 with an empty body when granted. Otherwise the variant's capture response: by default 302 with `Location: /portal/<variant>/`.
  - `/hotspot-detect.html` (A): when granted, 200 with the Apple body plus `\n`. Otherwise the same capture response as G.
  - `/portal/<variant>/...`.
  - `/raw?status=&body=&len=&location=&delay_s=&ctype=`: a literal response for the probe and download tests.
  - `/_control/grant`, `/_control/reset` (POST) and `/_control/state` (GET JSON `{"granted":bool,"variant":str}`).
- TLS endpoints: every TLS port serves `/confirm` (204) and the portal pages. The `Date` header is the current time plus `--date-offset-s`.
- `certs.py`: `make_certs(cert_dir, san) -> dict[str, tuple[cert_path, key_path]]` using `$(brew --prefix openssl@3)/bin/openssl`, or `OPENSSL` from the environment. It produces a CA plus `valid`, `selfsigned`, `expired` (not_after one day ago) and `notyet` (not_before one year ahead) leaves. Every leaf carries every `san` entry.

**Variants.** These are the spec §9 fixtures; the names are fixed.

| Variant | Capture response | How it grants |
|---|---|---|
| `plain-form` | 302 to the portal | POST `accept` with `name` and `room` |
| `room-name` | 302 | POST with `last_name` and `room_number` |
| `js-checkbox` | 302 | the checkbox enables a button whose JS calls `form.submit()` |
| `fetch-dom` | 302 | `fetch('accept',{method:'POST'})`, then only the DOM text changes to `Connected` |
| `delayed-grant` | 302 | POST shows `Connecting…`; the grant happens `grant_delay_s` later, with no navigation |
| `meta-refresh` | 302 | POST leads to `wait`, which self-refreshes every 3 s; the grant happens on the second `wait` request |
| `cross-origin` | 302 to `http://<bind>:<alt>/portal/cross-origin/` | POST |
| `relative-location` | 302 with `Location: portal/relative-location/` | POST |
| `status-511` | 511 with HTML containing `<a href="/portal/status-511/">Sign in</a>` | POST |
| `status-403` | 403 with an HTML login form | POST |
| `target-blank-post` | 302 | `<form target="_blank" method="post">` |
| `window-open` | 302 | a button runs `window.open('accept-get')` |
| `anchor-hash` | 302 | `<a href="#">` plus an `addEventListener('click')` that fetches `accept` and then sets `location='done'` |
| `div-role-button` | 302 | `<div role="button" tabindex="0">` with a click listener, as in `anchor-hash` |
| `terms-link` | 302 | the form needs `agree=on`; a link goes to `terms` and back with `history.back()`. The `terms` page also links to `/raw?status=200&body=PK&ctype=application/zip`, `rmweb:settings` and `file:///etc/passwd`, for Task 15's checks |
| `tls-portal` | 302 to `https://<bind>:<tls_self>/portal/tls-portal/`, or to `tls_expired` with `--portal-cert expired` | POST |
| `tls-two-hosts` | 302 to the self-signed host A | the page links to host B, `https://<san[1]>:<tls_self>/portal/tls-two-hosts/b`; for device runs, `san[1]` is the Mac IP with dashes plus `.sslip.io` |
| `get-token` | 302 | GET `accept?room=<token>&name=Smith` |
| `long-select` | 302 | documented failure: 200 options; grants only through `/_control/grant` |
| `non-ascii` | 302 | documented failure: surname `Müller`; grants only through `/_control/grant` |

- [ ] **Step 1: Write the failing tests** in `tools/fake-portal/test_fake_portal.py` (unittest; no redirect following; TLS cases are skipped when OpenSSL 3 is missing):
  - `test_probe_redirects_until_granted`: G returns 302 with Location `/portal/plain-form/`. A POST to `accept` returns 200. Then G returns 204 with body `b""`.
  - `test_apple_probe_success_body`: after a grant, A returns 200 with the body `b'<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>\n'`.
  - `test_relative_location`: Location is `portal/relative-location/`.
  - `test_cross_origin_location`: Location starts with `f"http://127.0.0.1:{ports['alt']}/portal/cross-origin/"`.
  - `test_511_has_login_link`: status 511, and `b'href="/portal/status-511/"'` is in the body.
  - `test_delayed_grant`: with `grant_delay_s=0.5`, G returns 302 right after the POST and 204 0.7 s later.
  - `test_meta_refresh_grants_on_second_wait`: G returns 302 after the first `wait` and 204 after the second.
  - `test_raw_endpoint`: `/raw?status=200&body=&len=0` gives 200 with `Content-Length: 0`; `delay_s=0.3` delays by at least 0.3 s.
  - `test_date_offset`: with `date_offset_s=3600`, the Date header parses within ±5 s of now + 3600.
  - `test_tls_listeners`: the test CA verifies `tls_valid`; `tls_self` fails with `CERTIFICATE_VERIFY_FAILED`; `tls_expired` has `verify_code == 10`; `tls_notyet` has `verify_code == 9`.
  - `test_reset`: after `/_control/reset`, G returns 302 again.
  - `test_portal_cert_expired`: with `variant="tls-portal", portal_cert="expired"`, G's Location points at the `tls_expired` port.
- [ ] **Step 2: Run them and confirm they fail.** Run `python3 -m unittest discover -s tools/fake-portal -p 'test_*.py' -v`. Expected: errors with `ModuleNotFoundError: fake_portal`.
- [ ] **Step 3: Implement `fake_portal.py` and `certs.py`.** Use `http.server.ThreadingHTTPServer` and `ssl.SSLContext` on one thread per listener. Grant state lives on the `FakePortal` instance and is shared by all listeners. Send a `Connection: close` header on probe responses.
- [ ] **Step 4: Run them and confirm they pass.** Same command. Expected: `OK`, with TLS tests skipped only if OpenSSL 3 is absent.
- [ ] **Step 5: Run the full host suite and commit.** `bash scripts/run-tests.sh` ends with `ALL HOST TESTS OK`. Commit message: `test: fake captive portal for host and device tests`.

### Task 3: Probe classification

**Files:**
- Create: `engine/wpeqt/captive/probe_classify.h`, `tests/captive_probe_classify_test.cpp`

**Interfaces (Produces, namespace `rmweb::captive`):**

```cpp
inline constexpr const char* kAppleSuccessBody = "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>";
enum class ProbeKind { Google204, AppleSuccess };
enum class ProbeClass { ProbeOk, Limited, Captive, NoNetwork };
struct HttpReply { bool transportOk = false; int status = 0; std::string body; long long contentLength = -1; std::string location; };
struct ProbeVerdict { ProbeClass cls = ProbeClass::NoNetwork; std::string probeUrl; std::string location; }; // location: resolved, "" if none
std::string resolveUrl(const std::string& base, const std::string& ref);   // RFC 3986 §5.2 reference resolution
ProbeVerdict classifyProbe(ProbeKind kind, const std::string& probeUrl, const HttpReply& r);
enum TlsFlag : unsigned { kTlsUnknownCa = 1u<<0, kTlsBadIdentity = 1u<<1, kTlsNotActivated = 1u<<2, kTlsExpired = 1u<<3,
                          kTlsRevoked = 1u<<4, kTlsInsecure = 1u<<5, kTlsGenericError = 1u<<6 };  // == GTlsCertificateFlags
enum class TlsConfirm { Ok204, ValidityOnly, OtherFailure };
TlsConfirm tlsConfirmFrom(bool transportOk, unsigned tlsFlags, int status);
enum class CycleResult { Online, OnlineClockWrong, Captive, Partial, Limited, NoNetwork };
const char* cycleResultName(CycleResult r);                                 // "Online", "OnlineClockWrong", ...
struct CycleOutcome { CycleResult result = CycleResult::NoNetwork; std::string viewUrl; std::string locationUrl; };
CycleOutcome combineCycle(bool hasDefaultRoute, const ProbeVerdict& g, const ProbeVerdict& a, TlsConfirm confirm);
struct CycleReport { CycleOutcome outcome; std::string gAddress, aAddress; bool dateKnown = false; long long skewSec = 0; };
bool hasDefaultRoute(const std::string& procNetRoute, const std::string& iface);  // /proc/net/route text
long long parseHttpDate(const std::string& imfFixdate);                     // epoch seconds, -1 if invalid
bool clockSkewed(long long serverEpoch, long long deviceEpoch);             // |diff| > 300
```

The rules come from spec §5, "Probe".
- `classifyProbe`:
  - ProbeOk when G answers 204, or A answers 200 with a body that equals `kAppleSuccessBody` after trimming trailing whitespace.
  - Limited when the status is 200 or in 400 to 599 and the body is empty or `contentLength` is between 0 and 4.
  - NoNetwork when `!transportOk`.
  - Captive otherwise. `location` is set only from the Location header.
- `combineCycle` decides in this order:
  1. NoNetwork if there is no route.
  2. Captive if any probe is captive. The view URL is G's probe URL when G is captive, otherwise A's, and the location comes from that probe.
  3. Online, OnlineClockWrong or Partial if any probe is ProbeOk, according to the confirmation (Ok204, ValidityOnly or OtherFailure).
  4. Limited if any probe is limited.
  5. NoNetwork.

- [ ] **Step 1: Write the failing test** `tests/captive_probe_classify_test.cpp` (the url_test.cpp `CHECK` style). Let `G` be the G URL and `A` the A URL.

```cpp
const std::string G = "http://connectivitycheck.gstatic.com/generate_204", A = "http://captive.apple.com/hotspot-detect.html";
CHECK(classifyProbe(ProbeKind::Google204, G, {true,204,"",0,""}).cls == ProbeClass::ProbeOk);
CHECK(classifyProbe(ProbeKind::AppleSuccess, A, {true,200,std::string(kAppleSuccessBody)+"\n",69,""}).cls == ProbeClass::ProbeOk);
CHECK(classifyProbe(ProbeKind::AppleSuccess, A, {true,200,"<html>loginSuccess</html>",25,""}).cls == ProbeClass::Captive);
CHECK(classifyProbe(ProbeKind::Google204, G, {true,200,"",0,""}).cls == ProbeClass::Limited);
CHECK(classifyProbe(ProbeKind::Google204, G, {true,200,"abc",3,""}).cls == ProbeClass::Limited);
CHECK(classifyProbe(ProbeKind::Google204, G, {true,403,"",0,""}).cls == ProbeClass::Limited);
CHECK(classifyProbe(ProbeKind::Google204, G, {true,403,"<form>login</form>",18,""}).cls == ProbeClass::Captive);
CHECK(classifyProbe(ProbeKind::Google204, G, {true,511,"<a href=\"/p\">Sign in</a>",24,""}).cls == ProbeClass::Captive);
CHECK(classifyProbe(ProbeKind::Google204, G, {false,0,"",-1,""}).cls == ProbeClass::NoNetwork);
auto v = classifyProbe(ProbeKind::Google204, G, {true,302,"",0,"portal/x/"});
CHECK(v.cls == ProbeClass::Captive && v.location == "http://connectivitycheck.gstatic.com/portal/x/" && v.probeUrl == G);
CHECK(resolveUrl("http://h/a/b?q", "//o/p")  == "http://o/p");
CHECK(resolveUrl("http://h/a/b?q", "?x=1")   == "http://h/a/b?x=1");
CHECK(resolveUrl("http://h/a/b",   "#f")     == "http://h/a/b#f");
CHECK(resolveUrl("http://h/a/b/c", "../d")   == "http://h/a/d");
CHECK(resolveUrl("http://h/a",     "https://x/y") == "https://x/y");
CHECK(resolveUrl("http://h:8080/a", "/z")    == "http://h:8080/z");
CHECK(tlsConfirmFrom(true, 0, 204) == TlsConfirm::Ok204);
CHECK(tlsConfirmFrom(true, 0, 200) == TlsConfirm::OtherFailure);
CHECK(tlsConfirmFrom(false, kTlsExpired, 0) == TlsConfirm::ValidityOnly);
CHECK(tlsConfirmFrom(false, kTlsNotActivated, 0) == TlsConfirm::ValidityOnly);
CHECK(tlsConfirmFrom(false, kTlsExpired|kTlsUnknownCa, 0) == TlsConfirm::OtherFailure);
CHECK(tlsConfirmFrom(false, 0, 0) == TlsConfirm::OtherFailure);
ProbeVerdict ok{ProbeClass::ProbeOk,G,""}, okA{ProbeClass::ProbeOk,A,""}, cap{ProbeClass::Captive,A,"http://p/"},
             lim{ProbeClass::Limited,G,""}, nn{ProbeClass::NoNetwork,G,""};
CHECK(combineCycle(true, ok, okA, TlsConfirm::Ok204).result == CycleResult::Online);
CHECK(combineCycle(true, ok, okA, TlsConfirm::ValidityOnly).result == CycleResult::OnlineClockWrong);
CHECK(combineCycle(true, ok, okA, TlsConfirm::OtherFailure).result == CycleResult::Partial);
auto c = combineCycle(true, ok, cap, TlsConfirm::Ok204);   // any captive is conclusive; never view a ProbeOk URL
CHECK(c.result == CycleResult::Captive && c.viewUrl == A && c.locationUrl == "http://p/");
CHECK(combineCycle(true, lim, nn, TlsConfirm::OtherFailure).result == CycleResult::Limited);
CHECK(combineCycle(false, ok, okA, TlsConfirm::Ok204).result == CycleResult::NoNetwork);
CHECK(hasDefaultRoute("Iface\tDestination\tGateway\nwlan0\t00000000\t0101A8C0\n", "wlan0"));
CHECK(!hasDefaultRoute("Iface\tDestination\tGateway\nwlan0\t0001A8C0\t00000000\n", "wlan0"));
CHECK(parseHttpDate("Sun, 25 Oct 2026 14:02:12 GMT") == 1792936932LL);
CHECK(parseHttpDate("garbage") == -1);
CHECK(clockSkewed(1000, 1301) && !clockSkewed(1000, 1300));
CHECK(std::string(cycleResultName(CycleResult::Partial)) == "Partial");
```

- [ ] **Step 2: Run the test and confirm it fails.** Run `bash scripts/run-tests.sh`. Expected: `FAIL (compile): captive_probe_classify_test` (missing header).
- [ ] **Step 3: Implement `probe_classify.h`.** Header-only, standard library only. `parseHttpDate` accepts only IMF-fixdate (`%a, %d %b %Y %H:%M:%S GMT`) and computes UTC without `timegm` (days-from-civil), so it is portable.
- [ ] **Step 4: Run the test and confirm it passes.** `bash scripts/run-tests.sh` ends with `ALL HOST TESTS OK`.
- [ ] **Step 5: Commit.** Message: `feat(captive): probe classification`.

### Task 4: Re-probe scheduler

**Files:**
- Create: `engine/wpeqt/captive/probe_scheduler.h`, `tests/captive_probe_scheduler_test.cpp`

**Interfaces (Produces, namespace `rmweb::captive`):**

```cpp
enum class Trigger { PageEvent, SubmitLike, ProbeHostNav };
class ProbeScheduler {
public:
  static constexpr int64_t kMinGapMs = 3000, kSubmitDelayMs = 5000, kIdleMs = 10000;
  void arm(int64_t nowMs);                     // portal shown: idle timer starts
  void disarm();                               // start-up screens / exiting: nothing is due
  void trigger(Trigger t, int64_t nowMs);
  int64_t dueAtMs() const;                     // earliest allowed start of the next cycle; INT64_MAX if disarmed
  bool startIfDue(int64_t nowMs);              // true => caller starts a cycle now (marks in-flight)
  void cycleFinished(int64_t nowMs);
};
```

The semantics come from spec §5, "Re-probe triggers".
- A PageEvent asks for a cycle at `now`. A SubmitLike asks at `now + 5000`. A ProbeHostNav asks at `now` and ignores the minimum gap.
- The idle request is due at `max(lastFinish, lastPageEvent) + 10000`.
- A request is due at `max(requestAt, lastStart + 3000)` (no gap for ProbeHostNav). It is never started while a cycle is in flight.
- Requests are deferred, never dropped. One cycle satisfies every request due at or before its start; later requests stay pending.

- [ ] **Step 1: Write the failing test** with these cases:
  - `arm(0)`, then `startIfDue(9999)` is false and `startIfDue(10000)` is true (idle).
  - After `startIfDue(0)` and `cycleFinished(500)`, `trigger(PageEvent, 1000)` makes `dueAtMs() == 3000`, `startIfDue(2999)` false and `startIfDue(3000)` true (deferred, not dropped).
  - `trigger(PageEvent)` during flight leaves `startIfDue` false until `cycleFinished(t)`, then true at `max(t, lastStart + 3000)`.
  - `trigger(SubmitLike, 1000)` gives `dueAtMs() == 6000`.
  - `trigger(ProbeHostNav, 1000)` one second after a start gives `startIfDue(1000) == true`.
  - A page event at `t` resets idle: the next idle cycle is due at `t + 10000`, not at `lastFinish + 10000`.
  - Five PageEvents inside one 3 s window start exactly one cycle.
  - `disarm()` makes `dueAtMs() == INT64_MAX`.
- [ ] **Step 2: Run the test and confirm it fails.** `bash scripts/run-tests.sh` reports `FAIL (compile): captive_probe_scheduler_test`.
- [ ] **Step 3: Implement `probe_scheduler.h`.** Header-only. Track `lastStart`, `lastFinish`, `lastPageEvent`, `inFlight`, `armed`, the earliest pending normal request time and a pending bypass flag.
- [ ] **Step 4: Run the test and confirm it passes.** `ALL HOST TESTS OK`.
- [ ] **Step 5: Commit.** Message: `feat(captive): re-probe scheduler`.

### Task 5: Config, captive flag, argv, exit codes

**Files:**
- Create: `engine/wpeqt/captive/config.h`, `engine/wpeqt/captive/exit_codes.h`, `tests/captive_config_test.cpp`

**Interfaces (Produces, namespace `rmweb::captive`):**

```cpp
struct CaptiveConfig {
  std::string gUrl = "http://connectivitycheck.gstatic.com/generate_204";
  std::string aUrl = "http://captive.apple.com/hotspot-detect.html";
  std::string confirmUrl = "https://www.google.com/generate_204";
  int idleTimeoutS = 600;
  std::string caFile;            // test-only trust anchor
  bool overridden = false;       // any key present => "Test probe" badge
};
CaptiveConfig parseCaptiveConfig(const std::string& text);   // keys: probe_google probe_apple confirm idle_timeout_s ca_file
bool captiveModeFromEnv(const char* rmwebKiosk);             // true only for exactly "captive"
int stripCaptiveFlag(int argc, char** argv);                 // removes every "--captive", returns new argc
inline constexpr int kExitOnline = 20, kExitCancelled = 21, kExitIdle = 22, kExitNoNetwork = 23, kExitUseAsIs = 24;
const char* exitMeaning(int code);   // "online","cancelled","idle timeout","no network","use network as is",
                                     // "SIGTERM drain","touch grab failed","start-up failure"(2,3),"GUI watchdog"(63),
                                     // "killed by signal N"(>=128, thread-local buffer),"unknown"
```

The file format is `key = value` per line; `#` starts a comment; surrounding whitespace is trimmed; unknown keys are ignored. An invalid or non-positive `idle_timeout_s` keeps 600 but still sets `overridden`.

- [ ] **Step 1: Write the failing test** with these checks:
  - Empty text gives the defaults with `overridden == false`.
  - Each key overrides its field and sets `overridden`.
  - `"  confirm = https://x/c  # c"` gives `confirmUrl == "https://x/c"`.
  - `idle_timeout_s = 0` gives 600.
  - `captiveModeFromEnv("captive")` is true; `nullptr`, `""` and `"Captive"` are false.
  - `stripCaptiveFlag` on `{"app","--captive","x"}` returns 2 with `argv[1] == "x"`.
  - `exitMeaning(20) == "online"`, `exitMeaning(134) == "killed by signal 6"`, `exitMeaning(63) == "GUI watchdog"`.
- [ ] **Step 2: Run the test and confirm it fails.** Compile failure.
- [ ] **Step 3: Implement both headers.**
- [ ] **Step 4: Run the test and confirm it passes.**
- [ ] **Step 5: Commit.** Message: `feat(captive): config, flag and exit codes`.

### Task 6: Log redaction and rotating log

**Files:**
- Create: `engine/wpeqt/captive/log_filter.h`, `tests/captive_log_filter_test.cpp`

**Interfaces (Produces, namespace `rmweb::captive`):**

```cpp
std::optional<std::string> redactLogLine(const std::string& line);   // nullopt => drop the line
class RotatingLog {
public:
  explicit RotatingLog(std::string path, size_t maxBytes = 262144);
  bool append(const std::string& line);   // adds '\n'; if size+line > max: rename path -> path+".1" (replace), start new
};
```

Rules from spec §5, "Override file and logging":
- Drop any line containing `[form]` or `setFieldText`.
- Every URL `scheme://authority...` becomes `scheme://host[:port]`. Userinfo is removed. The path, query and fragment are removed, up to the next whitespace, `"`, `'`, `)`, `]` or `>`.

- [ ] **Step 1: Write the failing test** with these checks:
  - `"[nav] uri=https://portal.x/login?room=12&name=Smith"` becomes `"[nav] uri=https://portal.x"`.
  - `"[tls] cert error: https://u:p@h:8443/a#f (x)"` becomes `"[tls] cert error: https://h:8443 (x)"`.
  - Two URLs on one line are both reduced.
  - `"[form] setFieldText -> pw\nbob"` returns `nullopt`.
  - `"[t] display connected @12ms"` is unchanged.
  - RotatingLog with `maxBytes=100` in a temp directory: after appends totalling 150 bytes, `path.1` exists, and the size of `path` is at most 100.
- [ ] **Step 2: Run the test and confirm it fails.**
- [ ] **Step 3: Implement `log_filter.h`.** Header-only, standard library and POSIX `rename`.
- [ ] **Step 4: Run the test and confirm it passes.**
- [ ] **Step 5: Commit.** Message: `feat(captive): log redaction and rotation`.

### Task 7: Probe runner on libsoup, CLI and host integration test

**Files:**
- Create: `engine/wpeqt/captive/probe_runner.h`, `tools/probe-cli/probe-cli.cpp`, `tests/probe_runner_test.sh`, `tests/fixtures/route-default`, `tests/fixtures/route-none`
- Modify: `.github/workflows/tests.yml` (apt adds `libsoup-3.0-dev pkg-config`)

**Interfaces:**
- Consumes: Task 3 types, Task 5 `CaptiveConfig`.
- Produces (namespace `rmweb::captive`):

```cpp
struct ProbeEndpoints { std::string gUrl, aUrl, confirmUrl, caFile; std::string routeFile = "/proc/net/route"; std::string iface = "wlan0"; };
ProbeEndpoints endpointsFrom(const CaptiveConfig& c);
class ProbeRunner {
public:
  ProbeRunner(GMainContext* ctx, ProbeEndpoints ep);
  void runCycle(std::function<void(const CycleReport&)> done);   // async on ctx; G and A in parallel, then confirm
};
```

- CLI: `probe-cli --google URL --apple URL --confirm URL [--ca FILE] [--route-file FILE] [--iface NAME]`. It prints one line and exits 0:

  ```
  result=<cycleResultName> view=<url> location=<url> g_addr=<addr> skew=<seconds|unknown>
  ```

**Implementation decisions (spec §5).**
- Use a new `SoupSession` per cycle, with `timeout` = 5 and `idle-timeout` = 1. If `caFile` is set, use `tls-database` = `g_tls_file_database_new(caFile)`.
- Every message gets `SOUP_MESSAGE_NO_REDIRECT | SOUP_MESSAGE_NEW_CONNECTION` and `Connection: close`.
- The confirmation message connects `accept-certificate`, keeps the flags and returns FALSE.
- `network-event` with `G_SOCKET_CLIENT_CONNECTED` records the remote address.
- Read the body with `soup_session_send_and_read_async`.
- `static_assert` that each `TlsFlag` equals its `G_TLS_CERTIFICATE_*` value.
- The skew comes from G's `Date` header only.
- Route: read `routeFile` and call `hasDefaultRoute`.

- [ ] **Step 1: Write the failing test** `tests/probe_runner_test.sh`.
  - If `pkg-config --exists libsoup-3.0` fails, print `[tests] libsoup-3.0 not found — skipping probe runner test` and exit 0.
  - Otherwise build `build/probe-cli` with `clang++ -std=c++17 tools/probe-cli/probe-cli.cpp $(pkg-config --cflags --libs libsoup-3.0 gio-2.0)`, start `fake_portal.py --bind 127.0.0.1 --base-port <free>` per case, and assert the printed fields:

  | Case | Setup | Expected |
  |---|---|---|
  | open | `/_control/grant`, confirm `tls_valid` with `--ca` | `result=Online` |
  | captive | `plain-form`, no grant | `result=Captive view=http://127.0.0.1:P/generate_204 location=http://127.0.0.1:P/portal/plain-form/` |
  | relative | `relative-location` | `location=http://127.0.0.1:P/portal/relative-location/` |
  | 511 | `status-511` | `result=Captive` |
  | limited | G and A pointed at `/raw?status=200&body=&len=0` | `result=Limited` |
  | partial-self | granted, confirm `tls_self` | `result=Partial` |
  | clock | granted, confirm `tls_expired` | `result=OnlineClockWrong` |
  | partial-closed | granted, confirm on a closed port | `result=Partial` |
  | noroute | `--route-file tests/fixtures/route-none` | `result=NoNetwork` |
  | slow | G and A at `/raw?status=204&delay_s=7` | `result=NoNetwork`, and the CLI returns in under 6.5 s |
  | skew | `--date-offset-s 3600`, granted | `skew=3600`, within ±5 |

  Both route fixtures use iface `test0`; `route-default` holds a default route.
- [ ] **Step 2: Run it and confirm it fails.** `bash tests/probe_runner_test.sh` fails at the compile step (missing `probe-cli.cpp`).
- [ ] **Step 3: Implement `probe_runner.h` and `probe-cli.cpp`.** The CLI runs a `GMainLoop` on the default context and quits on `done`.
- [ ] **Step 4: Run it and confirm it passes.** `bash tests/probe_runner_test.sh` prints `probe runner: 11/11 OK`; `bash scripts/run-tests.sh` ends with `ALL HOST TESTS OK`.
- [ ] **Step 5: Commit.** Message: `feat(captive): libsoup probe runner and CLI`.

### Task 8: Captive controller

**Files:**
- Create: `engine/wpeqt/captive/controller.h`, `tests/captive_controller_test.cpp`

**Interfaces:**
- Consumes: Task 3 (`CycleReport`, `CycleResult`), Task 4 (`ProbeScheduler`), Task 5 (`CaptiveConfig`, exit codes).
- Produces (namespace `rmweb::captive`, standard library only):

```cpp
enum class Screen { None, AlreadyOpen, ConnectedClockWrong, NoNetwork, Limited, Partial, Connected };
enum class Button { Retry, Close, UseAsIs, Cancel };
const char* screenText(Screen s);            // exact copy from Global Constraints; None -> ""
std::vector<Button> screenButtons(Screen s); // NoNetwork:{Close}; Limited:{Retry,Close}; Partial:{Retry,UseAsIs,Close}; others:{}
inline constexpr const char* kStatusSecureFailing = "Secure connections still failing";
inline constexpr const char* kStatusClockWrong    = "Tablet clock looks wrong";
struct ControllerHooks {
  std::function<int64_t()> nowMs;
  std::function<void(int delayMs, std::function<void()>)> after;              // one-shot timer on the engine thread
  std::function<void(std::function<void(const CycleReport&)>)> runCycle;
  std::function<void(const std::string& url)> navigate;
  std::function<void(Screen)> showScreen;
  std::function<void(const std::string& text)> setStatus;                    // "" clears
  std::function<void(int code)> exitWith;                                    // GUI drains + _Exit; called at most once
};
class CaptiveController {
public:
  CaptiveController(CaptiveConfig cfg, ControllerHooks hooks);
  void start();                                       // first cycle -> start-up table (spec §5)
  void onNavigationDecision(const std::string& uri);  // every navigation decision (WebKit reports main and sub frames alike); PageEvent, or ProbeHostNav after leaving a probe host
  void onLoadEnd(const std::string& uri);             // finished/failed/cancelled/interrupted; PageEvent + Location fallback
  void onSubmitLike();                                // form submit, keyboard Go, control tap
  void onUserActivity();                              // any touch: resets the idle timeout
  void onButton(Button b);
};
```

**Behaviour, from spec §5 "Start-up" and "While the portal is shown".**
- The controller ticks every 500 ms through `after` while it is running. Each tick calls `startIfDue`, then `runCycle`.
- Start-up results:

  | Result | Action |
  |---|---|
  | Online | AlreadyOpen, then `exitWith(20)` after 2000 ms |
  | OnlineClockWrong | ConnectedClockWrong, exit 20 after 3000 ms |
  | NoNetwork | NoNetwork screen; exit 23 on Close or after 20000 ms |
  | Limited | Limited screen; Retry runs a cycle; Close exits 23 |
  | Partial | Partial screen; Retry; UseAsIs exits 24; Close exits 23 |
  | Captive | `showScreen(None)`, `navigate(viewUrl)`, arm the scheduler |

- Results while the portal is shown:

  | Result | Action |
  |---|---|
  | Online | Connected, exit 20 after 2000 ms |
  | OnlineClockWrong | ConnectedClockWrong, exit 20 after 2000 ms |
  | Partial | `setStatus(kStatusSecureFailing)` |
  | Captive, NoNetwork or Limited | no navigation and no exit |

- Location fallback: a load end whose host is the view URL's host, when the last captive outcome had a location, triggers `navigate(location)` once.
- A report with `dateKnown && |skewSec| > 300` triggers `setStatus(kStatusClockWrong)`.
- Idle: no `onUserActivity` for `idleTimeoutS × 1000` ms exits 22.
- Buttons: Cancel exits 21 and UseAsIs exits 24 from any state.
- The controller never navigates to a URL whose last probe class was ProbeOk.

- [ ] **Step 1: Write the failing test** with a fake clock, a timer queue, scripted `runCycle` results and recorded hooks. Cases:
  - `startup_online_exits_20_after_2s`
  - `startup_clock_wrong_exits_20_after_3s`
  - `startup_no_network_autocloses_23_after_20s`
  - `startup_no_network_close_exits_23`
  - `startup_limited_retry_runs_cycle_then_close_23`
  - `startup_partial_use_as_is_24`
  - `startup_captive_navigates_to_view_url`
  - `never_views_probe_ok_url` (G ok, A captive gives `navigate(A)`)
  - `location_fallback_when_view_stays_on_probe_host`
  - `no_fallback_without_location`
  - `page_event_then_online_shows_connected_exits_20`
  - `partial_while_shown_sets_status_keeps_page`
  - `no_network_while_shown_keeps_page` (Review Focus 4: no navigate, no exit, cycles continue)
  - `refresh_storm_is_rate_limited` (Review Focus 3: `onLoadEnd` every 500 ms for 60 s gives at most 21 cycles)
  - `probe_host_nav_runs_immediately`
  - `idle_timeout_exits_22`, `activity_resets_idle`, `idle_override_5s_exits_22_at_5s`
  - `cancel_exits_21`, `use_as_is_exits_24`
  - `skew_sets_clock_status`
  - `exit_called_once` (Cancel after a pending exit timer does not call `exitWith` again)
- [ ] **Step 2: Run the test and confirm it fails.**
- [ ] **Step 3: Implement `controller.h`.**
- [ ] **Step 4: Run the test and confirm it passes.**
- [ ] **Step 5: Commit.** Message: `feat(captive): controller state machine`.

### Task 9: Takeover launcher and host tests

**Files:**
- Create: `device/wifi-signin/lib.sh`, `device/wifi-signin/appload-entry.sh`, `device/wifi-signin/launch`, `device/wifi-signin/cleanup`, `device/wifi-signin/external.manifest.json`, `tests/wifi_signin_launcher_test.sh`
- Modify: `scripts/run-tests.sh` (the shellcheck loop adds `device/wifi-signin/lib.sh device/wifi-signin/appload-entry.sh device/wifi-signin/launch device/wifi-signin/cleanup`)

**Interfaces:**
- Produces (`lib.sh`, POSIX sh, sourced by the other three). Every path and timing comes from the environment, with these defaults:

  ```
  WS_RUN=/run/wifi-signin WS_APP=/home/root/xovi/exthome/appload/wifi-signin
  WS_RUNTIME=/home/root/.vellum/share/rmweb-runtime WS_USERDIR=/home/root/.wifi-signin
  WS_SYSPOWER=/sys/power WS_PROC=/proc WS_EPD_LOCK=/tmp/epframebuffer.lock WS_CARD=/dev/dri/card0
  WS_LIBEXEC=/usr/libexec WS_GUARD_S=30 WS_CARD_WAIT_S=5 WS_READY_TIMEOUT_S=30 (0 disables)
  WS_RETRY_WINDOW_S=20 WS_REFRESH_S=60 WS_APP_TERM_WAIT_S=3 WS_HELPER_WAIT_S=10
  ```

  Functions:
  - `ws_log MSG`: stdout with the prefix `[wifi-signin]`, so it lands in the journal;
  - `ws_state_add TOKEN` and `ws_state_has TOKEN`: one token per line in `$WS_RUN/state`; the tokens are `WAKELOCK`, `TRIPLETAP`, `STOPPED_XOCHITL`, `MOUNTED` and `APP=<pid>`;
  - `ws_settle MICROSECONDS`: `usleep`, falling back to `sleep` rounded up;
  - `ws_card_holder`: prints the first PID whose `$WS_PROC/<pid>/fd/*` links to `$WS_CARD`, returning 1 if none;
  - `ws_helper_pids`: PIDs whose `$WS_PROC/<pid>/cmdline` contains `wpe-webkit-2.0/WPE`;
  - `ws_xochitl_age`: integer seconds; MainPID from `systemctl show -p MainPID --value xochitl`; field 22 after the last `)` of `$WS_PROC/<pid>/stat`, divided by 100; the current time is the first field of `$WS_PROC/uptime`;
  - `ws_exit_meaning CODE`: the same strings as Task 5's `exitMeaning`;
  - `ws_wakelock SPEC`: writes to `$WS_SYSPOWER/wake_lock`;
  - `ws_wakeunlock`: writes `wifi-signin` to `$WS_SYSPOWER/wake_unlock`.
- `appload-entry.sh`, `launch` and `cleanup` behave exactly as spec §6 describes, steps 1 to 8.
  - `launch` sources `$WS_RUNTIME/rmweb-env.sh` with `R=$WS_RUNTIME`. It then exports:

    ```
    RMWEB_KIOSK=captive RMWEB_PROFILE=$WS_RUN/profile RMWEB_COOKIES=0
    XDG_CACHE_HOME XDG_DATA_HOME XDG_CONFIG_HOME under $WS_RUN/xdg
    MESA_SHADER_CACHE_DISABLE=true WS_READY_FILE=$WS_RUN/ready WS_LOG=$WS_USERDIR/log
    ```

    and runs `$WS_APP/bin/wifi-signin-wpeqt 2>"$WS_RUN/stderr"`.
  - The overlay copies `$WS_RUNTIME/libexec/wpe-webkit-2.0/.` into `$WS_RUN/ovl/upper/wpe-webkit-2.0/` and mounts `overlay -o lowerdir=$WS_LIBEXEC,upperdir=$WS_RUN/ovl/upper,workdir=$WS_RUN/ovl/work $WS_LIBEXEC`.
  - The retry markers are `Failed to initialize SWTCON` and `[touch] EVIOCGRAB failed`.
  - `cleanup` takes one argument, `trap` or `post`. In `trap` mode it acts only if `$WS_RUN/pid` equals its `$PPID`. In `post` mode it exits 0 if that PID is alive.
- Manifest (spec §7): `{"name":"Wi-Fi Sign-in","application":"/home/root/xovi/exthome/appload/wifi-signin/appload-entry.sh","workingDirectory":"/home/root/xovi/exthome/appload/wifi-signin","qtfb":false,"disablesWindowedMode":true,"aspectRatio":"original"}`.

- [ ] **Step 1: Write the failing test** `tests/wifi_signin_launcher_test.sh`, modelled on `tests/launcher_test.sh`.
  - **Harness.** Stubs on PATH for `systemctl`, `systemd-run`, `mount`, `umount`, `usleep` and `sleep`. Each logs to `stub.log`. The `sleep` and `usleep` stubs add their duration to the fake `$WS_PROC/uptime`. Fake `$WS_PROC/<pid>/{stat,cmdline,fd/}` and fake `$WS_SYSPOWER/{wake_lock,wake_unlock}` files.
  - **Fake app modes** (`APP_MODE`):
    - `online`: touches `$WS_READY_FILE` and exits 20;
    - `cancel`: touches the ready file and exits 21;
    - `hang`: touches the ready file and sleeps;
    - `noready`: sleeps without touching it;
    - `swtcon`: echoes `Failed to initialize SWTCON` to stderr and exits 134;
    - `swtcon-twice`.
  - **Cases.**
    1. `online`: in order, `wake_lock` gets `wifi-signin 900000000000`, then `systemctl stop xovi-tripletap`, `systemctl stop xochitl`, `mount -t overlay`, the app run, `umount`, `wake_lock` gets `wifi-signin 30000000000`, `systemctl reset-failed xochitl`, `systemctl start xochitl`, `systemctl start xovi-tripletap`. Afterwards `$WS_RUN` is gone and the log contains `exit 20 (online)`.
    2. `each_code_meaning`: 20 to 24, 0, 1, 2, 63 and 134 log their meanings.
    3. `term_mid_run`: SIGTERM to `launch` during `hang` restores xochitl.
    4. `launch_sigkilled_post_cleanup`: SIGKILL `launch`, then run `cleanup post`. It undoes the recorded state in reverse; xochitl and tripletap are started.
    5. `lock_live_pid`: exits 11; stub.log has no systemctl stop or start.
    6. `stale_lock_taken_over`.
    7. `guard_uses_boottime`: process start 10000 ticks, uptime 105 s, so it waits 5 s.
    8. `guard_restarts_on_new_mainpid`.
    9. `xochitl_inactive_exits_12_and_never_starts_xochitl`.
    10. `card_held_after_wait_skips_app_and_restores`.
    11. `swtcon_retry_once`: exactly two app runs, with `usleep 3000000` between them.
    12. `swtcon_twice_no_third_run`.
    13. `cancel_no_retry`.
    14. `noready_killed_after_timeout`.
    15. `ready_timeout_zero_disables`.
    16. `tripletap_inactive_left_alone`.
    17. `cleanup_twice_noop`.
    18. `entry_unit_deactivating_exits_0`: `systemctl show` prints `deactivating`, and systemd-run is not called.
    19. `entry_no_systemd_run_exits_13`.
    20. `wakelock_write_fails_never_touches_xochitl`.
- [ ] **Step 2: Run the test and confirm it fails.** `bash tests/wifi_signin_launcher_test.sh` fails because the scripts are missing.
- [ ] **Step 3: Implement the four scripts in POSIX sh.** `launch` traps EXIT, TERM, INT and HUP into `cleanup trap`, and keeps the wakelock refresher as a background loop while the app runs. `appload-entry.sh` is the exec line in spec §6, with `TimeoutStopSec=30` and `MemoryAccounting=yes`.
- [ ] **Step 4: Run the tests and confirm they pass.** `bash scripts/run-tests.sh` reports `ALL HOST TESTS OK`, and shellcheck is clean.
- [ ] **Step 5: Commit.** Message: `feat(launcher): wifi-signin takeover launcher`.

---

## Part B: Toolchain and first device contact

### Task 10: Toolchain on the 3.28 SDK, fast build, vendored app-SDK headers

**Carry-forward from Part A (2026-10-25):**
- Before step 2, check whether a 3.29 SDK exists under `remarkable-codex-toolchain/3.29.*` and prefer it (the Paper Pro runs 3.29); Task 17's `image()` must match the choice. (ledger: R6)
- Before step 1, run `brew install ninja librsvg colima docker docker-buildx`: Task 1 installed only what Part A needed, and Task 12's icon needs `rsvg-convert`. (ledger: R2)
- `fast-build.sh` and `gen-app-sdk.incontainer.sh` are the first rmbrowser ports: add rmbrowser's copyright line from its LICENSE to `NOTICE`, and move the Wi-Fi Sign-in section above NOTICE's closing disclaimer. (ledger: Task 1 minor (deferred))

**Files:**
- Modify: `scripts/fetch-sdk.sh` (the URL becomes `https://storage.googleapis.com/remarkable-codex-toolchain/3.28.0.172/remarkable-production-image-5.8.203-ferrari-public-aarch64-toolchain.sh`, plus its SHA256), `toolchain/Dockerfile` and `toolchain/README.md` (the SDK file name becomes 5.8.203), `engine/wpeqt/CMakeLists.txt` (add `libsoup-3.0 gio-2.0` to `pkg_check_modules(WPE ...)`)
- Create (ported from rmbrowser, MIT): `scripts/fast-build.sh`, `engine/gen-app-sdk.incontainer.sh`
- Create: `third_party/appsdk/usr/**` (generated headers and pkg-config files, committed; they stand in for the spec §7 `wpewebkit-2.48.5.tar.xz` package source, see Task 17) and `third_party/appsdk/README.md` (provenance, regeneration command, licenses: WebKit LGPL-2.1+/BSD, libsoup LGPL-2.0+, libwpe BSD-2, xkbcommon MIT)

**Interfaces:**
- Produces: `./scripts/fast-build.sh app` builds `build/wifi-signin-wpeqt` (an aarch64 ELF) from `engine/wpeqt` against `third_party/appsdk` and the rmweb 0.9.7 runtime libraries. It differs from rmbrowser's script in two ways: it uses `third_party/appsdk` instead of `build/appsdk`, and it names its output `build/wifi-signin-wpeqt`.

- [ ] **Step 1: Start Colima.** Run `colima start --vm-type vz --cpu 6 --memory 8 --disk 80`. Expected: `docker info --format '{{.Architecture}}'` prints `aarch64`.
- [ ] **Step 2: Fetch the SDK and pin it.**
  1. Download the 3.28 installer (497,093,913 bytes).
  2. Check that `openssl dgst -md5 -binary FILE | base64` equals `YyZX2wvtMWETPKx4FXsgNQ==`, the `x-goog-hash` md5.
  3. Record `shasum -a 256` into `fetch-sdk.sh`.
  4. Expected: `./scripts/fetch-sdk.sh` ends with `Saved & verified`.
- [ ] **Step 3: Build the toolchain image.** Use the command in `toolchain/README.md`, tagged `rmweb-sdk`. Expected: `docker run --rm rmweb-sdk bash -lc 'echo $SDKTARGETSYSROOT'` prints a path ending in `cortexa53-crypto-remarkable-linux`.
- [ ] **Step 4: Generate and vendor the headers.**
  1. `./scripts/fast-build.sh sdk`.
  2. Copy `build/appsdk/usr` to `third_party/appsdk/usr`.
  3. Write the README.
  4. Expected: `ls third_party/appsdk/usr/include` shows `wpe-webkit-2.0 libsoup-3.0 wpe-1.0 xkbcommon`.
- [ ] **Step 5: Build the unmodified app.** Run `./scripts/fast-build.sh app` with no captive code included yet. Then check `file build/wifi-signin-wpeqt`, which must report `ELF 64-bit LSB pie executable, ARM aarch64`, and `readelf -d` NEEDED, which must list `libWPEWebKit-2.0.so.1` and `libQt6Quick.so.6`.
  - If the 5.8.203 SDK fails to compile upstream `main.cpp`, stop and record the error. The fallback is upstream's own `5.7.119` SDK pin, with the Task 17 image changed to match. That decision is the owner's.
- [ ] **Step 6: Commit.** Message: `build: 3.28 SDK toolchain, fast build and vendored app SDK`.

### Task 11: Phase 0 on the Paper Pro (device, owner present)

**Carry-forward from Part A (2026-10-25):**
- The Paper Pro is on 3.29: the Prerequisite now allows 3.28.x or 3.29.x. Step 3 must pass on 3.29 before Task 12; if it fails, stop: a downgrade via reManager's OS Manager is the owner's call. (ledger: R6)
- `device-facts.sh` also reports `sh` (`readlink -f /bin/sh`, BusyBox ash expected) and `sleep_fraction` (`sleep 0.2` exits 0 in under 1 s): `ws_settle` falls back from `usleep` to a fractional sleep. (ledger: Task 9 'resolved by controller')
- It also reports `card0_link`: whether xochitl holds the panel, and that fd's target, which must be exactly `/dev/dri/card0` (`ws_card_holder` compares it whole). (ledger: Task 9 'resolved by controller')
- It also reports `epd_lock_with_xochitl`: whether `/tmp/epframebuffer.lock` exists while xochitl runs; acceptance test 4 expects it absent after cleanup restarts xochitl. (ledger: final review spec-alignment declinedToJudge#15)
- Step 5 also records the exit code and run time: the launcher retries only a run that ended within 20 s, with a code other than 0, 20-24 or 63 and a marker in stderr. (ledger: R23)
- Write the `wake_lock_timed` probe with `>>`, as `ws_wakelock` does. (ledger: Task 9 'resolved by controller')
- Add `wake_lock_extend_only`: write `ws-probe 60000000000`, then `ws-probe 2000000000`; after 3 s it must still be listed (a rewrite never shortens a timed lock); then unlock `ws-probe`. (ledger: R27)

**Prerequisite:** the Paper Pro is on 3.28.x, automatic updates are off in reManager, Developer Mode is on, and USB is connected.

**Files:**
- Create: `tools/phase0/device-facts.sh` (runs on the device and prints `key=value` lines), `docs/wifi-signin/phase0-paper-pro.md` (results)

- [ ] **Step 1: Write `device-facts.sh`.** It reports:
  - `os` (from `/usr/share/remarkable/update.conf`);
  - `run_fs` and `tmp_fs` (from `/proc/mounts`);
  - `wake_lock` (whether it exists);
  - `wake_lock_timed`: write `ws-probe 2000000000`, check `grep -w ws-probe /sys/power/wake_lock`, wait 3 s, and check that it is gone;
  - `usleep` (whether the applet exists);
  - `systemd_run` version;
  - `memory_peak`: `systemd-run --scope -p MemoryAccounting=yes sh -c 'cat /proc/self/cgroup'`, then check `/sys/fs/cgroup/<path>/memory.peak`;
  - `power_key_nodes`: for each `/sys/class/input/event*`, the name and whether `capabilities/key` contains bit 116;
  - `resolv` (from `/etc/resolv.conf`) and `dot_dnssec` (from `resolvectl status wlan0` if present);
  - `xochitl_startlimit` (`systemctl show -p StartLimitBurst -p StartLimitIntervalUSec -p OnFailure xochitl`);
  - `tripletap` (`systemctl is-active xovi-tripletap`).
- [ ] **Step 2: Run it and record the results.** Copy it to the device, run `scp tools/phase0/device-facts.sh root@10.11.99.1:/tmp/ && ssh root@10.11.99.1 sh /tmp/device-facts.sh`, and paste the output into `phase0-paper-pro.md`.
  - Expected: `run_fs=tmpfs`, `tmp_fs=tmpfs`, `wake_lock_timed=ok`, `memory_peak=ok`, `power_key_nodes` includes `30370000.snvs:snvs-powerkey key116=1`, and `dot_dnssec` is neither strict nor DoT.
  - Any other value is a finding: stop and report it before Task 12.
- [ ] **Step 3: Run upstream rmweb as-is.**
  1. Unpack `build/release/rmweb-0.9.7` to `/home/root/phase0-rmweb`.
  2. Over SSH, run `RMWEB_ROOT=/home/root/phase0-rmweb systemd-run --unit=phase0-rmweb -p MemoryAccounting=yes /home/root/phase0-rmweb/rmweb https://example.com`.
  3. Check that xochitl stops, the page renders and ⏻ restores xochitl.
  4. Record `memory.peak` from the unit's cgroup while the page is shown.
  5. Remove `/home/root/phase0-rmweb` and `/home/root/xovi/exthome/appload/rmweb` if they exist.
- [ ] **Step 4: Check legibility without the settle flash.** In upstream rmweb's settings, turn Settle flash off, open `https://example.com` and photograph the text. Record whether black text is legible. If it is not, the result is the fallback in spec §5 "Rendering"; the owner decides before Task 13.
- [ ] **Step 5: Force a display-owner collision.** **Warning:** this may reboot the tablet. Ask the owner first, and save notebooks to the cloud before starting. Start upstream rmweb as in step 3, then run `systemctl start xochitl` over SSH. Record the journal (`journalctl -b -1` if it rebooted) and the app's exit status and stderr lines. If the markers differ from `Failed to initialize SWTCON` and `[touch] EVIOCGRAB failed`, update the markers in `device/wifi-signin/launch` and Task 9's test.
- [ ] **Step 6: Commit.** Message: `docs: phase 0 results for Paper Pro`.

### Task 12: Dev deploy and the launcher on the device with the upstream binary

**Carry-forward from Part A (2026-10-25):**
- Time step 4's cleanups: `cleanup (post|trap): undoing` to `cleanup done` must stay under 30 s (stop-post gets a fresh TimeoutStopSec; the host worst case starts xochitl at about 22.6 s). Record xochitl's start latency. (ledger: Task 9 'resolved by controller')
- Identify processes by `/proc/PID/exe` (one `ls -l` pass) or `comm`, not `cmdline`, whose read takes the target's mmap_lock: update `ws_app_alive`, `ws_helper_pids` and the fake /proc; check helper exe paths via the overlay. (ledger: Task 9 POINTER for Task 12)
- A D-state app that survives FINAL_SIGKILL lets `--collect` drop the failed unit, so `appload-entry.sh` would start a session over it: decide whether it also refuses while an app or WPE helper exists. (ledger: Task 9 POINTER for Task 12)
- Step 5 deletes only the file `$APP/launch.env` (its `WS_READY_TIMEOUT_S=0` turns the start-up watchdog off); the hook code stays. Then the journal's start line must not mention `with overrides from`. (ledger: R5)
- After step 3, check that XOVI survived the `systemctl stop`/`start xochitl` pair: the AppLoad icon is still there without a triple press. (ledger: final review launcher-safety declinedToJudge#5)
- Run the launcher host test under the shell device-facts reports and add that run to CI: `apt-get install busybox`, `WS_TEST_SH` = a symlink named `ash` to busybox (`'/bin/busybox sh'` breaks the harness's quoting). (ledger: final review tests-ci#6)
- Cleanup now takes `wifi-signin-exit 30000000000`, then unlocks `wifi-signin` (replacing the Timings row "Final timed wakelock"); 40 s after each exit, `grep -w wifi-signin /sys/power/wake_lock` prints nothing. (ledger: R27)
- Consider `-p KillMode=mixed` (systemd TERMs only `launch`; today the app gets a second TERM mid-drain), but only if test 4(b) still passes: with mixed, a dead `launch` leaves the app to the final SIGKILL. (ledger: Task 9 M1)
- Lock-owner liveness is `kill -0` only (PID reuse): also check the owner's identity (`comm` is `launch`; its exe is the shell), and give `lock_live_pid`'s owner a fake one. (ledger: Task 9 M4)
- `appload-entry.sh` runs `${WS_SYSTEMD_RUN:-systemd-run}`, a test-only seam: the deploy script, `launch.env` and the packages never set it. (ledger: R30)
- In `launch`'s `leave`, when the second `wait` returns 127 and `APP_RC` is set (a TERM right after `wait`), use `APP_RC`, not 127 "unknown". (ledger: Task 9 M7)

**Files:**
- Create: `scripts/deploy-wifi-signin.sh` (over SSH to `root@10.11.99.1`)
- Create: `device/wifi-signin/icon.svg`, plus `device/wifi-signin/icon.png` rendered with `rsvg-convert -w 128` (a Wi-Fi glyph, black on white, legible at 64 px)

**Interfaces:**
- Produces: `./scripts/deploy-wifi-signin.sh [--app BINARY] [--override FILE] [--ca FILE]`. It does four things:
  1. installs `build/release/rmweb-0.9.7/{lib,libexec,share,rmweb-env.sh}` into `/home/root/.vellum/share/rmweb-runtime`;
  2. installs `device/wifi-signin/*` and the binary (default `build/wifi-signin-wpeqt`) as `$APP/bin/wifi-signin-wpeqt`;
  3. optionally copies an override file to `/home/root/.wifi-signin/settings.conf`, and a CA to `/home/root/.wifi-signin/test-ca.pem`;
  4. prints `Refresh AppLoad to see Wi-Fi Sign-in`.

- [ ] **Step 1: Write the deploy script and the icon.**
- [ ] **Step 2: Deploy the upstream binary.** Run `./scripts/deploy-wifi-signin.sh --app build/release/rmweb-0.9.7/bin/rmweb-wpeqt`, then over SSH write `WS_READY_TIMEOUT_S=0` into `$APP/launch.env`. `launch` sources that file if it exists, which is a dev-only hook covered by Task 9's case 15.
- [ ] **Step 3: First takeover.** Triple-press power, refresh AppLoad and tap "Wi-Fi Sign-in". Expected:
  - the upstream browser appears;
  - `journalctl -u wifi-signin` shows the launch sequence from Task 9 case 1;
  - ⏻ exits 0 and xochitl is back within 10 s;
  - `systemctl is-active xovi-tripletap` prints `active`;
  - `/run/wifi-signin` is gone.
- [ ] **Step 4: Kill safety (acceptance test 4) with the upstream binary.** Run `kill -9 $(pidof wifi-signin-wpeqt)` and `systemctl kill -s KILL --kill-whom=main wifi-signin`, three times each. Run test 4's checks after each.
- [ ] **Step 5: Remove the dev hook and commit.** Delete `$APP/launch.env`. Commit message: `feat(deploy): dev deploy script and icon`.

---

## Part C: Captive mode in the app (device-verified)

Each task builds with `./scripts/fast-build.sh app` and deploys with `./scripts/deploy-wifi-signin.sh`. Device runs use the fake portal on the Mac:

```bash
python3 tools/fake-portal/fake_portal.py --bind 0.0.0.0 --base-port 8080 --variant <v> --cert-dir build/certs --san <mac-ip> --san <mac-ip-with-dashes>.sslip.io
```

The deploy passes an override file setting:

```
probe_google = http://<mac-ip>:8080/generate_204
probe_apple = http://<mac-ip>:8080/hotspot-detect.html
confirm = https://<mac-ip>:8082/confirm
ca_file = /home/root/.wifi-signin/test-ca.pem
```

### Device checks used in Part C and Part D

Device-verified tasks follow TDD with a check script instead of a unit test. You write the check first, run it and watch it fail, implement, and run it again until it passes. Check scripts run on the Mac, drive the tablet over SSH, and print one `PASS name` or `FAIL name: reason` line per check, ending with `N/M checks passed`. A check that needs the owner prints `ACTION: <what to tap or press>` and waits for the expected journal line.

`tools/device-checks/lib.sh` is created in Task 13 and sourced by every check script. It provides:

- `dev_ssh CMD`: `ssh -o BatchMode=yes root@${DEV_HOST:-10.11.99.1}`;
- `mac_ip`: `${MAC_IP:-10.11.99.2}`, the Mac's address on the USB link;
- `portal_start VARIANT [args]` and `portal_stop`: run `tools/fake-portal/fake_portal.py` in the background with `--bind 0.0.0.0 --base-port 8080 --cert-dir build/certs --san $(mac_ip) --san <mac-ip-with-dashes>.sslip.io`;
- `portal_grant`: `curl -X POST http://127.0.0.1:8080/_control/grant`;
- `session_start`: runs `$APP/appload-entry.sh` over SSH, the same path AppLoad uses;
- `wait_exit CODE TIMEOUT_S`: polls `journalctl -u wifi-signin -o cat` for `[wifi-signin] exit CODE (`;
- `xochitl_back_within S`: `systemctl is-active --quiet xochitl` within S seconds, with an unchanged `/proc/sys/kernel/random/boot_id`;
- `ask MSG`: prints `ACTION: MSG`.

The tablet must also be on a Wi-Fi network, because the probe checks wlan0's default route. A home network or a phone hotspot works.

### Task 13: App foundation: flag, exit path, ready file, log handler, rendering, power key

**Carry-forward from Part A (2026-10-25):**
- `captiveLogHandler` caps each message at a few KB before `redactLogLine`: the filter's closer look-behind is quadratic (400 KB of URLs took 14 s). (ledger: Task 6 minor (deferred, from re-review))
- App lines never reach the journal (stderr goes to `/run/wifi-signin/stderr`): give `tools/device-checks/lib.sh` a helper that greps only the log lines (`/home/root/.wifi-signin/log*`) added since `session_start`, for Tasks 13-18. (ledger: final review spec-alignment#2)
- Step 1's `no_vendor_refresh` is replaced: this session's log lines hold `[captive] mode on` and no `[refresh] EPFramebuffer ready` (the journal grep passes even if `epaper.init()` runs). (ledger: final review spec-alignment#2)
- Call `argc = stripCaptiveFlag(argc, argv)` before `QGuiApplication app(argc, argv)` (main.cpp:3545, it takes `int&`), not just before the url and savePath parse (3566-3567). (ledger: Task 5 POINTER for Task 13)
- `power_reader.h` ports rmbrowser's PowerReader: credit it in its header comment and check that `NOTICE` has rmbrowser's copyright line (Task 10). (ledger: Task 1 minor (deferred))
- Replace plan task numbers in Part A comments (log_filter.h:8, probe_runner.h:42, captive_config_test.cpp:297, launch:19) with the file or function meant. (ledger: final review deferred-triage#30)

**Files:**
- Modify: `engine/wpeqt/main.cpp`
  - globals: add `static bool g_captive = false;` next to the other `g_*` globals;
  - `main()` at 3511: set `g_captive` from `captiveModeFromEnv(getenv("RMWEB_KIOSK"))` and `argc = stripCaptiveFlag(argc, argv)` before the url and savePath parse (3567-3568); in captive mode both are empty;
  - epaper init at 3715: skip `epaper.init()` entirely when `g_captive`;
  - `WpeView::onFrameSwapped` (2674): touch the ready file;
  - the TERM poll (3826-3836) and the ⏻ case (3778-3783): route to `captiveExit` when `g_captive`;
  - the `linkMissed` connection (3816): ignored in captive mode; `setChromeOn(true)` is forced.
- Create: `engine/wpeqt/captive/power_reader.h`, ported from rmbrowser `engine/wpeqt/main.cpp:3934-3975` (`PowerReader`), but choosing the node by KEY_POWER capability (`EVIOCGBIT(EV_KEY, …)`, bit 116) instead of by name.
- Create: `tools/device-checks/lib.sh`, `tools/device-checks/13-foundation.sh`

**Interfaces:**
- Consumes: Task 5 (`captiveModeFromEnv`, `stripCaptiveFlag`, exit codes), Task 6 (`redactLogLine`, `RotatingLog`).
- Produces:
  - `[[noreturn]] static void captiveExit(WpeView* view, int code)`: `view->drainForExit(); fflush(nullptr); std::_Exit(code);`. This is the only exit path in captive mode.
  - `class PowerReader : public QObject`, with a `pressed()` signal emitted on key-down (`value == 1`). It runs on its own `QThread` and holds EVIOCGRAB for the app's lifetime. If no node reports KEY_POWER, it logs `[power] no KEY_POWER node` and never emits.
  - `static void captiveLogHandler(QtMsgType, const QMessageLogContext&, const QString&)`, installed with `qInstallMessageHandler` when `g_captive`. It writes the raw line to stderr (the launcher sends that to tmpfs) and `redactLogLine(line)` to a static `RotatingLog` at `getenv("WS_LOG")`, default `/home/root/.wifi-signin/log`. It creates the parent directory with mode 0700.
  - Ready file: `getenv("WS_READY_FILE")`, default `/run/wifi-signin/ready`. It is created once, on the first `onFrameSwapped`.
  - In captive mode, `main()` logs `[captive] mode on`.

- [ ] **Step 1: Write the failing device check.** Write `tools/device-checks/lib.sh` as described above. Then write `13-foundation.sh` with these checks:
  - `ready_file`: `/run/wifi-signin/ready` exists within 30 s of `session_start`;
  - `captive_log`: `/home/root/.wifi-signin/log` exists and contains `[captive] mode on`;
  - `no_vendor_refresh`: `journalctl -u wifi-signin` has no `[refresh] EPFramebuffer ready`;
  - `no_query_in_log`: `grep -cE '://[^ ]*\?' /home/root/.wifi-signin/log` is 0;
  - `power_cancels`: `ask "Press the power button once"`, then `wait_exit 21 30` and `xochitl_back_within 10`;
  - `term_drains`: `session_start`, then `systemctl stop wifi-signin`, `wait_exit 0 15` and `xochitl_back_within 10`.
- [ ] **Step 2: Run the check and confirm it fails.** Run `bash tools/device-checks/13-foundation.sh` with the Task 12 deployment, which is the upstream binary. Expected: `FAIL ready_file`, and the other checks fail too.
- [ ] **Step 3: Implement the hooks and `power_reader.h`.** In `main()`, create PowerReader only when `g_captive`, and connect `pressed` to `captiveExit(view, kExitCancelled)` with a queued connection.
- [ ] **Step 4: Build, deploy and run the check.**
  1. `./scripts/fast-build.sh app && ./scripts/deploy-wifi-signin.sh`
  2. `bash tools/device-checks/13-foundation.sh`

  Expected: `6/6 checks passed`. `bash scripts/run-tests.sh` still ends with `ALL HOST TESTS OK`.
- [ ] **Step 5: Commit.** Message: `feat(captive): flag, exit path, ready file, log handler, power key`.

### Task 14: Controller wiring, probe runner, start-up screens, captive bar

**Carry-forward from Part A (2026-10-25):**
- A controller navigation must never pass the `rmweb:tls-continue` gate, which trusts `expectUserNav` as a tap (main.cpp:445): the `navigate` hook must not set `m_expectUserNav` once Task 15 drops the auto-refresh guard, or use its own flag. (ledger: final review controller#2)
- Step 1's journal greps become log reads (Task 13's helper): `already_open` needs the `showScreen` hook to log the screen text; `portal_then_idle_grant` looks for `[nav] uri=http://<mac-ip>:8080`. (ledger: final review spec-alignment#2)
- In `onLoadFailed`'s non-cancelled branch, call `m_ctl->onLoadEnd(uri)` after `load_alternate_html` (main.cpp:1893) so the Location fallback wins; keep it before the cancelled early return (or defer `load_uri`). (ledger: R31)
- Add a `14-flow.sh` check that fails the view's load of the probe URL after the probe saw 302 + Location (e.g. a fake-portal mode resetting the second request); the log must show the Location host. (ledger: R31)
- Create and declare `m_probe` before `m_ctl` (the runner outlives the controller), never recreate either mid-session, and call `runCycle` only through the controller hook: a destroyed or overlap-refused runner never calls `done`, which stops re-probing. (ledger: final review controller#3)
- Call `engine.captiveActivity()` from every gesture, not only the tap lambda: also the `swipe` (main.cpp:3740) and `longPress` (:3820) connections, or TouchReader's touch-down. (ledger: final review controller#4)
- No stale load end may reach `onLoadEnd` between `navigate(viewUrl)` and that load's own end: the first load end after it consumes the Location fallback (R21). (ledger: Task 8 POINTER for Task 14)
- Log every cycle's result with `gAddress`, `aAddress` and the new `CycleReport::confirmAddress` (spec §5: the resolved address is logged). (ledger: R33)
- Add a device `clock_wrong` case: grant, `confirm = https://<mac-ip>:8084/confirm` (expired leaf); the log shows `Connected. The tablet clock looks wrong.`, then `wait_exit 20 15`. (ledger: Task 7 'resolved by controller')
- The TLS session cache is process-global: if one session can run two confirmations to a host, record whether both report OnlineClockWrong, then reword probe_runner.h:9-10. (ledger: final review deferred-triage#10)
- An unloadable `ca_file` falls back to the system trust store (probe_runner.h:214): fail closed (an empty `GTlsFileDatabase`) or reword the comment. (ledger: final review deferred-triage#9)
- In `onCycleDone`, prefer an Online result over the idle exit when both are due at once (idle 22 wins today); pin it with a controller test. (ledger: final review deferred-triage#13)
- Re-check `exit_ != ExitState::None` after `updateStatus` and before each later hook call in `setScreen` and `enterPortal`; test a `setStatus` hook that exits re-entrantly. (ledger: final review deferred-triage#12)
- Bound the probe body read (`soup_session_send_async`, at most 64 KB), add `-Werror` to the probe build, and pin the 5 s deadline with a blackholed connect target if CI timing allows. (ledger: final review deferred-triage#11)
- Optional: a per-cycle deadline in the controller (about 15 s) that treats a silent cycle as NoNetwork and logs it. (ledger: final review controller#3)
- Make `scheduler()` a narrow test-only accessor unless main.cpp logs from it; wrap controller.h:43 (158 characters). (ledger: final review deferred-triage#14)

**Files:**
- Modify: `engine/wpeqt/main.cpp`
  - `WpeEngine::start()` (317): after the view is mapped (around 392), when `g_captive`:
    - read `slurp("/home/root/.wifi-signin/settings.conf")` (`slurp` is at 195) into `parseCaptiveConfig`;
    - create `m_probe` (`ProbeRunner(m_ctx, endpointsFrom(cfg))`) and `m_ctl` (`CaptiveController(cfg, hooks)`);
    - emit `captiveBadge(cfg.overridden)`.
  - `loadInitial()` (1503): when `g_captive`, call `m_ctl->start()` instead of loading a page.
  - `decide-policy` navigation branch (after 425): when `g_captive`, call `m_ctl->onNavigationDecision(uri)`.
  - `onLoadChanged` at `WEBKIT_LOAD_FINISHED` (1732) and `onLoadFailed` (1852): when `g_captive`, call `m_ctl->onLoadEnd(uri)`. In `onLoadFailed`, the call goes before the early return for cancelled or interrupted loads, so those load ends count too.
  - `onUri` (1538): emit `captiveHost(host)`.
  - `WpeView` (2175):
    - append `UseAsIs, Cancel` to `enum Hit` (2356);
    - when `g_captive`, `chromeHitRect` (2418), `hitChrome` (2368) and the bar painting in `paint` (2811-2890) use the captive layout: Back, Reload, the host label, Use network as is, Cancel;
    - add the start-up screen overlay and the status and badge pill, using `pillZone()` (3083).
  - The tap dispatch in `main()` (3748):
    - when `g_captive`, every tap first calls `engine.captiveActivity()`;
    - a screen-button hit calls `engine.captiveButton(int(button))`;
    - chrome Back and Reload use the existing engine calls;
    - UseAsIs and Cancel call `engine.captiveButton`.
- Create: `tools/device-checks/14-flow.sh`

**Interfaces:**
- Consumes: Task 7 (`ProbeRunner`, `endpointsFrom`), Task 8 (all controller types), Task 13 (`captiveExit`).
- Produces. On `WpeEngine`:
  - `Q_SIGNALS`: `void captiveScreen(int screen); void captiveStatus(const QString &text); void captiveHost(const QString &host); void captiveBadge(bool testProbe); void captiveExitRequested(int code);`
  - `void captiveButton(int button); void captiveActivity();` Both marshal to the engine thread with `marshalToCtx` (1168).
- Produces. On `WpeView`:
  - `void setCaptiveScreen(int screen); void setCaptiveStatus(const QString &text); void setCaptiveHost(const QString &host); void setCaptiveBadge(bool on);`
  - `int captiveButtonAt(int x, int y) const;` returns a `Button` value or -1.
  - Screen text and buttons come only from `screenText` and `screenButtons`.
- Controller hooks in `start()`:
  - `nowMs`: `g_get_monotonic_time() / 1000`;
  - `after`: a `g_timeout_source_new(ms)` attached to `m_ctx`;
  - `runCycle`: `m_probe->runCycle`;
  - `navigate`: sets `m_expectUserNav = true`, then `webkit_web_view_load_uri`;
  - `showScreen`, `setStatus` and `exitWith`: emit the matching signals.
- `captiveExitRequested` is connected, queued, to `captiveExit(view, code)` in `main()`.

- [ ] **Step 1: Write the failing device check** `14-flow.sh`. Each case starts the fake portal with the given variant and the Part C override:

  | Check | Setup | Expect |
  |---|---|---|
  | `already_open` | `plain-form`, then `portal_grant` before the start | `wait_exit 20 15`; the journal contains `This network is already open` |
  | `portal_then_idle_grant` | `plain-form` | the journal shows a navigation to host `<mac-ip>:8080`; after `portal_grant` with no tap, `wait_exit 20 25` |
  | `https_confirm_ok` | the same run | exit 20 proves the libsoup TLS confirmation validated `ca_file` from the UI process (Phase 0 item) |
  | `no_network_autoclose` | override with every endpoint on closed ports | `wait_exit 23 30` |
  | `power_on_start_screen` | same as `no_network_autoclose` | `ask "Press the power button once"`, then `wait_exit 21 15` (Review Focus 5) |
  | `limited_close` | override with G and A on `/raw?status=200&body=&len=0` | `ask "Tap Close"`, then `wait_exit 23 30` |
  | `partial_use_as_is` | granted, `confirm` on closed port 8087 | `ask "Tap Use network as is"`, then `wait_exit 24 30` |
  | `bar_cancel` | `plain-form` | `ask "Tap Cancel"`, then `wait_exit 21 30` |
  | `test_probe_badge` | any | `ask "Is the Test probe badge visible? Tap Cancel if yes"`, then `wait_exit 21 30` |

  Every case ends with `xochitl_back_within 10`.
- [ ] **Step 2: Run the check and confirm it fails.** Expected: `already_open` fails, because the start page loads instead.
- [ ] **Step 3: Implement the wiring, the screens and the captive bar.** The host label is elided in the middle so it fits the Move's 954 px width. The status pill shows `kStatusSecureFailing` or `kStatusClockWrong` when set. The badge pill shows `Test probe`.
- [ ] **Step 4: Build, deploy and run the check.** Expected: `9/9 checks passed`. Host tests still pass.
- [ ] **Step 5: Commit.** Message: `feat(captive): controller wiring, start-up screens and captive bar`.

### Task 15: Privacy and hardening in the engine

**Carry-forward from Part A (2026-10-25):**
- Step 1's `privacy` adds a `login-password` run (a fake-portal variant from the Part A fix wave) where the owner types a unique username and password: four `wait_exit 20 60`, then 0 hits for both in the log and `/home/root/.rmweb/passwords.txt`. (ledger: final review spec-alignment#1)
- Put the captive allowlist before upstream's `rmweb:` dispatch (main.cpp:427), so `[nav] unknown rmweb: command ignored: %s` (page text) never logs; keep reader mode unreachable so `[reader] not applied: %s` never fires. (ledger: Task 6 POINTER for Task 15)
- `scheme_blocked` also asserts that this session's log lines hold no `[nav] unknown rmweb: command`; the filter's opaque-URL redaction (R29) is only a second guard. (ledger: final review deferred-triage#8)
- `rmweb:tls-continue` passes the allowlist only into upstream's gate (main.cpp:445), whose `expectUserNav` must come from a tap, never from the controller's `navigate` hook (Task 14). (ledger: final review controller#2)
- Step 1's `download_blocked` cannot see `[dl] blocked in captive mode`: the log filter drops every `[dl]` line and stderr never reaches the journal. Log it under a kept tag, e.g. `[captive] download blocked`. (ledger: R13)
- Read Step 1's other app lines (`[captive] settings …`, `[captive] blocked navigation: …`) through Task 13's log helper, never from the journal. (ledger: final review spec-alignment#2)

**Files:**
- Modify: `engine/wpeqt/main.cpp`. Every change is guarded by `g_captive`.

  | Location | Change |
  |---|---|
  | `start()` 322-333 | Skip the profile mkdir and the six loads. Set `m_settings = rmweb::Settings{}`, then `block = false`, `bwFast = false`, `settleFlash = false`, `autoRefreshSec = 0`. |
  | View construction 368-369 | Pass `"network-session", ns` with `ns = webkit_network_session_new_ephemeral()`, and unref `ns` after construction. |
  | After the view is created | Set WebKitSettings `enable-webgl`, `enable-webaudio`, `enable-media`, `enable-mediasource`, `enable-webrtc` and `enable-developer-extras` to FALSE. Log `[captive] settings webgl=0 webaudio=0 media=0 mse=0 webrtc=0 devextras=0`, reading each value back from the settings object. |
  | UA block 642-660 | Skip (WPE's default UA). |
  | `download-started` 688-689 | Connect a handler that calls `webkit_download_cancel(dl)` and logs `[dl] blocked in captive mode`. |
  | Downloads sweep 692-711 | Skip. |
  | Persistent cookies 716-727 | Skip. |
  | Content filter 733-739 | Skip, and call `loadInitial()` directly. |
  | Response decision 406-415 | When the MIME type is unsupported or PDF: `webkit_policy_decision_ignore` instead of a download. |
  | Navigation branch | Allow only `http:`, `https:`, `about:blank` and `rmweb:tls-continue`. Everything else gets `webkit_policy_decision_ignore` and logs `[captive] blocked navigation: <scheme>`. |
  | Auto-refresh guard 611 | Add `!g_captive &&` to the guard condition. |
  | `flushSync` 769, `queueSave` 1468, `flushPendingWrites` 1494 | Return immediately. |
  | FINISHED branch 1765-1768 | Skip `addHistory` and `upsertTab`; skip `checkReaderable`. |
  | `onFieldSet` 1087 | Skip `upsertPassword`. |
  | `learnFieldText` 1098 | Return immediately. |
  | Prefill 1303-1313 | `suggest` stays empty. |
  | `toggleReader` 1151, `applyReader` 1328, `checkReaderable` 1353 | Return immediately. |
- Create: `tools/device-checks/15-privacy.sh`

**Interfaces:**
- Consumes: Task 14's flow.
- Produces: no new interfaces. Upstream behaviour with `RMWEB_KIOSK` unset is unchanged.

- [ ] **Step 1: Write the failing device check** `15-privacy.sh`. This is acceptance test 6, plus the blocks.
  - `privacy`:
    1. `dev_ssh 'touch /run/marker'`.
    2. Run `get-token` with `--token T0K3N9` and ask the owner to submit it.
    3. Run `plain-form`, where the owner types name `Zed` and room `4711`.
    4. Run `terms-link`, where the owner checks the box.

    Expect three times `wait_exit 20 60`. Then `dev_ssh 'find /home -newer /run/marker -type f'` prints only `/home/root/.wifi-signin/log` and `log.1`. `dev_ssh 'grep -cE "T0K3N9|Zed|4711|://[^ ]*\\?" /home/root/.wifi-signin/log'` prints 0.
  - `rmweb_profile_untouched`: the `sha256sum` of every file under `/home/root/.rmweb` (if the directory exists) is the same before and after.
  - `hardening_logged`: the log contains `[captive] settings webgl=0 webaudio=0 media=0 mse=0 webrtc=0 devextras=0`.
  - `download_blocked`: on `terms-link`, `ask "Open terms, tap the download link"`. Expect `[dl] blocked in captive mode` and no new files in `/home/root/Downloads`.
  - `scheme_blocked`: `ask "Tap the settings link, then the file link"`. Expect `[captive] blocked navigation: rmweb` and `[captive] blocked navigation: file`.
- [ ] **Step 2: Run the check and confirm it fails.** Expected: `FAIL hardening_logged` and `FAIL download_blocked`.
- [ ] **Step 3: Implement the table above.**
- [ ] **Step 4: Build, deploy and run the check.** Expected: `5/5 checks passed`.
- [ ] **Step 5: Confirm non-captive mode is unchanged.**
  1. Unpack `build/release/rmweb-0.9.7` to `/home/root/phase0-rmweb` and replace its `bin/rmweb-wpeqt` with `build/wifi-signin-wpeqt`.
  2. Over SSH, run `RMWEB_ROOT=/home/root/phase0-rmweb systemd-run --unit=phase0-rmweb /home/root/phase0-rmweb/rmweb about:blank`.

  Expected: the rmweb start page and the full rmweb bar appear, and ⏻ restores xochitl. Remove `/home/root/phase0-rmweb` afterwards.
- [ ] **Step 6: Commit.** Message: `feat(captive): no persistence, ephemeral session, hardening`.

### Task 16: Taps, popups, submit trigger, certificate hint

**Carry-forward from Part A (2026-10-25):**
- Backdate the self-signed leaf like the CA-signed ones (`openssl ca -selfsign -startdate`): with a tablet clock slightly behind, `tls-portal` sees NOT_ACTIVATED and shows the clock hint wrongly. (ledger: Task 2 POINTER for Task 16)
- Before the TLS checks, make `certs.py` write a random 63-bit serial (`format(secrets.randbits(63), '016x')`): the CA persists across runs, while every leaf restarts at serial 01. (ledger: Task 2 POINTER for Task 16)
- If the fake portal is touched: `tls-*` variants exit 1 without OpenSSL 3 (today: `ready` with dead TLS ports), catch `ValueError` from a bad `--san`, answer a non-latin-1 `/raw?location=` with 400. (ledger: final review deferred-triage#24)

**Files:**
- Create: `engine/wpeqt/captive/tap_js.h`
- Modify: `engine/wpeqt/main.cpp`

  | Location | Change |
  |---|---|
  | `probeWith` 940-1002 | When `g_captive`, the in-page `probe(e)` function comes from `captiveProbeJs()`. The rest of the probe script is unchanged. |
  | `onTapLink` 1270 | When `g_captive`, `TapHit::Link` and `TapHit::Tick` call `m_ctl->onSubmitLike()`, and `TapHit::None` does nothing. |
  | `setFieldText` 1024 | A committed edit (keyboard Go) calls `m_ctl->onSubmitLike()` when `g_captive`. |
  | `start()` | When `g_captive`, connect `create` to `onCreate` and `submit-form` to `onSubmitForm`. |
  | `onTlsError` 1820 | Name the `GTlsCertificateFlags` parameter and store it in `m_tlsFlags`. |
  | `buildErrorPage` 1899 | Takes a fourth parameter, `bool clockHint`. When it is true, the page adds a paragraph with `The tablet clock may be wrong.` The caller passes `g_captive && (m_tlsFlags & (G_TLS_CERTIFICATE_NOT_ACTIVATED \| G_TLS_CERTIFICATE_EXPIRED))`. |
- Create: `tools/device-checks/16-interaction.sh`

**Interfaces:**
- Produces:
  - `std::string captiveProbeJs()` (namespace `rmweb::captive`) returns the JS source of a `function probe(e){…}` with the same contract as upstream's: it returns `'link'`, `'tick\n…'`, `'field\n…'` or `null`. The differences:
    - anchors are activated with `a.click()`, never `location.href = a.href`;
    - the matched selector is `a[href],area[href],[role=link],[role=button],[role=checkbox],[tabindex],summary,label,button`;
    - when nothing matches, it dispatches `pointerdown`, `mousedown`, `pointerup`, `mouseup` and `click` (bubbling, at the tap point) to `document.elementFromPoint(x, y)` and returns `'link'` if that element is not `body` or `html`, otherwise `null`.
  - `static WebKitWebView* onCreate(WebKitWebView*, WebKitNavigationAction*, gpointer)`:
    - if the request URI is empty or `about:blank`, it logs `[captive] popup ignored` and returns NULL;
    - otherwise it calls `webkit_web_view_load_request(m_view, webkit_navigation_action_get_request(action))`, which keeps the method and the POST body, and returns NULL.
  - `static gboolean onSubmitForm(WebKitWebView*, WebKitFormSubmissionRequest*, gpointer)` calls `m_ctl->onSubmitLike()`, then `webkit_form_submission_request_submit(req)` (the submission does not proceed without it), and returns TRUE.

- [ ] **Step 1: Write the failing device check** `16-interaction.sh`. Each variant below must reach `wait_exit 20 90` after the owner completes the visible sign-in step, which the script names with `ask`:
  - `js-checkbox`, `fetch-dom`, `delayed-grant`, `meta-refresh`;
  - `cross-origin`, `relative-location`, `status-511`, `status-403`;
  - `target-blank-post`, `window-open`, `anchor-hash`, `div-role-button`;
  - `terms-link` (the owner opens terms, taps Back, then accepts);
  - `tls-portal` (the owner taps Continue anyway);
  - `tls-two-hosts` (the owner accepts host A, then follows the link to host B).

  Add `clock_hint`: run `portal_start tls-portal --portal-cert expired`. The certificate page shows `The tablet clock may be wrong.` The owner confirms it, then taps Cancel, and `wait_exit 21 30` follows.
- [ ] **Step 2: Run the check and confirm it fails.** Expected failures: `fetch-dom` (no automatic exit is reached in time without the submit trigger), `anchor-hash`, `div-role-button`, `target-blank-post` and `clock_hint`.
- [ ] **Step 3: Implement `tap_js.h` and the main.cpp changes.**
- [ ] **Step 4: Build, deploy and run the check.** Expected: `16/16 checks passed`. Host tests still pass.
- [ ] **Step 5 (optional): Per-host certificate experiment** (spec §12, decision 5). On a throwaway branch:
  1. Replace the `tls-continue` handler with `webkit_network_session_allow_tls_certificate_for_host(session, cert, host)` followed by a reload.
  2. Run `tls-portal` and record in `docs/wifi-signin/phase0-paper-pro.md` whether the reloaded page paints.
  3. Delete the branch.

  The result decides v2 only.
- [ ] **Step 6: Commit.** Message: `feat(captive): JS-aware taps, popups, submit trigger, clock hint`.

---

## Part D: Packaging, acceptance and the other devices

### Task 17: Vellum packages and CI build

**Carry-forward from Part A (2026-10-25):**
- Before step 3's first push, the owner sets `git config user.email` (e.g. the GitHub noreply address) and decides whether to rewrite the authors: every commit so far is `edgarrodriguez@Edgars-Mac-mini.local`. (ledger: Task 1 minor (deferred))
- Step 3's push also triggers `tests.yml` (now on `wifi-signin` pushes and `wifi-signin-v*` tags): that first CI run must be green before step 4. (ledger: final review tests-ci#2)
- That run must exercise, not skip: OpenSSL 3.0 cert generation (R8), the launcher test under dash as /bin/sh, and `probe_runner_test.sh` on libsoup 3.4 with GnuTLS; under CI a skip now fails. (ledger: Task 2 and Task 7 'resolved by controller')
- `entry_no_systemd_run_exits_13` must pass there with `WS_SYSTEMD_RUN` set to a missing path: no case may reach the host's real `systemd-run`, `systemctl` or `mount`. (ledger: R30)
- If Task 10 chose a 3.29 SDK, `image()` uses the matching `eeems/remarkable-toolchain` tag, not `5.8.203-aarch64`. (ledger: R6)

**Files:**
- Create: `packaging/vellum/rmweb-runtime/VELBUILD`, `packaging/vellum/wifi-signin/VELBUILD`, `.github/workflows/vellum-package.yml`, `docs/wifi-signin/README.md`

**Interfaces:**
- Consumes: Task 9's device files, Task 10's `third_party/appsdk`, and Task 12's icon.
- Produces: two aarch64 apks and `vellum-dev.rsa.pub`, as workflow artifacts.

**rmweb-runtime VELBUILD** (spec §7):

```sh
maintainer="<owner name> <email>"   # the owner fills this in
pkgname=rmweb-runtime
pkgver=0.9.7
pkgrel=0
upstream_author="exp78"
category="framework"
pkgdesc="WPE WebKit engine runtime from rmweb 0.9.7, used by Wi-Fi Sign-in"
url="https://github.com/exp78/rmweb"
arch="aarch64"
license="MIT AND LGPL-2.1-or-later AND LGPL-2.0-or-later AND BSD-2-Clause AND BSD-3-Clause AND Apache-2.0 WITH LLVM-exception"
depends="remarkable-os>=3.28 remarkable-os<3.30 !rm1 !rm2"
options="!tracedeps !strip"
source="
https://github.com/exp78/rmweb/releases/download/v$pkgver/rmweb-$pkgver.tar.gz
rmweb-LICENSE::https://raw.githubusercontent.com/exp78/rmweb/v$pkgver/LICENSE
rmweb-NOTICE::https://raw.githubusercontent.com/exp78/rmweb/v$pkgver/NOTICE
"
```

- `unpack()` extracts the tarball into `$srcdir/rmweb`.
- `package()` copies exactly `lib`, `libexec`, `share` and `rmweb-env.sh` into `$pkgdir/home/root/.vellum/share/rmweb-runtime/`. It installs `rmweb-LICENSE` and `rmweb-NOTICE` as `LICENSE` and `NOTICE` under `$pkgdir/home/root/.vellum/licenses/$pkgname/`, and writes `SOURCES` there with the URLs and versions named in spec §7.
- `preupgrade()` and `predeinstall()` run the spec §7 `systemctl stop wifi-signin` snippet.

**wifi-signin VELBUILD:**
- `pkgname=wifi-signin`, `pkgver=0.1.0_rc1`, `category="apps"`, `upstream_author="edgarrodriguez-git"`, `license="MIT"`, `url="https://github.com/edgarrodriguez-git/rmweb"`, `arch="aarch64"`.
- `pkgdesc="Sign in to hotel, airport and cafe Wi-Fi pages on the tablet. Uses an unsandboxed web engine; see README."`
- `depends="rmweb-runtime>=0.9.7 rmweb-runtime<0.9.8 launcher remarkable-os>=3.28 remarkable-os<3.30 !rm1 !rm2 !rmppm !rmppure"`.
- `source`: the fork tag archive as `wifi-signin-$pkgver.tar.gz::https://github.com/edgarrodriguez-git/rmweb/archive/refs/tags/wifi-signin-v$pkgver.tar.gz`, plus the upstream `rmweb-0.9.7.tar.gz` for linking.
- `image()` echoes `eeems/remarkable-toolchain:5.8.203-aarch64`.
- `build()`:
  1. Source the first `environment-setup-*` under `/opt/codex/ferrari`, as `packages/highlight-reflow` does.
  2. Copy `third_party/appsdk/usr/.` into `$SDKTARGETSYSROOT/usr/`.
  3. Copy `libWPEWebKit-2.0`, `libWPEPlatform-2.0`, `libsoup-3.0` and `libwpe-1.0` from the extracted runtime into `$SDKTARGETSYSROOT/usr/lib/` without overwriting anything.
  4. Run `cmake -S engine/wpeqt -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXE_LINKER_FLAGS=-Wl,-rpath-link,<runtime>/lib`, using `-G Ninja` if `ninja` exists, otherwise Unix Makefiles, then `cmake --build build`.
- `package()` installs the following into `$pkgdir/home/root/xovi/exthome/appload/wifi-signin/`: `build/rmweb-wpeqt` as `bin/wifi-signin-wpeqt`; `lib.sh`, `appload-entry.sh`, `launch` and `cleanup` (mode 755); `external.manifest.json` and `icon.png`. It installs `LICENSE` and `NOTICE` under `licenses/wifi-signin/`.
- Hooks:
  - `postinstall()` echoes `Refresh AppLoad to see Wi-Fi Sign-in`;
  - `preupgrade()` and `predeinstall()` run the stop snippet;
  - `postdeinstall()` removes `/home/root/.wifi-signin` when `VELLUM_PURGE=1`.
- **Deviation from spec §7, recorded here.** The build uses the vendored `third_party/appsdk` headers, not `wpewebkit-2.48.5.tar.xz` plus header generation. The generator needs Debian's apt and python3 in its container, and the toolchain image cannot provide them. The vendored headers are generated from that same tarball by the same script (Task 10), and their provenance is in `third_party/appsdk/README.md`.

**Workflow** `vellum-package.yml`. It runs on `push` of tags `wifi-signin-v*` and on `workflow_dispatch`, on `ubuntu-latest`. Its steps:
1. Check out the fork, and `vellum-dev/vellum` into `vellum/`.
2. `uses: ./vellum/.github/actions/install-vbuild`.
3. Copy `packaging/vellum/*` into `vellum/packages/`.
4. In `vellum/`, run `./scripts/update-checksums.sh rmweb-runtime wifi-signin`, then `./scripts/build-package.sh rmweb-runtime aarch64` and `./scripts/build-package.sh wifi-signin aarch64`.
5. Upload `vellum/dist/aarch64/*.apk`, `vellum/keys/vellum-dev.rsa.pub` and the two checksummed VELBUILDs as artifacts.

- [ ] **Step 1: Write `docs/wifi-signin/README.md`.** It explains what the app does, how to install it with reManager or `vellum add`, how to use it (triple-press, tap the icon), and the limits (OAuth portals, non-ASCII text, long selects). It also carries a "Security" paragraph: the WebProcess runs unsandboxed as root on WPE 2.48.5, which predates later WebKit security fixes, and captive mode loads content from untrusted networks (spec §11, risk 1).
- [ ] **Step 2: Write both VELBUILDs and the workflow.**
- [ ] **Step 3: Tag and build.** With the owner's go-ahead, push the branch and the tag `wifi-signin-v0.1.0_rc1`. Expected: the workflow succeeds, with two `.apk` artifacts and the public key. Commit the checksummed VELBUILDs from the artifact back into `packaging/vellum/`.
- [ ] **Step 4: Install on the Paper Pro through Vellum.**
  1. `dev_ssh 'rm -rf /home/root/xovi/exthome/appload/wifi-signin /home/root/.vellum/share/rmweb-runtime'` (this removes the dev deployment).
  2. Copy the key to `/home/root/.vellum/etc/apk/keys/vellum-dev.rsa.pub`.
  3. Run `vellum add ./rmweb-runtime-0.9.7-r0.apk ./wifi-signin-0.1.0_rc1-r0.apk` in reManager's terminal.

  Expected: the install prints `Refresh AppLoad to see Wi-Fi Sign-in`, and both packages appear under reManager's Installed list. Then run `bash tools/device-checks/14-flow.sh`; expected `9/9 checks passed`.
- [ ] **Step 5: Check the lifecycle hooks.**
  1. Start a session, then run `vellum del wifi-signin` over SSH. Expected: `Closing Wi-Fi Sign-in first`, then `xochitl_back_within 10`, and `/home/root/.wifi-signin` still exists.
  2. Reinstall, then run `vellum purge wifi-signin`. Expected: `/home/root/.wifi-signin` is gone.
  3. Reinstall once more.
- [ ] **Step 6: Commit.** Message: `build(vellum): rmweb-runtime and wifi-signin packages`.

### Task 18: Acceptance on the Paper Pro

**Carry-forward from Part A (2026-10-25):**
- Test 7's triple press within 10 s after exit comes after cleanup restarted xovi-tripletap, so it runs `xovi/start` on a xochitl under 30 s old (a crash hazard): agree the expected result with the owner first. (ledger: final review launcher-safety declinedToJudge#6)
- Test 6 reuses `15-privacy.sh` with its `login-password` run: the typed username and password appear neither in the log nor in `/home/root/.rmweb/passwords.txt`. (ledger: final review spec-alignment#1)
- Test 4: during a session `/sys/power/wake_lock` lists `wifi-signin`; 40 s after each exit neither `wifi-signin` nor `wifi-signin-exit` (`grep -w wifi-signin` matches both). Record R27 as a spec §6 erratum. (ledger: R27)
- Include Task 14's `clock_wrong` case and record its result on the device's libsoup 3.6 with OpenSSL. (ledger: Task 7 'resolved by controller')
- Record the OS version (`os=` from device-facts) with the results: the Paper Pro runs 3.29, while spec §2 names 3.28.x. (ledger: R6)
- Record each test-1 run's probe-cycle count from that session's log lines, not the journal. (ledger: final review deferred-triage#14)

**Files:**
- Create: `tools/device-checks/acceptance.sh`, `docs/wifi-signin/acceptance-paper-pro.md`

- [ ] **Step 1: Write `acceptance.sh`.** It runs spec §2 tests 1 and 3 to 7 against the Vellum-installed packages, with these methods:
  - **Test 1:** every variant from Task 16 plus `plain-form`, `room-name`, `get-token` and `delayed-grant`. Each must reach exit 20 automatically. Time each run from `session_start`, with xochitl active for more than 30 s first, until xochitl is active again. The limit is 60 s.
  - **Test 3:** 20 launches 35 s apart, cycling through the exit paths 20, 21 (Cancel), 21 (power), 24 and 22. For exit 22, the override sets `idle_timeout_s = 20`. Each must have xochitl active within 10 s of the journal's exit line, with an unchanged `boot_id`.
  - **Test 4:** acceptance test 4 exactly. Run `kill -9 $(pidof wifi-signin-wpeqt)` and `systemctl kill -s KILL --kill-whom=main wifi-signin`, 5 times each, with every listed check.
  - **Test 5:** read `/sys/fs/cgroup/system.slice/wifi-signin.service/memory.peak` (use the path that `systemctl show -p ControlGroup wifi-signin` gives) on the `plain-form` portal page, along with `memory.swap.current` and `MemAvailable`. The limits are under 600 MB and above 300 MB.
  - **Test 6:** reuse `15-privacy.sh`.
  - **Test 7:** single press, triple press during a session, and triple press within 10 s after exit.

  The `delayed-grant` and `fetch-dom` runs are timed separately and reported. The 60 s limit applies to them too.
- [ ] **Step 2: Run it and record the results.** Record every result in `acceptance-paper-pro.md` as a table with columns test, result, time or value, and notes. Any failure is fixed in the owning task, with its check script re-run, before going on.
- [ ] **Step 3: Test 2, a real portal.** At a hotel, airport or cafe, or with an openNDS travel router on stock defaults if no public portal is available, remove the override file and complete a sign-in. Record vendor, result and time. Check that reMarkable cloud sync then works (Settings, then Storage).
- [ ] **Step 4: Commit.** Message: `docs: Paper Pro acceptance results`.

### Task 19: Paper Pro Move

**Prerequisite:** the Move is on 3.28.x, automatic updates are off, Developer Mode is on, and the mod stack from spec §3 is installed through reManager.

**Files:**
- Modify: `packaging/vellum/wifi-signin/VELBUILD` (remove `!rmppm`; `pkgrel` +1)
- Create: `docs/wifi-signin/acceptance-paper-pro-move.md`

- [ ] **Step 1: Device facts.** Run `device-facts.sh` on the Move. Expected: as on the Paper Pro, except that `power_key_nodes` includes `44440000.bbnsm:pwrkey key116=1`. Record the output.
- [ ] **Step 2: Install a test build.** On a branch, remove `!rmppm` and bump `pkgrel`, then run the workflow with `workflow_dispatch` on that branch. Install its apks on the Move as in Task 17 step 4, then run `13-foundation.sh` and `14-flow.sh` with `DEV_HOST` pointing at the Move.
- [ ] **Step 3: Check the layout.** The owner confirms that all four bar buttons, the host label and every start-up screen fit in 954 px, and that taps land on what they show. Fix any problem in Task 14's layout code, and re-run Task 14's check on the Paper Pro too.
- [ ] **Step 4: Run acceptance tests 1, 3, 4 and 7** with `acceptance.sh --only 1,3,4,7` (add that option). Record the results.
- [ ] **Step 5: Enable the Move in the package.** Merge the test branch's VELBUILD change. Commit message: `feat: enable Paper Pro Move`.

### Task 20: Paper Pure

**Prerequisite:** the same as Task 19, for the Pure.

**Files:**
- Modify: `packaging/vellum/wifi-signin/VELBUILD` (remove `!rmppure`; `pkgrel` +1)
- Create: `docs/wifi-signin/acceptance-paper-pure.md`

- [ ] **Step 1: Device facts.** Expected `power_key_nodes` includes `44440000.bbnsm:pwrkey key116=1`.
- [ ] **Step 2: Official epaper hello-world.**
  1. Build `hello_remarkable` from `github.com/reMarkable/remarkable-developer-examples` in the `rmweb-sdk` container.
  2. Run it on the Pure as `systemd-run --unit=hello -p ExecStopPost=-/bin/systemctl start xochitl sh -c 'systemctl stop xochitl; sleep 2; QT_QUICK_BACKEND=epaper ./hello_remarkable -platform epaper'`.

  Expected: it renders. If it does not, stop: the Pure is out of scope until upstream support exists. Record that and finish here.
- [ ] **Step 3: Upstream rmweb.** Run it as in Task 11 step 3. Expected: it renders on the monochrome 1872x1404 panel, and taps land correctly (touch 1776x2400).
- [ ] **Step 4: This app.** Install a test build without `!rmppure`, as in Task 19 step 2. Run `13-foundation.sh` and `14-flow.sh`, then `acceptance.sh --only 1,3,4,7`, and record the results.
- [ ] **Step 5: Enable the Pure in the package.** Merge the test branch's VELBUILD change. Commit message: `feat: enable Paper Pure`.

### Task 21: Vellum submission (owner only)

Agents do not perform this task. Vellum requires that issues, PRs, their text and their commits come from a person.

- [ ] **Step 1: Ask the maintainers** on the vellum-dev Discord or in an issue, in the owner's own words. Ask the two spec §7 questions together: whether `rmweb-runtime` may repackage upstream's own v0.9.7 release asset (which includes Debian-built libLLVM-16 and its helpers), and whether a 107 MB package is acceptable.
- [ ] **Step 2: If yes,** open one PR for `rmweb-runtime`, and then one for `wifi-signin`. Review and rewrite the VELBUILDs as needed, and follow Vellum's contribution rules, including any rule about how the work was produced. After a maintainer runs `/publish-testing`, test with `vellum testing enable && vellum update && vellum add wifi-signin@testing` on each enabled device, then comment `/ready-for-review`.
- [ ] **Step 3: If no,** pick a fallback from spec §7: either build the engine inside vbuild, or publish both apks and the public key from the fork's GitHub releases and point the README at them.
