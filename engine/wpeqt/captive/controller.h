// Pure captive-portal controller: the state machine of the Wi-Fi sign-in mode. Standard library only, and it reaches
// the outside world only through ControllerHooks, so it is unit-tested on the host with a fake clock
// (tests/captive_controller_test.cpp); main.cpp wires the hooks to the probe runner, WebKit and the GUI.
//
// Phases (spec §5 "Start-up" and "While the portal is shown"):
//  1. start(): the idle clock starts and the start-up probe cycle runs. Its result picks what comes next:
//       Online            AlreadyOpen screen, exit 20 after 2 s
//       OnlineClockWrong  ConnectedClockWrong screen, exit 20 after 3 s
//       NoNetwork         NoNetwork screen; Close, or 20 s later: exit 23
//       Limited           Limited screen; Retry runs another cycle, Close exits 23
//       Partial           Partial screen; Retry, UseAsIs exits 24, Close exits 23
//       Captive           the portal: showScreen(None), navigate(viewUrl), arm the scheduler, start the 500 ms tick
//     A Retry cycle's result goes through the same table. viewUrl is the URL of the probe that answered captive
//     (combineCycle), so the view never loads a probe URL whose answer was probe-ok.
//  2. Portal shown: navigation decisions, load ends and submit-like actions become scheduler triggers, and the tick
//     (plus every trigger) asks the scheduler whether a cycle is due. Results while shown:
//       Online / OnlineClockWrong             Connected / ConnectedClockWrong screen, exit 20 after 2 s
//       Partial, Limited, NoNetwork, Captive  keep the page: no navigation, no exit; re-probing goes on
//  3. Exiting: an exit timer is pending or exitWith has run. From then on nothing happens: the tick stops, no cycle
//     starts, results, buttons and page events are ignored, and exitWith never runs a second time.
//
// Rules across phases:
//  - Cycles: the start-up and Retry cycles go straight to hooks.runCycle while the scheduler is disarmed and never
//    touch it. Only a cycle started because startIfDue() returned true ends with cycleFinished() — always, even when
//    its answer arrives after an exit (otherwise the scheduler would stay "in flight" for good). Never two cycles at
//    once: a Retry during a cycle is ignored. Cycles are at least 3 s apart (spec §5), so the first scheduled cycle
//    also waits until 3 s after the start-up / Retry cycle started — unless it is a ProbeHostNav, which ignores gaps.
//  - Status line, recomputed after every completed cycle: Partial -> kStatusSecureFailing; otherwise a Date from a 204
//    on G more than 300 s off the device clock -> kStatusClockWrong; otherwise "" (cleared). setStatus runs only when
//    the text changes.
//  - Buttons: Cancel (21) and UseAsIs (24) work in every phase; Close (23) and Retry only on a start-up screen that
//    offers them (screenButtons).
//  - Idle: exit 22 once onUserActivity has not been called for idleTimeoutS seconds, counted from start(). A one-shot
//    `after` watchdog fires at the deadline in every phase (re-armed when activity moved it); ticks and cycle
//    completions check the deadline as well.
//  - Hosts compare as host[:port], case-insensitively, with userinfo and a default port dropped (ctlHostKey); the
//    probe hosts are those of cfg.gUrl and cfg.aUrl. A navigation decision to a probe host after the view has been on
//    any other host is a ProbeHostNav (immediate cycle); every other decision, and every load end, is a PageEvent.
//    The hosts the view has been on are learned from decisions and load ends while the portal is shown.
//  - Location fallback, once per captive outcome: a load end on exactly that outcome's viewUrl (fragment ignored, see
//    ctlUrlKey) navigates to its locationUrl when it has one — the view stayed on the probe URL with an empty page or
//    an error. A load end on another page of the probe host does not: same-origin portals serve their pages there.
//
// Threading and lifetime: call every member on one thread, the one `after` callbacks and runCycle's `done` run on (the
// engine thread). Every hook must be set. Callbacks handed to `after` and runCycle hold only a liveness token, so they
// may safely outlive the controller: they then do nothing.
#pragma once
#include "config.h"
#include "exit_codes.h"
#include "probe_classify.h"
#include "probe_scheduler.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace rmweb::captive {

