# Wi-Fi Sign-in for reMarkable: design spec

Revision 2, 2026-10-25. Status: approved by the owner on 2026-10-25, with Google's 204 endpoint as the HTTPS confirmation default.

Revision 2 applies an independent review of revision 1 against the rmweb source (master at v0.9.7), rmbrowser, quill and remagic's Chromium launcher, xovi-tripletap, AppLoad, Vellum, vbuild, apk-tools 3.0.3, AOSP NetworkStack, GNOME's portal helper, NetworkManager and the relevant RFCs. 56 findings were confirmed by a second, adversarial check. The largest changes are in the launcher (power key, wakelock, lock location, cleanup order), in what captive mode switches off in rmweb (profile, passwords, vendor display symbols), in the probe (classification, triggers, transport) and in packaging (built from source by Vellum, runtime in a private prefix).

## 1. Purpose

Let a reMarkable Paper Pro, Paper Pro Move or Paper Pure complete a captive-portal Wi-Fi sign-in on the device itself: the terms-acceptance or login page that hotel, airport and cafe networks show. reMarkable OS has no browser and officially does not support these networks. Its only advice is a phone hotspot.

The app only lets the device owner complete the network's own sign-in flow on the device. It does nothing to avoid or weaken that flow.

## 2. Goals, acceptance tests and non-goals

Goals for v1:

- On a captive network joined in the normal Wi-Fi settings, the owner launches "Wi-Fi Sign-in" and sees the network's portal page. They complete it with the on-screen keyboard, and the app returns to the stock UI on its own once the network is open.
- Works on JavaScript-driven portals as well as plain-HTML ones: Meraki click-through, Aruba, Fortinet and Mist credential forms, Cisco ISE, UniFi, and hotel room-and-name pages.
- Installable and removable through Vellum, so it can be offered in reManager once accepted upstream.
- Never leaves the tablet without its stock UI, and never triggers the device's emergency reboot.

Acceptance tests. These run on the Paper Pro on OS 3.28.x first, then on each device as it is enabled (section 8). "No reboot" always means `/proc/sys/kernel/random/boot_id` is unchanged.

1. **Fake-portal flows.** Every fake-portal variant in section 9 ends with exit 20 (online) without any manual exit. The clock starts at the icon tap, with xochitl active for more than 30 s, and stops when xochitl is active again. A scripted tester uses at most two taps, and the total is under 60 s.
2. **Real portal.** At least one real public portal completes. If none is available, a captive fixture running stock defaults (openNDS on a travel router) completes. Record vendor, result and time.
3. **Stability.** 20 launches spaced at least 35 s apart mix every exit path: online, Cancel, single power press, Use network as is, and idle timeout shortened through the override file. Each time xochitl is active again within 10 s of the app's exit, as timed from the journal. There is no reboot.
4. **Kill safety.** Run (a) `kill -9 $(pidof wifi-signin-wpeqt)` and (b) `systemctl kill -s KILL --kill-whom=main wifi-signin`, which kills only `launch`, so that ExecStopPost does the restore. Repeat each 5 times. Within 15 s of each:
   - xochitl is active;
   - `/proc/mounts` has no overlay on `/usr/libexec`;
   - `/run/wifi-signin` and `/tmp/epframebuffer.lock` are absent;
   - xovi-tripletap is active if it was before;
   - there is no reboot, and the next launch succeeds.

   40 s later, `grep -w wifi-signin /sys/power/wake_lock` finds nothing.
5. **Memory.** The service runs with `MemoryAccounting=yes`. On the named fake-portal page, the unit cgroup's `memory.peak` stays under 600 MB, and `memory.swap.current` is recorded. `MemAvailable` in `/proc/meminfo` stays above 300 MB.
6. **Privacy.** `touch /run/marker`, then run the fake-portal login flow, which submits a login, a password, a surname and a GET form carrying a unique token.
   - `find /home -newer /run/marker` lists only `/home/root/.wifi-signin/log*`.
   - `/home/root/.rmweb` is unchanged.
   - Grepping the log for the submitted values, the token and `?` finds nothing.
7. **Power key.** A single press during a session exits 21 (cancelled) cleanly. A triple press during a session, and another within 10 s after exit, never starts a second xochitl: `journalctl -u xochitl` shows only the launcher's own start. There is no reboot, and xovi-tripletap is active afterwards.

Non-goals for v1. Some are candidates for later versions.

- Automatic portal detection in the background with a prompt. A display-free probe daemon is a v2 option.
- Running inside the stock UI without stopping it. This depends on rmweb's unmerged QTFB-native path; v2.
- Portals that sign in through Google, Facebook or other OAuth providers. Those providers reject embedded browsers by design.
- Portals that script into a popup window or post back to `window.opener`.
- Non-ASCII text entry (accented surnames), date pickers and long select lists. The rmweb keyboard is ASCII only (`@` is on the symbols page). A select advances one option per tap. The date and month input types do not open the keyboard. Such portals need "Use network as is" plus a phone, or a later version.
- reMarkable 1 and 2. They are armv7, and no WebKit build exists for them.
- Saving credentials, Passpoint or OpenRoaming provisioning, and general web browsing.

## 3. Assumptions and constraints

**Devices.**

| Device | Codename | Panel | Notes |
|---|---|---|---|
| Paper Pro | ferrari | 2160x1620 colour | aarch64 |
| Paper Pro Move | chiappa | 1696x954 colour | aarch64 |
| Paper Pure | tatsu | 1872x1404 monochrome | aarch64, no frontlight |

All three run reMarkable OS 3.28.x. The owner updates from 3.27.3 to 3.28 and then turns off automatic updates in reManager's Maintenance tab, so the devices stay on a verified version. OS 3.29 is unverified for the browser engine.

