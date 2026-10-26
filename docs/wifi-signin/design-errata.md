# Wi-Fi Sign-in design: proposed errata

Status: proposed, pending the owner's approval. `design.md` (revision 2) stays unchanged until the owner approves an entry. An approved entry then goes into a new revision of `design.md`, and this file records that.

This file lists the places where the Part A code deliberately differs from the spec's literal text, plus two slips in the spec itself (E9, E22). The code was checked at `int/part-a` 571d532, which is `wifi-signin` with the final fix wave applied. Line numbers are those of `docs/wifi-signin/design.md` revision 2. The ruling IDs (R…) refer to the plan's SDD ledger, which is kept out of git, so each "Why" stands on its own.

E1 to E6 are the corrections the final review asked for. E7 to E22 are the remaining deliberate differences, in spec order.

| # | Where | Topic | Ruling |
|---|---|---|---|
| E1 | §6 step 8.4, line 318 | The exit wakelock gets its own name | R27, R34 |
| E2 | §6 launch intro and step 2, lines 291, 294 | State tokens are recorded before their action | R24, R25 |
| E3 | §6 step 4, line 300 | xochitl's age is computed in centiseconds | fix wave (Task 9 M5) |
| E4 | §9 Privacy, line 465 | The privacy fixture includes a login form with a password | fix wave (R26) |
| E5 | §5 Probe, lines 178, 201 | A non-http(s) Location counts as absent | R28 |
| E6 | §5 Logging, line 267 | URL redaction: port, tail and opaque URLs | R12, R14, R29, R35 |
| E7 | §5 Logging, lines 268-269 | More line tags are dropped | R11, R13 |
| E8 | §6 step 8.5, line 323 | When cleanup drops the 15-minute lock | R27 |
| E9 | §4 diagram, line 114 | The exit wakelock comes before xochitl's start | spec slip |
| E10 | §5 exit codes, lines 255-256 | What exits 11 and 12 cover | R25 |
| E11 | §6 intro, line 275 | The runtime's variables go to the app only | R25 |
| E12 | §6 appload-entry, line 285 | ExecStopPost names the cleanup mode | R22 |
| E13 | §6 step 1, line 293 | A stale lock is cleaned up before the takeover | R25 |
| E14 | §6 steps 3 and 5, lines 295, 305 | Refuse when tripletap or xochitl survives its stop | R25, fix wave (Task 9 M2) |
| E15 | §6 step 4, line 302 | The guard gives up after 180 s | R25 |
| E16 | §6 step 7, line 311 | Which exit codes the start-up retry skips | R23 |
| E17 | §6 step 8.1, line 313 | Cleanup's two modes | R22 |
| E18 | §6 step 8.2, line 314 | One deadline for the stop step, then kill -9 | R25, Task 9 fix round 1 |
| E19 | §6 step 8.6, line 324 | Tripletap restarts even if xochitl does not | R25 |
| E20 | §5 Probe, line 181 | The confirmation is sent only when it can change the result | R15, R16 |
| E21 | §5 Start-up, line 201 | When the Location fallback fires | R18, R21, R28 |
| E22 | §9 Launcher tests, line 468 | The launcher test's file name | spec slip |

## Corrections the final review asked for

### E1. §6 step 8.4: the exit wakelock gets its own name

- **Where:** §6 "launch", step 8.4, line 318. Related: §2 test 4 (line 34) and E8.
- **The spec says:** "rewrite the wakelock as `wifi-signin 30000000000`, a 30 s timed lock that the kernel drops on its own;"
- **Proposed:** "take a second timed lock, `wifi-signin-exit 30000000000` (30 s, which the kernel drops on its own), and only then write `wifi-signin` to `/sys/power/wake_unlock`. If the 30 s lock cannot be taken, log it and keep the 15-minute lock, which expires on its own;"
- **Ruling:** R27 for the mechanism. R34 accepts keeping the 15-minute lock when the exit lock cannot be taken.
- **Why:**
  - The kernel only ever extends a timed wakelock: a write with a shorter timeout re-arms the timer only when the new expiry is later. The spec's rewrite therefore leaves the 15-minute lock in place, and the tablet cannot suspend for up to 15 minutes after a session.
  - A second name gives the 30 s bound that the spec intends. Taking it before the unlock means autosleep never gets a gap while xochitl starts.
  - xochitl's start must never run without a wakelock, so when the exit lock cannot be taken, the 30 s bound gives way.
  - Test 4 still holds as written. `grep -w wifi-signin` matches both names, and an expired timed lock is no longer listed. Only in the R34 case (a failed sysfs write) would its 40 s check still find the 15-minute lock.
  - Plan Task 11 checks the extend-only behaviour on the device (`wake_lock_extend_only`).