enum class Screen { None, AlreadyOpen, ConnectedClockWrong, NoNetwork, Limited, Partial, Connected };
enum class Button { Retry, Close, UseAsIs, Cancel };

// A screen's text, exactly as the UI shows it. None is the portal page itself: "".
inline const char* screenText(Screen s) {
    switch (s) {
        case Screen::None:                return "";
        case Screen::AlreadyOpen:         return "This network is already open";
        case Screen::ConnectedClockWrong: return "Connected. The tablet clock looks wrong.";
        case Screen::NoNetwork:           return "Not connected to Wi-Fi. Join the network in Settings first.";
        case Screen::Limited:             return "This network is not responding to sign-in checks";
        case Screen::Partial:             return "Web pages load but secure connections fail on this network";
        case Screen::Connected:           return "Connected";
    }
    return "";
}

// The buttons a start-up screen offers (the top bar's Use network as is and Cancel are always there).
inline std::vector<Button> screenButtons(Screen s) {
    switch (s) {
        case Screen::NoNetwork: return {Button::Close};
        case Screen::Limited:   return {Button::Retry, Button::Close};
        case Screen::Partial:   return {Button::Retry, Button::UseAsIs, Button::Close};
        default:                return {};
    }
}

inline constexpr const char* kStatusSecureFailing = "Secure connections still failing";
inline constexpr const char* kStatusClockWrong    = "Tablet clock looks wrong";

struct ControllerHooks {
    std::function<int64_t()> nowMs;                                          // monotonic clock, ms
    std::function<void(int delayMs, std::function<void()>)> after;          // one-shot timer on the engine thread
    std::function<void(std::function<void(const CycleReport&)>)> runCycle;  // async; `done` exactly once, later
    std::function<void(const std::string& url)> navigate;
    std::function<void(Screen)> showScreen;
    std::function<void(const std::string& text)> setStatus;                 // "" clears
    std::function<void(int code)> exitWith;                                 // GUI drains + _Exit; called at most once
};

namespace detail {

inline std::string ctlLower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

// Index of the "://" that ends a valid RFC 3986 scheme at the start of uri, npos if there is none (about:blank,
// rmweb:tls-continue, data:..., a relative reference).
inline std::size_t ctlSchemeEnd(const std::string& uri) {
    const std::size_t sep = uri.find("://");
    if (sep == std::string::npos || sep == 0 || !isAlpha(uri[0])) return std::string::npos;
    for (std::size_t i = 1; i < sep; ++i) {
        const char c = uri[i];
        if (!isAlpha(c) && !isDigit(c) && c != '+' && c != '-' && c != '.') return std::string::npos;
    }
    return sep;
}

// host[:port] of an absolute URI, lowercased, without userinfo and without a default (:80 for http, :443 for https) or
// empty port; "" when the URI has no authority.
inline std::string ctlHostKey(const std::string& uri) {
    const std::size_t sep = ctlSchemeEnd(uri);
    if (sep == std::string::npos) return std::string();
    const std::string scheme = ctlLower(uri.substr(0, sep));
    const std::size_t begin = sep + 3;
    const std::size_t end = uri.find_first_of("/?#", begin);
    std::string auth = uri.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
    const std::size_t at = auth.rfind('@');
    if (at != std::string::npos) auth.erase(0, at + 1);
    auth = ctlLower(auth);
    const std::size_t colon = auth.rfind(':');
    if (colon != std::string::npos && auth.find(']', colon) == std::string::npos) {   // a port, not an IPv6 colon
        const std::string port = auth.substr(colon + 1);
        if (port.empty() || (scheme == "http" && port == "80") || (scheme == "https" && port == "443")) auth.erase(colon);
    }
    return auth;
}

// A URL reduced for "is the view on this exact page?": the fragment is dropped, the scheme lowercased, the authority
// reduced to ctlHostKey, and an empty path counts as "/" — the differences WebKit's URL parser normalises away. The
// path and the query stay exact.
inline std::string ctlUrlKey(const std::string& url) {
    const std::string u = url.substr(0, url.find('#'));
    const std::size_t sep = ctlSchemeEnd(u);
    if (sep == std::string::npos) return u;
    const std::size_t pathAt = u.find_first_of("/?", sep + 3);
    std::string rest = pathAt == std::string::npos ? std::string() : u.substr(pathAt);
    if (rest.empty() || rest[0] == '?') rest.insert(0, 1, '/');
    return ctlLower(u.substr(0, sep)) + "://" + ctlHostKey(u) + rest;
}

inline bool ctlScreenOffers(Screen s, Button b) {
    const std::vector<Button> buttons = screenButtons(s);
    return std::find(buttons.begin(), buttons.end(), b) != buttons.end();
}

}  // namespace detail