**Access.** Developer Mode is on for each device; the owner accepts the factory reset, the permanent boot warning and the warranty carve-out. SSH runs over USB at 10.11.99.1.

**Mod stack.** Vellum is bootstrapped through reManager. Installed packages are `xovi`, `xovi-extensions` (qt-resource-rebuilder), `appload` 0.6.0 (OS 3.28 to 3.29) and `tripletap`. XOVI is tethered by design: a triple press of the power button starts it for the current boot, and AppLoad icons exist only after that. That triple press restarts xochitl.

**Display.** Only the closed vendor library libqsgepaper drives the panel. v1 presents by stopping xochitl and running as a Qt Quick app on the `epaper` platform plugin. This is the official method, and rmweb uses it.

**Filesystem.** The root filesystem is read-only, and an OS update replaces it wholesale. `/etc` is a volatile overlay. `/home` survives reboots and updates; `/run` and `/tmp` are tmpfs and are cleared at boot. Phase 0 confirms the tmpfs mounts. Installed files live under `/home/root`. Runtime state lives under `/run`.

**Hazards the launcher must handle.**

- Two display owners within about 2 s produce "Failed to initialize SWTCON", which has escalated into a reboot.
- xochitl's unit has StartLimitBurst=4 per 600 s and OnFailure=emergency.target. `systemctl reset-failed xochitl` clears the start counter.
- Stopping xochitl within about 30 s of its start has crashed it, and the emergency target then reboots the device.
- Kernel autosleep is `mem`, and the device suspends whenever no wakelock is held. xochitl holds one; when it stops, nothing does. A resume can start a second xochitl on top of a running app. This has been observed to end in a segfault and a reboot.
- xovi-tripletap keeps reading the power-key node while xochitl is stopped. On a triple press it runs `/home/root/xovi/start`, which runs `systemctl restart xochitl`.
- Qt and WebKit teardown intermittently aborts on this stack, and a fatal signal costs a reboot through the watchdog. rmweb therefore always leaves through `drainForExit()` and then `std::_Exit()`.

**Engine base.** exp78/rmweb v0.9.7 (MIT).

- WPE WebKit 2.48.5, rendered on the CPU (Skia plus Mesa llvmpipe), presented through the `epaper` platform plugin.
- Device-verified on the Paper Pro at 3.28.x. It needs OS 3.28 or newer.
- The release tarball (107 MB compressed, about 164 MB installed) contains the engine runtime.
- The Qt app can be compiled alone against that runtime in minutes, using rmbrowser's fast-build approach.
- The WebProcess runs as root with WebKit's sandbox disabled (see risk 1 in section 11).

## 4. Architecture

Two Vellum packages and one upstream fork.

```
reManager / vellum add
  rmweb-runtime  upstream rmweb 0.9.7 engine bundle, repackaged
     /home/root/.vellum/share/rmweb-runtime/{lib, libexec, share, rmweb-env.sh}
  wifi-signin    built from this project's fork by vbuild
     /home/root/xovi/exthome/appload/wifi-signin/
         external.manifest.json, icon.png, appload-entry.sh, launch, cleanup,
         bin/wifi-signin-wpeqt

Runtime state (tmpfs):  /run/wifi-signin/{pid, state, ready, stderr, profile/, xdg/, ovl/}
User data (persistent): /home/root/.wifi-signin/{log, log.1, settings.conf (optional)}

Owner: triple-press power -> XOVI and AppLoad up -> tap "Wi-Fi Sign-in"
  AppLoad runs appload-entry.sh (a child of xochitl)
    -> systemd-run transient service "wifi-signin", ExecStopPost=cleanup
         launch: lock -> wakelock -> stop tripletap -> 30 s guard -> stop xochitl
                 -> settle -> overlay -> run app
           wifi-signin-wpeqt (RMWEB_KIOSK=captive):
             probe -> portal -> user signs in -> re-probe -> _Exit(code)
         cleanup (launch's trap, then again from ExecStopPost; state-driven, idempotent):
             stop app and helpers -> unmount overlay -> remove EPD lock -> settle 2 s
             -> reset-failed + start xochitl -> timed wakelock -> start tripletap
             -> remove /run/wifi-signin
```

Components:

1. **wifi-signin-wpeqt**: the rmweb Qt/WPE app built from this fork. Captive mode (section 5) is switched on by an environment variable. Without it the binary behaves like upstream, so upstream fixes merge cleanly.
2. **appload-entry.sh**, **launch** and **cleanup**: the takeover launcher (section 6).
3. **Vellum packaging** (section 7).
4. **Test tooling** (section 9): a fake portal for a Mac, host-side launcher tests, and probe classifier tests.

Repository: a fork of exp78/rmweb, with `upstream` kept as a git remote so engine fixes can be merged. Project additions live in:

- `engine/wpeqt/captive/`: new C++ files, plus small, flag-guarded hooks in the existing `main.cpp`;
- `device/wifi-signin/`;
- `packaging/vellum/{rmweb-runtime,wifi-signin}/VELBUILD`;
- `tools/fake-portal/`;
- `tests/`;
- `docs/`.

## 5. Captive mode (inside the app)

**Activation.** The launcher exports `RMWEB_KIOSK=captive`. If `--captive` is also accepted on the command line, `main()` removes it from argv before rmweb parses its positional arguments. In captive mode, positional arguments are ignored: rmweb treats argv[1] as the start URL and argv[2] as a headless save path that exits 0.

### What captive mode switches off in rmweb

Captive mode never reads or writes the rmweb profile:

- no bookmarks, history, tabs, scroll positions or `settings.txt`;
- no `passwords.txt`, no autofill learning and no autofill prefill (`queueSave` and `flushSync` are no-ops);
- no persistent cookie storage (`webkit_cookie_manager_set_persistent_storage` is never called);
- no content-filter store (the content blocker is off, because portals load scripts from vendor domains);
- no Downloads sweep, and downloads are blocked.