### E2. §6 launch and step 2: state tokens are recorded before their action

- **Where:** §6 "launch", the intro (line 291) and step 2 (line 294). Steps 3 (line 295), 5 (line 305) and 6 (line 308) follow the same order.
- **The spec says:**
  - line 291: "Each step appends a token to `/run/wifi-signin/state` as soon as its action completes, and cleanup undoes only what is recorded."
  - line 294: "Write `wifi-signin 900000000000` (a 15-minute timed lock) to `/sys/power/wake_lock` and record `WAKELOCK`. If the write fails, log it and exit without touching xochitl."
- **Proposed:**
  - line 291: "Each step appends its token to `/run/wifi-signin/state` just before its action, and cleanup undoes only what is recorded, re-checking the real state before each undo. A token that cannot be recorded stops the launch with exit 12 before its action. An app whose PID cannot be recorded is stopped before launch exits."
  - line 294: "Record `WAKELOCK`, then write `wifi-signin 900000000000` (a 15-minute timed lock) to `/sys/power/wake_lock`. If either fails, log it and exit 12 without touching tripletap or xochitl."
  - Steps 3, 5 and 6 likewise say "record X, then" before their action.
- **Ruling:** R24 for the order. R25, and the final fix wave's M3 fix, for refusing with exit 12 when a token cannot be written. R34(a) for the unrecorded app.
- **Why:**
  - sh runs a trap only when the foreground command returns. A TERM or SIGKILL during `systemctl stop xochitl` would otherwise leave no `STOPPED_XOCHITL` token, and nothing would start xochitl again.
  - Recording early is safe because every undo re-checks the real state. xochitl is started only if it is not active, the overlay is unmounted only if `/proc/mounts` shows ours, and tripletap's start is idempotent.
  - A full or broken `/run` must not let a step run unrecorded.
  - The one cost is cosmetic, and R34 accepts it: when the wakelock write itself fails, cleanup's unlock of a lock that was never taken logs a spurious WARN.

### E3. §6 step 4: xochitl's age is computed in centiseconds

- **Where:** §6 "launch", step 4, line 300.
- **The spec says:** "age = now − start ÷ 100."
- **Proposed:** "age = (now − start) ÷ 100, rounded down once. now is the first field of `/proc/uptime` in centiseconds (it has two decimals), and start is in ticks, which are centiseconds (USER_HZ = 100)."
- **Ruling:** Task 9 minor M5, fixed in the final fix wave (R26).
- **Why:** Read literally, the formula takes now in whole seconds and floors start ÷ 100 separately. Two floors can call a true age of 29.99 s "30" (now 130.50 s gives 130, and start 10051 ticks gives 100). The guard would then stop a xochitl younger than 30 s, which is the crash §3 warns about. One floor of the centisecond difference never lets a true age under 30 s pass.

### E4. §9 Privacy: the fixture includes a login form with a password

- **Where:** §9 "Fake portal", the Privacy bullet, line 465. Related: §2 test 6 (line 36).
- **The spec says:** "**Privacy.** A GET form carrying a unique token, for the privacy test."
- **Proposed:** "**Privacy.** A login form with a username field and a real `type="password"` field (`login-password`), and a GET form carrying a unique token (`get-token`), for the privacy test."
- **Ruling:** the final fix wave (R26), from the confirmed finding "fake portal lacks a password form (spec test 6)".
- **Why:** Test 6 submits "a login, a password, a surname and a GET form carrying a unique token". §9's fixture list named only the token form, so the fixtures could not run test 6. rmweb's password store reacts only to a `type="password"` input, so the form needs a real one to prove that captive mode stores nothing.

### E5. §5 Probe: a non-http(s) Location counts as absent

- **Where:** §5 "Probe", the captive row of the classification table, line 178. Related: §5 "Start-up", the captive row, line 201 (E21).
- **The spec says:** "Portal URL: the `Location` header resolved against the probe URL, otherwise the probe URL itself"
- **Proposed:** "Portal URL: the `Location` header resolved against the probe URL, when that gives an `http` or `https` URL. Otherwise, with no Location or a Location of any other scheme (`javascript:`, `data:`, `file:`, `rmweb:`, …), it is the probe URL itself."
- **Ruling:** R28.
- **Why:** §5 "Hardening" allows navigation only to `http`, `https` and `rmweb:tls-continue`. A portal-chosen `rmweb:` or `javascript:` URL must never reach the view's navigation, and the start-up fallback would load it. The cost is that a portal redirecting to a non-web scheme is shown through the probe URL instead. No real portal does that.