class CaptiveController {
public:
    static constexpr int kTickMs = 500;                // scheduler poll and idle check while the portal is shown
    static constexpr int kAlreadyOpenMs = 2000;        // start-up Online
    static constexpr int kStartupClockWrongMs = 3000;  // start-up OnlineClockWrong
    static constexpr int kConnectedMs = 2000;          // Online / OnlineClockWrong while the portal is shown
    static constexpr int kNoNetworkCloseMs = 20000;    // NoNetwork screen auto-close

    // A non-positive cfg.idleTimeoutS (parseCaptiveConfig never yields one) falls back to the 600 s default.
    CaptiveController(CaptiveConfig cfg, ControllerHooks hooks)
        : hooks_(std::move(hooks)),
          gHost_(detail::ctlHostKey(cfg.gUrl)),
          aHost_(detail::ctlHostKey(cfg.aUrl)),
          idleMs_(static_cast<int64_t>(cfg.idleTimeoutS > 0 ? cfg.idleTimeoutS : CaptiveConfig{}.idleTimeoutS) * 1000) {}
    CaptiveController(const CaptiveController&) = delete;             // callbacks hold `this`
    CaptiveController& operator=(const CaptiveController&) = delete;

    // First cycle -> start-up table. Only the first call counts.
    void start() {
        if (started_ || exit_ != ExitState::None) return;
        started_ = true;
        const int64_t now = hooks_.nowMs();
        lastActivity_ = now;
        armIdleWatchdog(now);
        startCycle(/*fromScheduler=*/false, now);
    }

    // Every navigation decision (WebKit reports main and sub frames alike): PageEvent, or ProbeHostNav when it targets
    // a probe host after the view has been on another host.
    void onNavigationDecision(const std::string& uri) {
        if (!portalLive()) return;
        const int64_t now = hooks_.nowMs();
        const std::string host = detail::ctlHostKey(uri);
        if (isProbeHost(host) && leftProbeHost_) {
            sched_.trigger(Trigger::ProbeHostNav, now);
            hostNavPending_ = true;
        } else {
            sched_.trigger(Trigger::PageEvent, now);
        }
        noteHost(host);
        pump(now);
    }

    // A load finished, failed, was cancelled or interrupted: PageEvent, plus the Location fallback.
    void onLoadEnd(const std::string& uri) {
        if (!portalLive()) return;
        const int64_t now = hooks_.nowMs();
        noteHost(detail::ctlHostKey(uri));
        sched_.trigger(Trigger::PageEvent, now);
        if (fallbackArmed_ && detail::ctlUrlKey(uri) == fallbackViewKey_) {
            fallbackArmed_ = false;
            hooks_.navigate(fallbackLocation_);
        }
        pump(now);
    }

    // A form submit, keyboard Go or control tap: re-probe 5 s later.
    void onSubmitLike() {
        if (!portalLive()) return;
        const int64_t now = hooks_.nowMs();
        sched_.trigger(Trigger::SubmitLike, now);
        pump(now);
    }

    // Any touch: restarts the idle timeout.
    void onUserActivity() {
        if (exit_ != ExitState::None) return;
        lastActivity_ = hooks_.nowMs();
    }

    void onButton(Button b) {
        if (exit_ != ExitState::None) return;
        switch (b) {
            case Button::Cancel:  exitNow(kExitCancelled); break;
            case Button::UseAsIs: exitNow(kExitUseAsIs); break;
            case Button::Close:   if (startupScreenOffers(Button::Close)) exitNow(kExitNoNetwork); break;
            case Button::Retry:
                if (startupScreenOffers(Button::Retry) && !cycleInFlight_) startCycle(/*fromScheduler=*/false, hooks_.nowMs());
                break;
        }
    }