The user's rmweb settings (blocker, black-and-white mode, settle flash, auto-refresh, user agent, zoom) are ignored, and fixed captive defaults apply. The auto-refresh guard is off: rmweb normally ignores same-URL meta refresh and `location.reload()` within 15 s of a load, and "Connecting, please wait" portal pages depend on them. Reader mode is unreachable.

**Session.** The view is constructed with `network-session = webkit_network_session_new_ephemeral()`, so cookies, storage and HSTS state live in memory and die with the process. As a second guard, the launcher also exports `RMWEB_PROFILE=/run/wifi-signin/profile` (created empty) and `RMWEB_COOKIES=0`.

**Hardening.** Captive mode loads content from a network the device does not control. The WebKit settings therefore turn off WebGL, WebAudio, media playback, MediaSource, WebRTC and developer extras. JIT stays off; rmweb's default is off. Navigation is allowed only to `http`, `https` and the `rmweb:tls-continue` command. The other internal `rmweb:` pages are blocked.

**Rendering.** All presents go through the epaper platform plugin's own render loop. In captive mode `main()` does not call `EpaperRefresh::init()`. That function would `dlopen` libqsgepaper and resolve mangled `EPFramebuffer` symbols, whose ABI changes between OS builds. As a result there is no settle flash, no black-and-white fast mode and no manual full refresh. Reload forces a repaint, and some ghosting is accepted. Phase 0 checks that black text on a portal page is legible this way; upstream added the settle flash because the plugin's automatic waveform underdrives black. If it is not legible, the fallback is to keep the settle flash and re-verify the vendor-symbol path for each OS build and device.

**Power key.** The app opens the evdev node that reports KEY_POWER and holds EVIOCGRAB on it for its whole lifetime, as rmbrowser does. The node is `30370000.snvs:snvs-powerkey` on the Paper Pro and `44440000.bbnsm:pwrkey` on the Move and the Pure, and the app matches it by capability, not by name. A press means Cancel (exit 21).

**Ready signal.** After its first present to the panel, the app creates `/run/wifi-signin/ready`.

### Probe

The probe is implemented on the libsoup 3 that `rmweb-runtime` already ships. It uses the same glib-networking TLS and system CA bundle as the view, has no host cache, and adds no Qt Network dependency. Every request uses `SOUP_MESSAGE_NO_REDIRECT | SOUP_MESSAGE_NEW_CONNECTION` and `Connection: close`, with a 5 s timeout and plain HTTP, and is never upgraded to HTTPS. The resolved address is logged.

Default endpoints:

- HTTP probe G: `http://connectivitycheck.gstatic.com/generate_204`
- HTTP probe A: `http://captive.apple.com/hotspot-detect.html`
- HTTPS confirmation: `https://www.google.com/generate_204` (requires a verified TLS connection)

The probe hosts must never send HSTS, and they are not on any preload list the app uses. Every endpoint can be overridden in the override file (below).

Classification of a single HTTP probe:

| Response | Class |
|---|---|
| G answers 204. A answers 200 whose body, with trailing whitespace trimmed, equals `<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>` | probe-ok |
| 200, 4xx or 5xx with an empty body or Content-Length of 4 or less | limited |
| any other HTTP response, including 3xx, 511, other 2xx, and 4xx/5xx with a body | captive. Portal URL: the `Location` header resolved against the probe URL, otherwise the probe URL itself |
| DNS failure, connect failure, timeout | no network |

A probe cycle sends G and A in parallel, then the HTTPS confirmation. Only one cycle runs at a time. The combined result is decided in this order:

1. **captive**, if any HTTP probe is captive. This is conclusive.
2. **online**, if at least one HTTP probe is probe-ok and the confirmation returns 204 over verified TLS.
3. **online, clock wrong**, if HTTP is probe-ok and the confirmation fails only with certificate validity-period errors (`G_TLS_CERTIFICATE_NOT_ACTIVATED` or `G_TLS_CERTIFICATE_EXPIRED`).
4. **partial**, if HTTP is probe-ok and the confirmation fails for any other reason.
5. **limited**, if any HTTP probe is limited.
6. **no network**, otherwise. This also applies when wlan0 has no IPv4 address or default route, which is checked first.

Clock check: when the Date header of a 204 from G differs from the device clock by more than 5 minutes, the app logs it and shows "Tablet clock looks wrong" in the top bar. Apple's Date header is never used, because it comes from a CDN cache.

### Start-up

| Combined result | Behaviour |
|---|---|
| online | show "This network is already open" for 2 s, exit 20 |
| online, clock wrong | show "Connected. The tablet clock looks wrong." for 3 s, exit 20 |
| no network | show "Not connected to Wi-Fi. Join the network in Settings first.", exit 23 on Close or after 20 s |
| limited | show "This network is not responding to sign-in checks" with Retry and Close (exit 23) |
| partial | show "Web pages load but secure connections fail on this network" with Retry, Use network as is (exit 24) and Close (exit 23) |
| captive | navigate the view to the probe URL that answered captive, so the network's own redirect or page is shown. If that load ends on the probe host with an empty body or an error and the probe saw a Location, navigate to the resolved Location. The view never loads a probe URL whose last answer was probe-ok. |

### While the portal is shown

**Top bar.** The top bar is always visible. It shows "Wi-Fi Sign-in", the current page's host and a "Test probe" badge whenever any endpoint is overridden. It has four buttons:

- Back: enabled while the view can go back; uses rmweb's existing `goBack`.
- Reload.
- Use network as is: exit 24.
- Cancel: exit 21.