### E6. §5 Logging: URL redaction keeps the port and also covers opaque URLs

- **Where:** §5 "Override file and logging", line 267.
- **The spec says:** "any URL is cut down to `scheme://host`;"
- **Proposed:** "any `scheme://` URL is cut down to `scheme://host[:port]`: userinfo is removed, and so is everything after the authority up to the next whitespace. An opaque `data:`, `javascript:`, `vbscript:`, `mailto:`, `blob:`, `tel:` or `sms:` URL is cut down to `scheme:`. For the first three, the rest of the line goes too;"
- **Ruling:** R29 for opaque URLs. R35 for `data:`, `javascript:` and `vbscript:` running to the end of the line, and for a filter that is linear in the line length. R12 and R14 for the port and for the tail ending at whitespace.
- **Why:**
  - "Any URL" includes opaque URLs, which have no `//` and so no host. `mailto:guest@hotel.example` or a `data:text/html,…` page would otherwise reach the log whole.
  - WebKit leaves raw spaces in `data:`, `javascript:` and `vbscript:` payloads, so stopping at whitespace would still log page content. Address schemes stop at whitespace, like a `scheme://` tail.
  - A `scheme://` tail runs to whitespace because WebKit keeps `)`, `]` and `'` raw in URLs (`/wiki/Foo_(bar)?token=…`). Stopping at one of them would leave the rest of the URL in the log. One closer is kept when the URL sits inside its opener, so prose stays readable.
  - The port carries no user data, and it tells the test listeners apart.
  - The cost: text glued to a `data:` URL on the same line is lost, and a prose word such as `data:` followed directly by text loses that text.

## Other deliberate differences

### E7. §5 Logging: more line tags are dropped

- **Where:** §5 "Override file and logging", lines 268-269.
- **The spec says:** "every `[form]` line is dropped;" and "the `setFieldText` result is never logged."
- **Proposed:** "every message that can carry page content is dropped whole. These are messages containing `[form]` or `setFieldText` (field values), `[meta]` (page titles), `[dl]` or `[library]` (server-supplied file names), `[find]` or `[search]` (typed search terms), and `[t] page JS done` (page-script answers that echo the DOM). A multi-line message goes whole if any of its lines matches."
- **Ruling:** R11 (whole messages) and R13 (the tag list). `[library]` was added in the final fix wave.
- **Why:** §5 says the log "never contains page content, form data or credentials", and each of these upstream tags carries page-controlled text. In return, captive mode must log its own download block under a kept tag (plan Task 15).

### E8. §6 step 8.5: when cleanup drops the 15-minute lock

- **Where:** §6 step 8.5, line 323.
- **The spec says:** "If `WAKELOCK` and xochitl was never stopped, write `wifi-signin` to `/sys/power/wake_unlock`."
- **Proposed:** "If `WAKELOCK` and step 4 did not hand over to the exit lock (xochitl was never stopped, or was already active again), write `wifi-signin` to `/sys/power/wake_unlock`."
- **Ruling:** part of R27's cleanup as reviewed.
- **Why:** If xochitl is already back, for example after a triple press, step 4 does nothing. The literal text would then keep the 15-minute lock for its full term.

### E9. §4 diagram: the exit wakelock comes before xochitl's start

- **Where:** §4 "Architecture", the cleanup line of the diagram, line 114.
- **The spec says:** "-> reset-failed + start xochitl -> timed wakelock -> start tripletap"
- **Proposed:** "-> exit wakelock, then unlock -> reset-failed + start xochitl -> start tripletap"
- **Ruling:** none; this is a slip in the spec, and the code follows step 8.4.
- **Why:** §6 step 8.4 takes the timed lock before the start, so the tablet cannot suspend while xochitl starts. The diagram contradicts it.

### E10. §5 exit codes: what 11 and 12 cover

- **Where:** §5 "Exit codes and how the app exits", lines 255-256.
- **The spec says:** "11 | already running" and "12 | display busy: xochitl was not active at stop time, or the panel was still held".
- **Proposed:**
  - "11 | already running, or the lock in `/run/wifi-signin` could not be taken or written"
  - "12 | refused before or while taking the display; the log line before it names the reason. The reasons are: xochitl not active at stop time, or still active after the stop; xovi-tripletap still active after its stop; the panel still held; xochitl not settling within 180 s; the wakelock or a state token that cannot be written; the WPE helpers that cannot be copied; and an overlay mount that fails"