    // Read-only view of the re-probe scheduler, for tests and logging.
    const ProbeScheduler& scheduler() const { return sched_; }

private:
    enum class ExitState { None, Pending, Done };   // Pending: an exit timer is running; Done: exitWith has run

    // Wraps a callback for `after` / runCycle so that it does nothing once this controller is gone.
    template <class F>
    auto guarded(F f) const {
        return [alive = std::weak_ptr<int>(alive_), f = std::move(f)](auto&&... args) {
            if (!alive.expired()) f(std::forward<decltype(args)>(args)...);
        };
    }

    bool portalLive() const { return shown_ && exit_ == ExitState::None; }
    bool idleExpired(int64_t now) const { return now - lastActivity_ >= idleMs_; }
    bool isProbeHost(const std::string& host) const { return !host.empty() && (host == gHost_ || host == aHost_); }
    void noteHost(const std::string& host) { if (!host.empty() && !isProbeHost(host)) leftProbeHost_ = true; }
    bool startupScreenOffers(Button b) const { return !shown_ && screenKnown_ && detail::ctlScreenOffers(screen_, b); }

    void startCycle(bool fromScheduler, int64_t now) {
        cycleInFlight_ = true;
        const uint64_t id = ++cycleId_;
        if (!fromScheduler) directGapUntil_ = now + ProbeScheduler::kMinGapMs;
        hooks_.runCycle(guarded([this, id, fromScheduler](const CycleReport& r) { onCycleDone(id, fromScheduler, r); }));
    }

    void onCycleDone(uint64_t id, bool fromScheduler, const CycleReport& r) {
        if (!cycleInFlight_ || id != cycleId_) return;          // a stray or repeated `done`
        cycleInFlight_ = false;
        const int64_t now = hooks_.nowMs();
        if (fromScheduler) sched_.cycleFinished(now);           // always, even when exiting
        if (exit_ != ExitState::None) return;
        updateStatus(r);
        if (idleExpired(now)) { exitNow(kExitIdle); return; }
        if (shown_) {
            onShownResult(r.outcome);
            pump(now);
        } else {
            onStartupResult(r.outcome, now);
        }
    }

    void onStartupResult(const CycleOutcome& o, int64_t now) {
        switch (o.result) {
            case CycleResult::Online:           setScreen(Screen::AlreadyOpen);         exitAfter(kExitOnline, kAlreadyOpenMs); break;
            case CycleResult::OnlineClockWrong: setScreen(Screen::ConnectedClockWrong); exitAfter(kExitOnline, kStartupClockWrongMs); break;
            case CycleResult::NoNetwork:        setScreen(Screen::NoNetwork);           armNoNetworkClose(); break;
            case CycleResult::Limited:          setScreen(Screen::Limited); break;
            case CycleResult::Partial:          setScreen(Screen::Partial); break;
            case CycleResult::Captive:          enterPortal(o, now); break;
        }
    }

    void onShownResult(const CycleOutcome& o) {
        switch (o.result) {
            case CycleResult::Online:           setScreen(Screen::Connected);           exitAfter(kExitOnline, kConnectedMs); break;
            case CycleResult::OnlineClockWrong: setScreen(Screen::ConnectedClockWrong); exitAfter(kExitOnline, kConnectedMs); break;
            case CycleResult::Captive:          armFallback(o); break;   // keeps the page: never navigates again
            case CycleResult::Partial:                                   // the status line says it
            case CycleResult::Limited:
            case CycleResult::NoNetwork:        break;                   // keeps the page (rmweb's error page has Reload)
        }
    }

    void enterPortal(const CycleOutcome& o, int64_t now) {
        shown_ = true;
        screen_ = Screen::None;
        screenKnown_ = true;
        hooks_.showScreen(Screen::None);
        armFallback(o);
        hooks_.navigate(o.viewUrl);
        if (exit_ != ExitState::None) return;
        sched_.arm(now);
        hostNavPending_ = false;
        scheduleTick();
    }

    void armFallback(const CycleOutcome& o) {
        fallbackViewKey_ = detail::ctlUrlKey(o.viewUrl);
        fallbackLocation_ = o.locationUrl;
        fallbackArmed_ = !o.locationUrl.empty();
    }

    void setScreen(Screen s) {
        if (screenKnown_ && screen_ == s) return;
        screen_ = s;
        screenKnown_ = true;
        hooks_.showScreen(s);
    }