The bar is laid out for the Move's 954-pixel width; rmweb already scales its chrome for that.

**Taps.** rmweb delivers every tap as a JavaScript probe. In captive mode the probe calls `el.click()` on anchors instead of assigning `location.href`, so page handlers and the default action both run. It also matches `[role=button]`, `[role=checkbox]`, `[tabindex]`, `label` and `summary`. When nothing matches, it dispatches pointerdown, mousedown, pointerup, mouseup and click at `document.elementFromPoint(x, y)`. A tap on empty space does nothing; it never hides the bar.

**New-window requests.** The view's `create` handler loads `webkit_navigation_action_get_request(action)` into the main view with `webkit_web_view_load_request`, which keeps the method and body of a `target=_blank` form POST, and then returns NULL. Requests for `about:blank` or an empty URL are logged and ignored, never navigated.

**Certificate errors.** These keep rmweb's existing behaviour, described accurately here. An error page offers "Continue anyway (this site only, this session only)", implemented through the `rmweb:tls-continue` command. Accepting adds the host to an in-memory list for the rest of this run. While a load to a listed host is in flight, the network session ignores certificate errors for all hosts. Strict checking returns when that load finishes, fails or is cancelled. Nothing is persisted. Captive mode keeps the certificate flags from the error callback and shows a "tablet clock may be wrong" hint on the error page only when they include NOT_ACTIVATED or EXPIRED. Phase 0 also tries the stricter per-host alternative, `webkit_network_session_allow_tls_certificate_for_host` followed by a reload, which v2 adopts only if that load paints on the device.

**Re-probe triggers.** These follow AOSP's CaptivePortalLogin, GNOME's portal helper and NetworkManager:

1. every main-frame navigation decision, and every main-frame load end: finished, failed, cancelled or interrupted;
2. 5 s after a form submission, a keyboard Go, or a tap that activated a control;
3. an idle timer: every 10 s while the portal is shown, reset by any page event;
4. immediately, ignoring the rate limit, when a main-frame navigation targets a probe host after the view has left it.

Cycles are at least 3 s apart. A trigger that arrives during a cycle or inside the 3 s window is deferred to the end of the window, never dropped.

**Outcomes while shown.**

- online: show "Connected" (or the clock-wrong variant) for 2 s, then exit 20.
- partial: keep the page, show "Secure connections still failing" in the top bar, and keep re-probing.
- no network: rmweb's error page, with Reload available.

**Idle timeout.** 10 minutes without a touch, exit 22.

**Keyboard.** rmweb's on-screen keyboard is unchanged (see the non-goals in section 2).

### Exit codes and how the app exits

Every captive-mode exit runs on the GUI thread as `WpeView::drainForExit()` followed by `std::_Exit(code)`, the same sequence as rmweb's power and SIGTERM paths, without `flushSync` because nothing is persisted. The app never calls `QCoreApplication::exit()` or `quit()`, and never returns from `app.exec()`.

| Code | Meaning | Source |
|---|---|---|
| 20 | online | captive mode |
| 21 | cancelled (Cancel button or power key) | captive mode |
| 22 | idle timeout | captive mode |
| 23 | no network, or limited and closed | captive mode |
| 24 | use network as is | captive mode |
| 0 | SIGTERM drain | inherited from rmweb |
| 1 | touch grab failed | inherited from rmweb |
| 2, 3 | display connect or QML start-up failure | inherited from rmweb |
| 63 | GUI watchdog: more than 12 s of event-loop silence | inherited from rmweb |
| 128+n | killed by signal n (an abort is 134) | inherited from rmweb |
| 11 | already running | launcher |
| 12 | display busy: xochitl was not active at stop time, or the panel was still held | launcher |
| 13 | systemd-run missing | appload-entry.sh |

The launcher logs the code and its meaning. Every code leads to the same cleanup.

### Override file and logging

**Override file.** Defaults are compiled in, and the package ships no settings file. An optional override file, `/home/root/.wifi-signin/settings.conf`, may set the probe endpoints, the HTTPS confirmation URL and the idle timeout (for tests). apk does not track it, so it survives upgrades and `vellum del`. It is removed only on purge.

**Logging and privacy.** Only the app writes `/home/root/.wifi-signin/log`, capped at 256 KB with one rotation (`log.1`). In captive mode a Qt message handler filters everything before it is written:

- any URL is cut down to `scheme://host`;
- every `[form]` line is dropped;
- the `setFieldText` result is never logged.

The log records probe results with resolved addresses, navigation hosts, overrides in effect and the exit code. It never contains page content, form data or credentials. The launcher sends the app's raw stderr to `/run/wifi-signin/stderr` (tmpfs), reads it only to detect start-up failures, and deletes it in cleanup.

## 6. Launcher

The launcher runs as root under systemd, not inside xochitl, so it sets its own environment: PATH, `HOME=/home/root`, and the Qt and WPE variables from `rmweb-env.sh` with `R=/home/root/.vellum/share/rmweb-runtime`. It also points `XDG_CACHE_HOME`, `XDG_DATA_HOME` and `XDG_CONFIG_HOME` at `/run/wifi-signin/xdg`, and sets `MESA_SHADER_CACHE_DISABLE=true`, so that no library caches land on flash. In the paths below, `$APP` is `/home/root/xovi/exthome/appload/wifi-signin`.

**appload-entry.sh** (what AppLoad executes, as a child of xochitl):

1. If the `wifi-signin` unit exists in any state other than `inactive` or `failed` (`systemctl show -p ActiveState --value wifi-signin`), exit 0. A session is already running or finishing.
2. If `systemd-run` is missing, log it and exit 13 without touching xochitl. There is no direct-exec fallback: inside xochitl's cgroup, stopping xochitl would kill the launcher itself.
3. Otherwise:

```
exec systemd-run --unit=wifi-signin --collect --quiet \
  -p ExecStopPost=-$APP/cleanup -p TimeoutStopSec=30 -p MemoryAccounting=yes \
  $APP/launch
```

This is a transient service rather than a scope, so `cleanup` runs even if `launch` is killed with SIGKILL or by the OOM killer.

**launch.** Each step appends a token to `/run/wifi-signin/state` as soon as its action completes, and cleanup undoes only what is recorded.

1. **Lock.** `mkdir /run/wifi-signin` and write `$$` to `/run/wifi-signin/pid`. If the directory exists and that PID is alive, exit 11 without touching anything; cleanup is disarmed for this run. If the PID is dead, the lock is stale; take it over. `/run` is cleared at boot, so a reboot never leaves a stale lock behind.
2. **Wakelock** (before touching tripletap or xochitl). Write `wifi-signin 900000000000` (a 15-minute timed lock) to `/sys/power/wake_lock` and record `WAKELOCK`. If the write fails, log it and exit without touching xochitl. A background loop in launch rewrites the same line every 60 s while the app runs, so the lock never expires mid-session. Because it is timed, it also expires on its own if everything is killed.
3. **Tripletap.** If `systemctl is-active --quiet xovi-tripletap`, stop it and record `TRIPLETAP`. This is runtime only, and nothing is disabled or persisted.
4. **30-second guard.** Measure xochitl's age on the boot-time clock only:
   - `pid=$(systemctl show -p MainPID --value xochitl)`;
   - the start time is field 22 of `/proc/$pid/stat`, parsed after the last `)`, in USER_HZ ticks since boot;
   - the current time is the first field of `/proc/uptime`;
   - age = now − start ÷ 100.

   Loop with `sleep 1` until ActiveState is `active` and the age is at least 30 s. Re-read MainPID and ActiveState on every pass, and once more immediately before the stop; if either changed, restart the wait. `ActiveEnterTimestampMonotonic` is never compared with `/proc/uptime`, because the two clocks differ by all the time spent suspended.
5. **Stop xochitl.**
   - If xochitl is not `active` at this point, another program owns the display: log it and exit 12, without recording `STOPPED_XOCHITL`, so cleanup will not start xochitl.
   - Otherwise run `systemctl stop xochitl` and record `STOPPED_XOCHITL`.
   - Wait up to 5 s until no process holds `/dev/dri/card0`. Phase 0 confirms the tool: `fuser` if BusyBox has it, otherwise a scan of `/proc/*/fd`. If the panel is still held, log the holder and go to cleanup without launching.
   - Then `rm -f /tmp/epframebuffer.lock` and `sleep 1.5`.
6. **Overlay.** WPE launches its helper processes from a fixed `/usr/libexec/wpe-webkit-2.0` prefix, and `/` is read-only. Mount an overlay on `/usr/libexec` with upper and work directories in `/run/wifi-signin/ovl` (tmpfs), and copy the runtime's helpers into the upper directory. Record `MOUNTED`.
7. **Run the app.** Run `wifi-signin-wpeqt` with stderr redirected to `/run/wifi-signin/stderr`, and record its PID in the state file.
   - **Start-up watchdog.** If `/run/wifi-signin/ready` is absent 30 s after exec, send SIGKILL to the app and the WPE helpers and go to cleanup.
   - **Start-up retry.** If the app ends within 20 s of starting, with any non-zero code or a signal, and its stderr contains `Failed to initialize SWTCON` or `[touch] EVIOCGRAB failed`, retry once. First wait until no app or WPE helper process remains and the panel is free, then `rm -f /tmp/epframebuffer.lock` and `sleep 3`. The retry is best effort; the settles in steps 5 and 8 are the real protection. Exit 63 is never retried. Phase 0 records how a forced display-owner collision actually ends.
8. **Cleanup** runs as `$APP/cleanup`, from launch's trap on EXIT, TERM, INT and HUP, and again from ExecStopPost. It is state-driven and idempotent:
   1. If `/run/wifi-signin/state` is missing, exit 0. If the PID in `/run/wifi-signin/pid` is alive and is not the `launch` process that invoked this cleanup, exit 0: the state belongs to another session.
   2. Send TERM to the app if it is running and wait up to 3 s; its TERM handler drains the panel. Then `kill -9` any WPEWebProcess, WPENetworkProcess or WPEGPUProcess. Wait up to 10 s until no app or helper PID remains.
   3. If `MOUNTED`: unmount `/usr/libexec`, but only if `/proc/mounts` shows `upperdir=/run/wifi-signin/ovl/upper`.
   4. If `STOPPED_XOCHITL` and xochitl is not active:
      - `rm -f /tmp/epframebuffer.lock` and `sleep 2`;
      - rewrite the wakelock as `wifi-signin 30000000000`, a 30 s timed lock that the kernel drops on its own;
      - `systemctl reset-failed xochitl`, then `systemctl start xochitl`;
      - if xochitl is not active, `sleep 2`, then reset-failed and start once more, and log a warning.

      Cleanup never waits on the panel while xochitl is active.
   5. If `WAKELOCK` and xochitl was never stopped, write `wifi-signin` to `/sys/power/wake_unlock`.
   6. If `TRIPLETAP`, start xovi-tripletap again, after xochitl is verified active.
   7. Delete `/run/wifi-signin`. No newer session can exist yet, because appload-entry.sh refuses to start while the unit is still deactivating.

## 7. Packaging and distribution

### rmweb-runtime (category framework)

**Contents.**