- **Ruling:** R25 ("every non-lock refusal exits 12"), plus the final fix wave's M2 and M3 fixes.
- **Why:** Each new refusal closes a stranding or misfire path that the spec leaves open (E2, E14, E15). One code for "did not take the display" keeps the meaning simple, and the log says which case it was.

### E11. §6 intro: the runtime's variables go to the app only

- **Where:** §6 "Launcher", the intro, line 275.
- **The spec says:** "it sets its own environment: PATH, `HOME=/home/root`, and the Qt and WPE variables from `rmweb-env.sh` …"
- **Proposed:** "it sets its own PATH and `HOME=/home/root`. The Qt and WPE variables from `rmweb-env.sh` (with `R=…`) are exported to the app only, in the subshell that runs it."
- **Ruling:** R25.
- **Why:** `rmweb-env.sh` sets `LD_LIBRARY_PATH` to the runtime's libraries. In the launcher's own environment, they would be loaded by `systemctl`, `mount` and the other system tools it runs.

### E12. §6 appload-entry: ExecStopPost names the cleanup mode

- **Where:** §6 "appload-entry.sh", step 3, line 285.
- **The spec says:** "`-p ExecStopPost=-$APP/cleanup …`"
- **Proposed:** "`-p "ExecStopPost=-$APP/cleanup post" …`"
- **Ruling:** R22(a). The code also runs `"${WS_SYSTEMD_RUN:-systemd-run}"`, a test-only seam (R30) that the spec need not name.
- **Why:** Cleanup has to know whether it runs from launch's trap or from ExecStopPost (E17).

### E13. §6 step 1: a stale lock is cleaned up before the takeover

- **Where:** §6 "launch", step 1, line 293.
- **The spec says:** "If the PID is dead, the lock is stale; take it over."
- **Proposed:** "If the PID is dead, the lock is stale. Undo what that session recorded first (`cleanup post`), then take it over."
- **Ruling:** R25.
- **Why:** An owner killed before its cleanup finished can leave the overlay mounted or xochitl stopped. Starting afresh over that state would lose those tokens, and nothing would undo them.

### E14. §6 steps 3 and 5: refuse when tripletap or xochitl survives its stop

- **Where:** §6 "launch", step 3 (line 295) and step 5 (line 305).
- **The spec says:** step 3 "stop it and record `TRIPLETAP`"; step 5 "run `systemctl stop xochitl` and record `STOPPED_XOCHITL`".
- **Proposed:**
  - step 3: "… If it is still active after the stop, exit 12 without stopping xochitl."
  - step 5: "… If xochitl is still active after the stop, exit 12 without launching."
- **Ruling:** the final fix wave's M2 fix (tripletap) and R25 (xochitl).
- **Why:** A tripletap that survives its stop could restart xochitl over the app with a triple press. A xochitl that survives its stop is a second display owner, which §3 lists as a reboot hazard.

### E15. §6 step 4: the guard gives up after 180 s

- **Where:** §6 "launch", step 4, line 302.
- **The spec says:** "Loop with `sleep 1` until ActiveState is `active` and the age is at least 30 s. …"
- **Proposed:** add: "If that has not happened within 180 s on the boot clock (about four restarts), exit 12 without touching xochitl."
- **Ruling:** R25.
- **Why:** A crash-looping xochitl would otherwise keep the launcher waiting forever, with nothing on screen to explain why.

### E16. §6 step 7: which exit codes the start-up retry skips

- **Where:** §6 "launch", step 7, the start-up retry, line 311.
- **The spec says:** "If the app ends within 20 s of starting, with any non-zero code or a signal, … retry once. … Exit 63 is never retried."
- **Proposed:** "If the app ends within 20 s of starting, with an exit code other than 0, 20-24 or 63 (that is 1, 2, 3, any other code, or a signal), … retry once. Not after the start-up watchdog's kill either."
- **Ruling:** R23.
- **Why:** Codes 20-24 are captive mode's own deliberate verdicts. Upstream logs `[touch] EVIOCGRAB failed (attempt N/5 …)` even when a later grab succeeds, so an "already open" exit 20 after about 2 s would otherwise be retried. That costs an extra xochitl stop and start, and those count towards StartLimitBurst.

### E17. §6 step 8.1: cleanup's two modes