    void updateStatus(const CycleReport& r) {
        const char* text = "";
        if (r.outcome.result == CycleResult::Partial) text = kStatusSecureFailing;
        else if (r.dateKnown && clockSkewed(r.skewSec, 0)) text = kStatusClockWrong;   // skewSec is already server - device
        if (status_ == text) return;
        status_ = text;
        hooks_.setStatus(status_);
    }

    // Starts a scheduler cycle if one is due. Inside the 3 s after a start-up / Retry cycle started, only a ProbeHostNav
    // may start one.
    void pump(int64_t now) {
        if (!portalLive() || cycleInFlight_) return;
        if (now < directGapUntil_ && !hostNavPending_) return;
        if (!sched_.startIfDue(now)) return;
        hostNavPending_ = false;
        startCycle(/*fromScheduler=*/true, now);
    }

    void scheduleTick() {
        if (tickPending_) return;
        tickPending_ = true;
        hooks_.after(kTickMs, guarded([this] {
            tickPending_ = false;
            onTick();
        }));
    }

    void onTick() {
        if (!portalLive()) return;                              // the tick stops for good once exiting
        const int64_t now = hooks_.nowMs();
        if (idleExpired(now)) { exitNow(kExitIdle); return; }
        pump(now);
        if (portalLive()) scheduleTick();
    }

    void armIdleWatchdog(int64_t now) {
        if (idleTimerPending_ || exit_ != ExitState::None) return;
        idleTimerPending_ = true;
        const int64_t left = lastActivity_ + idleMs_ - now;
        const int delayMs = static_cast<int>(std::clamp<int64_t>(left, 0, std::numeric_limits<int>::max()));
        hooks_.after(delayMs, guarded([this] {
            idleTimerPending_ = false;
            if (exit_ != ExitState::None) return;
            const int64_t t = hooks_.nowMs();
            if (idleExpired(t)) exitNow(kExitIdle);
            else armIdleWatchdog(t);                            // activity moved the deadline
        }));
    }

    // The NoNetwork screen offers only Close, so nothing but an exit can follow it; exitNow() ignores a second exit.
    void armNoNetworkClose() {
        hooks_.after(kNoNetworkCloseMs, guarded([this] {
            if (!shown_ && screen_ == Screen::NoNetwork) exitNow(kExitNoNetwork);
        }));
    }

    void exitNow(int code) {
        if (exit_ != ExitState::None) return;
        exit_ = ExitState::Done;
        sched_.disarm();
        hooks_.exitWith(code);
    }

    void exitAfter(int code, int delayMs) {
        if (exit_ != ExitState::None) return;
        exit_ = ExitState::Pending;
        sched_.disarm();
        hooks_.after(delayMs, guarded([this, code] {
            if (exit_ != ExitState::Pending) return;
            exit_ = ExitState::Done;
            hooks_.exitWith(code);
        }));
    }

    ControllerHooks hooks_;
    std::string gHost_, aHost_;                      // probe hosts (ctlHostKey of cfg.gUrl / cfg.aUrl)
    int64_t idleMs_;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);   // liveness token for guarded() callbacks
    ProbeScheduler sched_;

    bool started_ = false;
    bool shown_ = false;                             // the portal is on screen (never reverts)
    ExitState exit_ = ExitState::None;
    bool screenKnown_ = false;                       // false until the first showScreen
    Screen screen_ = Screen::None;
    std::string status_;                             // the status line's current text

    int64_t lastActivity_ = 0;
    bool idleTimerPending_ = false;
    bool tickPending_ = false;

    bool cycleInFlight_ = false;
    uint64_t cycleId_ = 0;                           // id of the latest cycle; its `done` must carry it
    int64_t directGapUntil_ = std::numeric_limits<int64_t>::min();   // start-up / Retry cycle start + 3 s
    bool hostNavPending_ = false;                    // a ProbeHostNav waits: it may start inside that gap

    bool leftProbeHost_ = false;                     // the view has been on a non-probe host
    bool fallbackArmed_ = false;                     // the latest captive outcome's Location fallback is unused
    std::string fallbackViewKey_, fallbackLocation_;
};

}  // namespace rmweb::captive