- `source`: upstream's own release asset `rmweb-0.9.7.tar.gz`, plus `rmweb-LICENSE::https://raw.githubusercontent.com/exp78/rmweb/v0.9.7/LICENSE` and `rmweb-NOTICE::…/NOTICE`. All are checked with `sha512sums`.
- `package()` installs only `lib/`, `libexec/`, `share/` and `rmweb-env.sh` into the package-private prefix `/home/root/.vellum/share/rmweb-runtime`. It leaves out upstream's `install.sh`, `rmweb`, `appload-entry.sh`, `appload/`, `icon.svg` and `bin/rmweb-wpeqt`. It never touches `/home/root/rmweb`, so a hand-installed upstream rmweb coexists. `LD_LIBRARY_PATH` from `rmweb-env.sh` already takes precedence over the helpers' baked RUNPATH; re-pointing it with patchelf is optional.

**Fields.**

- `arch="aarch64"`
- `depends="remarkable-os>=3.28 remarkable-os<3.30 !rm1 !rm2"`
- `options="!tracedeps !strip"`
- `upstream_author="exp78"`
- `maintainer="<owner name> <email>"`
- `pkgdesc` under 128 characters
- `url="https://github.com/exp78/rmweb"`
- `license="MIT AND LGPL-2.1-or-later AND LGPL-2.0-or-later AND BSD-2-Clause AND BSD-3-Clause AND Apache-2.0 WITH LLVM-exception"`

**Licenses.** LICENSE and NOTICE go to `/home/root/.vellum/licenses/rmweb-runtime/`, together with a SOURCES file. SOURCES lists the exact source URLs from rmweb v0.9.7's `scripts/build-wpe.sh` (wpewebkit 2.48.5, libwpe 1.16.2, libtasn1 4.19.0, libxslt 1.1.39, harfbuzz 8.3.0) and the libsoup, glib-networking and Mesa 24.0.9 versions. For libLLVM-16, libz3, libtinfo, libedit, libbsd, libmd and libatomic, which were copied from Debian bookworm binary packages, it names those source packages and versions.

**Hooks.** `preupgrade` and `predeinstall` stop a running session first (see wifi-signin).

### wifi-signin (category apps), built from source by vbuild

**Contents.**

- `source`: the fork's tag archive (`https://github.com/<owner>/rmweb/archive/refs/tags/wifi-signin-v$pkgver.tar.gz`), `rmweb-0.9.7.tar.gz` (link-time runtime only, not installed) and `wpewebkit-2.48.5.tar.xz` (public headers only).
- `image`: an `eeems/remarkable-toolchain` aarch64 image matching the OS 3.28 SDK (5.8.203).
- `build()`: rmbrowser's fast-build steps, header generation and then cmake of `engine/wpeqt`, run inside that image. No binary from this project's GitHub releases is packaged.
- `package()` installs everything under `$APP` (koreader and netsurf use the same convention): the manifest, icon, scripts and binary.

**Fields.**

- `depends="rmweb-runtime>=0.9.7 rmweb-runtime<0.9.8 launcher remarkable-os>=3.28 remarkable-os<3.30 !rm1 !rm2 !rmppm !rmppure"`. The `!rmppm` and `!rmppure` exclusions are removed as each device passes the acceptance tests. Every rmweb-runtime version bump ships with a wifi-signin rebuild that moves the upper bound. Both are published to the testing repository together, so no index state pairs a new runtime with an old app.
- `maintainer="<owner name> <email>"`
- `upstream_author="<owner GitHub user>"`
- `license="MIT"`
- `pkgdesc` under 128 characters
- `url` pointing at the fork. The fork's LICENSE and NOTICE go to `/home/root/.vellum/licenses/wifi-signin/`.

**Hooks.**

- `postinstall` prints "Refresh AppLoad to see Wi-Fi Sign-in".
- `preupgrade` and `predeinstall` close a running session instead of refusing; apk 3 ignores hook exit status:

  ```
  if systemctl -q is-active wifi-signin; then echo "Closing Wi-Fi Sign-in first"; systemctl stop wifi-signin; fi
  ```

  `systemctl stop` waits for ExecStopPost, and `cleanup` is still on disk at that point, because apk runs pre-deinstall before deleting files.
- `postdeinstall` removes `/home/root/.wifi-signin` only when `VELLUM_PURGE=1`.
- No systemd units are installed, and there is no `mount-rw` and no `postosupgrade`: nothing is installed outside `/home`.

**Manifest.**

```json
{ "name": "Wi-Fi Sign-in",
  "application": "/home/root/xovi/exthome/appload/wifi-signin/appload-entry.sh",
  "workingDirectory": "/home/root/xovi/exthome/appload/wifi-signin",
  "qtfb": false, "disablesWindowedMode": true, "aspectRatio": "original" }
```

### Distribution path

**Development.** vbuild ships only x86_64 Linux binaries, so builds run on Linux or in an x86_64 Linux container on the Mac with the Docker socket mounted.

1. Copy `packaging/vellum/rmweb-runtime` and `packaging/vellum/wifi-signin` into `packages/` of a vellum checkout.
2. Run `./scripts/build-package.sh rmweb-runtime aarch64` and `./scripts/build-package.sh wifi-signin aarch64`.
3. Copy `keys/vellum-dev.rsa.pub`, keeping that file name, to `/home/root/.vellum/etc/apk/keys/`.
4. Install both packages in one transaction, through reManager's terminal or SSH:

   ```
   vellum add ./rmweb-runtime-0.9.7-r0.apk ./wifi-signin-0.1.0-r0.apk
   ```

**Before either upstream PR**, the owner asks the Vellum maintainers, on Discord or in an issue, two things together:

1. Whether rmweb-runtime may repackage upstream's own v0.9.7 release asset under the same exception as koreader and appload, noting that it includes Debian-built libLLVM-16 and its helper libraries.
2. Whether a 107 MB package is acceptable. The largest today is about 87 MB.