- **Where:** §6 "launch", step 8.1, line 313.
- **The spec says:** "If the PID in `/run/wifi-signin/pid` is alive and is not the `launch` process that invoked this cleanup, exit 0: the state belongs to another session."
- **Proposed:** cleanup runs in one of two modes, and otherwise exits 0:
  - "From launch's trap (`cleanup trap`), it acts only if `/run/wifi-signin/pid` holds the launch that invoked it. That launch also passes the app PID it holds in memory, in case the state file could not record it."
  - "From ExecStopPost, or from a launch taking over a stale lock (`cleanup post`), it acts only if the recorded launch is gone."
- **Ruling:** R22(a); the PID argument is the final fix wave's M3 fix.
- **Why:** An ExecStopPost run cannot tell "invoked by" from the PID alone. The mode makes both cases explicit: a live owner means "my own session" to the trap run and "not mine yet" to the ExecStopPost run.

### E18. §6 step 8.2: one deadline for the stop step, then kill -9

- **Where:** §6 "launch", step 8.2, line 314.
- **The spec says:** "Send TERM to the app if it is running and wait up to 3 s … Then `kill -9` any WPEWebProcess, WPENetworkProcess or WPEGPUProcess. Wait up to 10 s until no app or helper PID remains."
- **Proposed:** add: "If any remains, `kill -9` the app and the helpers as well. The whole step ends 20 s after it starts, whatever the processes do, and cleanup goes on."
- **Ruling:** R25 (the kill -9), and Task 9 fix round 1 (the single deadline).
- **Why:**
  - An app stuck in the e-ink driver can survive even SIGKILL.
  - systemd gives ExecStopPost a 30 s budget (TimeoutStopSec). A cleanup that systemd kills before step 4 would leave the tablet without xochitl.
  - The bounded step keeps xochitl's start inside that budget. On the host, the worst case starts xochitl about 22.6 s after cleanup begins, and plan Task 12 measures it on the device.

### E19. §6 step 8.6: tripletap restarts even if xochitl does not

- **Where:** §6 "launch", step 8.6, line 324.
- **The spec says:** "If `TRIPLETAP`, start xovi-tripletap again, after xochitl is verified active."
- **Proposed:** "If `TRIPLETAP`, start xovi-tripletap again, after xochitl's start has been verified or reported as failed."
- **Ruling:** R25.
- **Why:** If xochitl does not come back, a triple press (tripletap runs `xovi/start`) is the owner's way to restart it without SSH. Leaving tripletap stopped would remove that.

### E20. §5 Probe: the confirmation is sent only when it can change the result

- **Where:** §5 "Probe", line 181.
- **The spec says:** "A probe cycle sends G and A in parallel, then the HTTPS confirmation."
- **Proposed:** "A probe cycle sends G and A in parallel. Then, only when its answer can change the result (there is a default route, at least one probe is probe-ok and none is captive), it sends the HTTPS confirmation. A confirmation URL that is not `https://` is never sent, and counts as a failure (partial)."
- **Ruling:** R15(a) and R16.
- **Why:** The decision order ignores the confirmation in every other case, so the result is identical, and a captive network does not pay for a confirmation that can only time out. A confirmation must use verified TLS, so a mistyped `http://` override shows as partial instead of passing.

### E21. §5 Start-up: when the Location fallback fires

- **Where:** §5 "Start-up", the captive row, line 201.
- **The spec says:** "If that load ends on the probe host with an empty body or an error and the probe saw a Location, navigate to the resolved Location."
- **Proposed:** "If the first load to end after that navigation ends on exactly that probe URL (the view stayed there, with an empty page or an error page), and the probe saw an `http` or `https` Location, navigate to the resolved Location, once."
- **Ruling:** R18 (exactly the probe URL), R21 (the first load end only, once) and R28 (http or https only).
- **Why:** Same-origin portals, the fake portal included, serve their pages on the probe host and port. "Ends on the probe host" would reload such a portal needlessly. A later return to the probe URL, such as a portal sending the tablet back to it after the sign-in, must not trigger the fallback either. The controller cannot see the page body; a load end on exactly the probe URL is the observable case of an empty page or an error.

### E22. §9 Launcher tests: the test's file name

- **Where:** §9 "Testing", the launcher tests, line 468.
- **The spec says:** "**Launcher tests** (`tests/launcher_test.sh`, …"
- **Proposed:** "**Launcher tests** (`tests/wifi_signin_launcher_test.sh`, …"
- **Ruling:** none; this is a slip in the spec, and plan Task 9 names the new file.
- **Why:** `tests/launcher_test.sh` is upstream rmweb's test of its own `device/rmweb` launcher, and it stays unchanged so upstream merges stay clean.