**Upstream.** One pull request per package to vellum-dev/vellum. A maintainer publishes it to the testing repository, it is tested on devices, and then it is merged and appears in reManager. Vellum's rules require that pull requests, their descriptions and their commits are written by a person, with no AI co-author trailers. The owner writes and submits these PRs and their commits. Prefer `depends="launcher"` over a hard AppLoad dependency.

**Fallback** (if the runtime is refused):

- either build WPE, Mesa llvmpipe and LLVM inside vbuild from rmweb's recipes, a multi-hour build;
- or keep both packages out of the index and ship signed .apk files from the fork's GitHub releases, with the signing public key published in the repository so users do not need `--allow-untrusted`.

**OS updates.** The packages need no re-enable step. After an OS update the owner runs reManager's Rebuild Hashtable and starts XOVI again, as for every AppLoad app.

## 8. Device plan

**Phase 0, Paper Pro, before any code.**

1. Update to 3.28 and turn off automatic updates.
2. Install upstream rmweb 0.9.7 as-is in a scratch location. Verify it launches and restores xochitl, measure memory with the method from test 5, then remove it and its AppLoad icon.
3. Confirm that `vellum add` of a locally built package works.
4. Record:
   - that `/run` and `/tmp` are tmpfs;
   - that `/sys/power/wake_lock` exists and accepts a timeout;
   - whether `fuser` exists;
   - that `systemd-run -p MemoryAccounting=yes` works;
   - the power-key evdev node names;
   - `/etc/resolv.conf` and `resolvectl status wlan0` (DNS-over-TLS or strict DNSSEC would make every portal unreachable).
5. Check that libsoup with glib-networking validates an HTTPS request from the UI process.
6. Check that portal text is legible without the settle flash.
7. Force a display-owner collision and record how it ends.
8. Try the per-host TLS allow API.

**Phase 1, Paper Pro.** Captive mode, launcher and packages. The fake-portal acceptance tests come first, then a real portal.

**Phase 2, Paper Pro Move.** The same aarch64 binary; rmweb reads panel geometry and touch ranges at runtime. Touch is 1248x2208 units onto a 954x1696 panel, and the power key is `44440000.bbnsm:pwrkey`. Verify presentation, touch, power key and the top bar layout, then drop `!rmppm`.

**Phase 3, Paper Pure.** No browser has run on it yet.

1. Run reMarkable's Qt Quick hello-world on the official `epaper` path.
2. Run upstream rmweb.
3. Run this app.

The panel is monochrome 1872x1404, touch is 1776x2400 units, and the power key is `44440000.bbnsm:pwrkey`. Then drop `!rmppure`.

## 9. Testing

**Fake portal** (`tools/fake-portal/`, Python, runs on the Mac on the same Wi-Fi). It serves the probe endpoints, the HTTPS confirmation endpoint and the portal pages. The device's override file points every endpoint at the Mac and shows the Test probe badge. The probe answers 302 to the portal until the session is granted, then 204. The confirmation endpoint can be switched to succeed, fail, or present a self-signed certificate.

The fake portal provides these variants:

- **Redirects.** A same-host 302; a cross-origin 302 to a second port or a second IP alias on the Mac, in both absolute and relative Location forms; a 511 with a login link; and a 403 with an HTML login page.
- **Sign-in styles.**
  - a plain HTML form;
  - a room-and-name form;
  - a checkbox with a JavaScript submit button;
  - a `fetch()` sign-in that only changes the DOM;
  - a grant that arrives 8 s after the final page has loaded, with no further navigation;
  - a "Connecting…" page that meta-refreshes its own URL every 3 s until the grant.
- **Navigation.** A `target=_blank` POST form; `window.open(url)` from a click handler; `<a href="#">` with an `addEventListener` handler; `<div role="button">`; and a terms link that opens and returns, keeping the checkbox state.
- **TLS.** A portal page on a self-signed HTTPS host, and two self-signed hosts.
- **Privacy.** A GET form carrying a unique token, for the privacy test.
- **Documented failures.** A 200-option select and a non-ASCII surname field, recorded as non-goals.

**Launcher tests** (`tests/launcher_test.sh`, host-side bash with stubbed `systemctl`, `systemd-run`, `mount`, `/proc` and `/sys/power`):

- each exit code;
- SIGTERM mid-run;
- launch SIGKILLed, after which cleanup undoes the recorded state;
- lock held by a live PID (exit 11, nothing touched), and stale lock with a dead PID (taken over);
- `/proc/uptime` ahead of the monotonic clock by simulated suspend time;
- xochitl restarting (new MainPID) during the guard wait;
- xochitl not active at stop time (exit 12, xochitl not started);
- the panel still held after 5 s;
- early exit 134 with a SWTCON line (one retry), a second early 134 (no further retry), and early exit 21 (no retry);
- the ready file missing after 30 s (SIGKILL and cleanup);
- tripletap active before the run (restarted after) and inactive before (left alone);
- cleanup run twice (the second run is a no-op);
- appload-entry with the unit still deactivating (exit 0), and appload-entry with `systemd-run` missing (exit 13).

**Probe classifier tests** (a scriptable mode of the binary against a local test server):

- each row of the single-probe table;
- the exact Apple body, and a page that contains "Success" but is not that body;
- 200 with an empty body, and 200 with a 3-byte body;
- 511, 403 with HTML, and empty 403;
- relative Location;
- mixed results (A probe-ok, G 302 gives captive);
- partial (HTTP 204, confirmation pointed at a closed port);
- clock a year off against a reachable confirmation host;
- timeout and DNS failure.

**On-device checks.** The acceptance tests in section 2, and `journalctl -u xochitl -u wifi-signin` after each run.

## 10. Error handling

| Situation | Handling |
|---|---|
| Launch while xochitl is starting or restarting | the boot-clock 30 s guard in section 6, step 4 waits, and nothing is shown during the wait |
| Another program owns the display | exit 12, xochitl untouched (section 6, step 5) |
| Second display-owner race | wakelock first, tripletap stopped, EPD lock removed and settle before launch and before restart, one start-up retry |
| Power-button presses during a run | grabbed by the app; a single press cancels; tripletap is stopped, so a triple press cannot start xochitl |
| App crash, watchdog exit or start-up hang | the same cleanup path as a normal exit; the start-up watchdog covers a hang before the first present |
| Launcher killed | ExecStopPost runs cleanup from the recorded state; the timed wakelock expires on its own |
| Reboot mid-session | `/run` is cleared, so there is no stale lock; xochitl starts normally at boot |
| Package removed or upgraded mid-session | the hook stops the session first |
| Wi-Fi drops mid-sign-in | the probe reports no network; rmweb's error page offers Reload; the idle timeout still applies |
| HSTS or HTTPS-only landing | probes are plain HTTP and never upgraded; certificate errors go through "Continue anyway" |
| Wrong clock (NTP often blocked behind a portal) | validity-only failures of the HTTPS confirmation count as online with a notice; a Date-header check and a targeted hint on the certificate page |
| Frozen screen | the GUI watchdog exits after 12 s of event-loop silence, Cancel and the power key are always available, and the idle timeout is 10 minutes |

## 11. Risks

1. **Engine exposure.** The WebProcess runs unsandboxed as root on WPE 2.48.5, which predates later WebKit security fixes (WSA-2025-0006 onward). Captive mode loads content that an untrusted network can inject. The mitigations are the hardening settings in section 5 and the short lifetime of a session. The README and the package description state the residual risk. v2, or the maintainer conversation, may move the runtime to a current WPE stable release, rebuilt with each advisory.
2. **OS 3.29 and later.** The engine is verified only on 3.28.x, and each OS update needs re-verification. The owner pins 3.28 until then.
3. **Move and Pure.** No out-of-process browser has been tested on either; phases 2 and 3 may surface input or presentation bugs.
4. **Memory.** No measured figure exists for rmweb. Phase 0 measures it before any further commitment.
5. **Upstream health.** rmweb has a single maintainer, so the fork must be able to live on its own.
6. **Vellum acceptance.** The runtime's repackaging and its size are maintainer judgements; section 7 gives the fallback. Vellum's human-authorship rule means the owner authors the packaging PRs personally.
7. **Display hazards.** The launcher design follows every documented failure, but the SWTCON abort can still reboot the device in cases nobody has documented. Phase 0's forced collision test calibrates this.
8. **Other xochitl starters.** reManager's "Restart reMarkable UI" or a manual `systemctl start xochitl` over SSH during a session would create two display owners. Tripletap is handled. v2 can add `Conflicts=xochitl.service` to the transient unit.

## 12. Decisions

**The owner decides before implementation:**

1. **HTTPS confirmation host.** Decided 2026-10-25: Google's 204 endpoint (`https://www.google.com/generate_204`) is the default. It stays overridable in the override file.
2. **Commit attribution.** Commits in this fork may carry AI co-author trailers if the owner wants. Commits and PR text for vellum-dev/vellum must not, so the owner writes them.
3. **Maintainer question.** Ask the Vellum maintainers the section 7 question before building the runtime package.

**Phase 0 decides:**

4. Rendering without the settle flash, or the vendor-symbol fallback.
5. Whether to adopt the per-host TLS allow API.
6. Which tool waits on the panel (`fuser` or a `/proc` scan).

**Settled during implementation:**

7. Whether AppLoad needs a reload by name after install.

## 13. References

**Engine base:** exp78/rmweb (README, docs/install.md, docs/device-profile.md, device/rmweb, device/appload-entry.sh, device/rmweb-env.sh, engine/wpeqt/main.cpp, profile.h, scripts/bundle.sh and package.sh, releases) and jhasubhash/rmbrowser (fast-build.sh, docs/paper-pro-setup.md, the PowerReader and wakelock code in engine/wpeqt/main.cpp).

**Takeover launchers:** MaximeRivest/quill (scripts/takeover.sh, docs/device-test-2026-07-12.md) and MaximeRivest/chromium (chromium-bootstrap/README.md, chromium-takeover.sh, appload-launch.sh).

**Mod stack:** rmitchellscott/xovi-tripletap v1.1.1, asivery/rm-xovi-extensions (xovi/start) and asivery/rm-appload.

**Packaging:** vellum-dev/vellum (README, AGENTS.md, scripts, packages koreader, netsurf, appload, tripletap; PRs #245, #307 and #379), vellum-dev/vellum-cli, vellum-dev/apk-tools 3.0.3 (database.c, package.c), Eeems/vbuild 0.0.36 and rmitchellscott/reManager.

**Platform docs:** developer.remarkable.com (Developer mode, Writing Qt Quick Applications, SDK) and support.remarkable.com ("Why won't my reMarkable connect to Wi-Fi?").

**Captive-portal behaviour:**
- AOSP NetworkStack (NetworkMonitor, CaptivePortalProbeResult, NetworkStackUtils) and CaptivePortalLoginActivity;
- GNOME gnome-shell js/portalHelper/main.js;
- NetworkManager nm-connectivity.c and nm-device.c;
- Qt 6.8 qhostinfo.cpp and qnetworkaccesscache.cpp;
- WPE WebKit 2.48 API reference and WebKit security advisories;
- RFC 6585, 6797, 8908, 8910 and 8952;
- the Linux sysfs-power ABI, proc_uptime(5) and the systemd D-Bus property documentation.
