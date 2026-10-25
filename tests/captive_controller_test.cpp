// Host unit test for the captive-portal controller state machine (no Qt, no libsoup, no device). Build+run on the host:
//   clang++ -std=c++17 -Wall -Wextra -o build/captive_controller_test tests/captive_controller_test.cpp && ./build/captive_controller_test
// The harness stands in for the engine's main loop: a fake clock, a timer queue whose `after` callbacks fire in time
// order (ties in the order they were scheduled) as a case advances the clock, scripted probe-cycle results and a
// record of every hook call. runCycle only stores its `done`, so a case completes each cycle at the time it chooses,
// which simulates the cycle's duration (or the harness completes it by itself after autoCompleteMs).
#include "../engine/wpeqt/captive/controller.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>
using namespace rmweb::captive;

static int fails = 0;
#define CHECK(c) do { if(!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while(0)

static constexpr int64_t kNever = std::numeric_limits<int64_t>::max();
static const std::string kG = "http://connectivitycheck.gstatic.com/generate_204";   // CaptiveConfig's defaults
static const std::string kA = "http://captive.apple.com/hotspot-detect.html";
static const std::string kPortal = "http://portal.example.net/login?ap=7";          // the portal's own page

static const char* screenName(Screen s) {
    switch (s) {
        case Screen::None:                return "None";
        case Screen::AlreadyOpen:         return "AlreadyOpen";
        case Screen::ConnectedClockWrong: return "ConnectedClockWrong";
        case Screen::NoNetwork:           return "NoNetwork";
        case Screen::Limited:             return "Limited";
        case Screen::Partial:             return "Partial";
        case Screen::Connected:           return "Connected";
    }
    return "?";
}

struct Harness {
    struct Timer { int64_t at; uint64_t seq; std::function<void()> fn; };

    int64_t now = 0;
    uint64_t nextSeq = 0;
    std::vector<Timer> timers;                            // pending one-shots: `after` calls and auto-completions
    std::vector<int> afterDelays;                         // every delay the controller passed to `after`, in order

    std::function<void(const CycleReport&)> pendingDone;  // the in-flight cycle's completion
    std::vector<int64_t> cycleStarts;                     // clock time of every runCycle call
    bool overlap = false;                                 // runCycle while a cycle was still in flight
    int autoCompleteMs = -1;                              // >= 0: every cycle answers autoReport this long after it starts
    CycleReport autoReport;

    std::vector<std::string> navs;
    std::vector<Screen> screens;
    std::vector<std::string> statuses;
    std::vector<int> exits;
    std::vector<int64_t> exitTimes;
    std::vector<std::string> events;                      // every hook call, in order
    std::vector<bool> armedAtNavigate;                    // was the scheduler armed when navigate() ran?

    std::unique_ptr<CaptiveController> ctl;

    explicit Harness(const CaptiveConfig& cfg = CaptiveConfig{}) {
        ControllerHooks h;
        h.nowMs = [this] { return now; };
        h.after = [this](int delayMs, std::function<void()> fn) {
            afterDelays.push_back(delayMs);
            timers.push_back({now + delayMs, nextSeq++, std::move(fn)});
        };
        h.runCycle = [this](std::function<void(const CycleReport&)> done) {
            if (pendingDone) overlap = true;
            cycleStarts.push_back(now);
            events.push_back("cycle");
            pendingDone = std::move(done);
            if (autoCompleteMs >= 0)
                timers.push_back({now + autoCompleteMs, nextSeq++, [this] { if (pendingDone) complete(autoReport); }});
        };
        h.navigate = [this](const std::string& url) {
            navs.push_back(url);
            events.push_back("nav:" + url);
            armedAtNavigate.push_back(ctl && ctl->scheduler().dueAtMs() != kNever);
        };
        h.showScreen = [this](Screen s) { screens.push_back(s); events.push_back(std::string("screen:") + screenName(s)); };
        h.setStatus = [this](const std::string& t) { statuses.push_back(t); events.push_back("status:" + t); };
        h.exitWith = [this](int code) {
            exits.push_back(code);
            exitTimes.push_back(now);
            events.push_back("exit:" + std::to_string(code));
        };
        ctl = std::make_unique<CaptiveController>(cfg, std::move(h));
    }
    Harness(const Harness&) = delete;
    Harness& operator=(const Harness&) = delete;

    // Fires every timer due at or before t, earliest first (ties in scheduling order), then sets the clock to t.
    void advanceTo(int64_t t) {
        for (;;) {
            auto best = timers.end();
            for (auto it = timers.begin(); it != timers.end(); ++it)
                if (it->at <= t && (best == timers.end() || it->at < best->at || (it->at == best->at && it->seq < best->seq)))
                    best = it;
            if (best == timers.end()) break;
            Timer tm = std::move(*best);
            timers.erase(best);
            if (tm.at > now) now = tm.at;
            tm.fn();
        }
        if (t > now) now = t;
    }

    bool inFlight() const { return static_cast<bool>(pendingDone); }

    void complete(const CycleReport& r) {
        CHECK(inFlight());
        if (!pendingDone) return;
        std::function<void(const CycleReport&)> done = std::move(pendingDone);
        pendingDone = nullptr;
        done(r);
    }
    void completeAt(int64_t t, const CycleReport& r) { advanceTo(t); complete(r); }

    long ticksScheduled() const { return static_cast<long>(std::count(afterDelays.begin(), afterDelays.end(), 500)); }
};

static CycleReport report(CycleResult res, const std::string& view = "", const std::string& loc = "") {
    CycleReport r;
    r.outcome.result = res;
    r.outcome.viewUrl = view;
    r.outcome.locationUrl = loc;
    return r;
}
static CycleReport captive(const std::string& view = kG, const std::string& loc = "") {
    return report(CycleResult::Captive, view, loc);
}
static CycleReport withSkew(CycleReport r, long long skewSec) {   // a 204 from G whose Date is skewSec off
    r.dateKnown = true;
    r.skewSec = skewSec;
    return r;
}

// start() at 0; the start-up cycle answers Captive at 100: the portal is shown and the scheduler armed at 100. The tick
// then runs at 600, 1100, 1600, ... and the first scheduled re-probe may start at 3000 (3 s after the start-up cycle).
static void showPortal(Harness& h, const std::string& view = kG, const std::string& loc = "") {
    h.ctl->start();
    h.completeAt(100, captive(view, loc));
}

// One re-probe that answers r: at t a load end on the portal's own page asks for a cycle, which must start at once (the
// caller keeps t >= 3 s after the previous start, with no cycle in flight), and it answers at t + 100.
static void reprobeAt(Harness& h, int64_t t, const CycleReport& r) {
    h.advanceTo(t);
    const size_t before = h.cycleStarts.size();
    h.ctl->onLoadEnd(kPortal);
    CHECK(h.cycleStarts.size() == before + 1);
    CHECK(!h.cycleStarts.empty() && h.cycleStarts.back() == t);
    h.completeAt(t + 100, r);
}

// Advances the clock until the controller starts its next cycle (at most 20 s), which then answers r 100 ms after it began.
static void answerNextCycle(Harness& h, const CycleReport& r) {
    const size_t before = h.cycleStarts.size();
    const int64_t limit = h.now + 20000;
    while (h.cycleStarts.size() == before && h.now < limit) h.advanceTo(h.now + 100);
    CHECK(h.cycleStarts.size() == before + 1);
    if (h.cycleStarts.size() != before + 1) return;
    h.completeAt(std::max(h.now, h.cycleStarts.back() + 100), r);
}

// True when the controller's scheduler has no cycle in flight. arm() never clears the in-flight flag, so a re-armed COPY
// reports INT64_MAX exactly while a cycle that startIfDue() started has not been ended with cycleFinished().
static bool schedulerIdle(const CaptiveController& c, int64_t now) {
    ProbeScheduler s = c.scheduler();
    s.arm(now);
    return s.dueAtMs() != kNever;
}

// ===================================================================================================================
// The brief's cases
// ===================================================================================================================

static void startup_online_exits_20_after_2s() {
    Harness h;
    h.ctl->start();
    CHECK(h.cycleStarts == std::vector<int64_t>{0});
    h.completeAt(1500, report(CycleResult::Online));
    CHECK(h.screens == std::vector<Screen>{Screen::AlreadyOpen});
    CHECK(h.exits.empty());
    h.advanceTo(3499);
    CHECK(h.exits.empty());
    h.advanceTo(3500);
    CHECK(h.exits == std::vector<int>{kExitOnline});
    CHECK(h.exitTimes == std::vector<int64_t>{3500});
    h.advanceTo(700000);
    CHECK(h.exits.size() == 1);
    CHECK(h.navs.empty());
    CHECK(h.statuses.empty());                 // "" stays "": setStatus only on a change
    CHECK(h.cycleStarts.size() == 1);
    CHECK(h.timers.empty());                   // nothing left running after the exit
}

static void startup_clock_wrong_exits_20_after_3s() {
    Harness h;
    h.ctl->start();
    h.completeAt(1000, withSkew(report(CycleResult::OnlineClockWrong), -7200));
    CHECK(h.screens == std::vector<Screen>{Screen::ConnectedClockWrong});
    CHECK(h.statuses == std::vector<std::string>{kStatusClockWrong});
    h.advanceTo(3999);
    CHECK(h.exits.empty());
    h.advanceTo(4000);
    CHECK(h.exits == std::vector<int>{kExitOnline});
    CHECK(h.exitTimes == std::vector<int64_t>{4000});
}

static void startup_no_network_autocloses_23_after_20s() {
    Harness h;
    h.ctl->start();
    h.completeAt(5000, report(CycleResult::NoNetwork));
    CHECK(h.screens == std::vector<Screen>{Screen::NoNetwork});
    CHECK(screenButtons(Screen::NoNetwork) == std::vector<Button>{Button::Close});
    CHECK(std::count(h.afterDelays.begin(), h.afterDelays.end(), 20000) == 1);
    h.advanceTo(24999);
    CHECK(h.exits.empty());
    h.advanceTo(25000);
    CHECK(h.exits == std::vector<int>{kExitNoNetwork});
    CHECK(h.exitTimes == std::vector<int64_t>{25000});
    CHECK(h.cycleStarts.size() == 1);
    CHECK(h.navs.empty());
}

static void startup_no_network_close_exits_23() {
    Harness h;
    h.ctl->start();
    h.completeAt(100, report(CycleResult::NoNetwork));
    h.advanceTo(1000);
    h.ctl->onButton(Button::Retry);            // not offered on this screen
    CHECK(h.cycleStarts.size() == 1);
    h.ctl->onButton(Button::Close);
    CHECK(h.exits == std::vector<int>{kExitNoNetwork});
    CHECK(h.exitTimes == std::vector<int64_t>{1000});
    h.advanceTo(30000);                        // the 20 s auto-close does not exit a second time
    CHECK(h.exits.size() == 1);
}

static void startup_limited_retry_runs_cycle_then_close_23() {
    Harness h;
    h.ctl->start();
    h.completeAt(100, report(CycleResult::Limited));
    CHECK(h.screens == std::vector<Screen>{Screen::Limited});
    CHECK(screenButtons(Screen::Limited) == (std::vector<Button>{Button::Retry, Button::Close}));
    h.advanceTo(2000);
    h.ctl->onButton(Button::Retry);
    CHECK(h.cycleStarts == (std::vector<int64_t>{0, 2000}));
    h.completeAt(2600, report(CycleResult::Limited));
    CHECK(h.screens == std::vector<Screen>{Screen::Limited});    // same screen: not shown again
    CHECK(h.exits.empty());
    h.ctl->onButton(Button::Close);
    CHECK(h.exits == std::vector<int>{kExitNoNetwork});
    CHECK(h.ctl->scheduler().dueAtMs() == kNever);             // never armed on a start-up screen
    CHECK(h.navs.empty());
    CHECK(!h.overlap);
}

static void startup_partial_use_as_is_24() {
    Harness h;
    h.ctl->start();
    h.completeAt(100, report(CycleResult::Partial));
    CHECK(h.screens == std::vector<Screen>{Screen::Partial});
    CHECK(screenButtons(Screen::Partial) == (std::vector<Button>{Button::Retry, Button::UseAsIs, Button::Close}));
    CHECK(h.statuses == std::vector<std::string>{kStatusSecureFailing});
    h.advanceTo(900);
    h.ctl->onButton(Button::UseAsIs);
    CHECK(h.exits == std::vector<int>{kExitUseAsIs});
    CHECK(h.exitTimes == std::vector<int64_t>{900});
}

static void startup_captive_navigates_to_view_url() {
    Harness h;
    h.ctl->start();
    CHECK(h.ctl->scheduler().dueAtMs() == kNever);             // disarmed during the start-up cycle
    h.completeAt(100, captive(kG, kPortal));
    CHECK(h.events == (std::vector<std::string>{"cycle", "screen:None", "nav:" + kG}));
    CHECK(h.navs == std::vector<std::string>{kG});
    CHECK(h.armedAtNavigate == std::vector<bool>{false});      // navigate first, then arm
    CHECK(h.ctl->scheduler().dueAtMs() == 10100);              // armed at 100: idle re-probe 10 s later
    CHECK(h.exits.empty());
}

static void never_views_probe_ok_url() {
    {   // G answers 204 (probe-ok), A is redirected (captive): the view goes to A, never to G
        HttpReply g;
        g.transportOk = true; g.status = 204;
        HttpReply a;
        a.transportOk = true; a.status = 302; a.location = kPortal;
        CycleReport r;
        r.outcome = combineCycle(true, classifyProbe(ProbeKind::Google204, kG, g), classifyProbe(ProbeKind::AppleSuccess, kA, a),
                                 TlsConfirm::OtherFailure);
        CHECK(r.outcome.result == CycleResult::Captive);
        CHECK(r.outcome.viewUrl == kA);
        Harness h;
        h.ctl->start();
        h.completeAt(100, r);
        CHECK(h.navs == std::vector<std::string>{kA});
        h.advanceTo(500);
        h.ctl->onNavigationDecision(kA);
        h.ctl->onLoadEnd(kA);                  // the view stayed on A: fall back to A's Location
        CHECK(h.navs == (std::vector<std::string>{kA, kPortal}));
        h.ctl->onLoadEnd(kG);                  // a load end on the probe-ok URL is no reason to go anywhere
        CHECK(h.navs == (std::vector<std::string>{kA, kPortal}));
        for (const std::string& u : h.navs) CHECK(u != kG);
    }
    {   // A answers the Success page (probe-ok), G is redirected (captive): the view goes to G, never to A
        HttpReply g;
        g.transportOk = true; g.status = 302; g.location = kPortal;
        HttpReply a;
        a.transportOk = true; a.status = 200; a.body = kAppleSuccessBody;
        CycleReport r;
        r.outcome = combineCycle(true, classifyProbe(ProbeKind::Google204, kG, g), classifyProbe(ProbeKind::AppleSuccess, kA, a),
                                 TlsConfirm::OtherFailure);
        CHECK(r.outcome.viewUrl == kG);
        Harness h;
        h.ctl->start();
        h.completeAt(100, r);
        CHECK(h.navs == std::vector<std::string>{kG});
    }
}

static void location_fallback_when_view_stays_on_probe_host() {
    Harness h;
    showPortal(h, kG, kPortal);
    h.advanceTo(400);
    h.ctl->onNavigationDecision(kG);
    h.ctl->onLoadEnd(kG);                      // the load ended on the probe URL itself (empty body or an error)
    CHECK(h.navs == (std::vector<std::string>{kG, kPortal}));
    h.ctl->onLoadEnd(kG);                      // only the first load end after navigate(viewUrl) may fall back
    CHECK(h.navs.size() == 2);
    CHECK(h.exits.empty());
}

static void no_fallback_without_location() {
    Harness h;
    showPortal(h, kG, "");
    h.advanceTo(400);
    h.ctl->onNavigationDecision(kG);
    h.ctl->onLoadEnd(kG);
    CHECK(h.navs == std::vector<std::string>{kG});
}

static void page_event_then_online_shows_connected_exits_20() {
    Harness h;
    showPortal(h, kG, "");
    h.advanceTo(3500);
    h.ctl->onLoadEnd(kPortal);
    CHECK(h.cycleStarts == (std::vector<int64_t>{0, 3500}));
    h.completeAt(3700, report(CycleResult::Online));
    CHECK(h.screens == (std::vector<Screen>{Screen::None, Screen::Connected}));
    CHECK(h.ctl->scheduler().dueAtMs() == kNever);   // disarmed while the exit is pending
    h.advanceTo(5699);
    CHECK(h.exits.empty());
    h.advanceTo(5700);
    CHECK(h.exits == std::vector<int>{kExitOnline});
    h.advanceTo(700000);
    CHECK(h.exits.size() == 1);
    CHECK(h.cycleStarts.size() == 2);
    CHECK(h.timers.empty());                   // the tick stopped
}

static void partial_while_shown_sets_status_keeps_page() {
    Harness h;
    showPortal(h, kG, kPortal);
    reprobeAt(h, 4000, report(CycleResult::Partial));
    CHECK(h.statuses == std::vector<std::string>{kStatusSecureFailing});
    CHECK(h.screens == std::vector<Screen>{Screen::None});
    CHECK(h.navs == std::vector<std::string>{kG});
    CHECK(h.exits.empty());
    h.advanceTo(15000);                        // keeps re-probing: idle re-probe 10 s after the finish at 4100
    CHECK(h.cycleStarts.size() == 3);
    CHECK(h.cycleStarts.back() == 14100);
}

// Review Focus 4: no navigation, no exit, and cycles continue.
static void no_network_while_shown_keeps_page() {
    Harness h;
    showPortal(h, kG, kPortal);
    reprobeAt(h, 4000, report(CycleResult::NoNetwork));
    CHECK(h.navs == std::vector<std::string>{kG});
    CHECK(h.exits.empty());
    CHECK(h.screens == std::vector<Screen>{Screen::None});
    h.advanceTo(15000);
    CHECK(h.cycleStarts.size() == 3);          // the idle re-probe still runs
    h.completeAt(15100, report(CycleResult::Limited));
    h.advanceTo(26000);
    CHECK(h.cycleStarts.size() == 4);
    h.completeAt(26100, report(CycleResult::NoNetwork));
    h.advanceTo(37000);
    CHECK(h.cycleStarts.size() == 5);
    CHECK(h.navs == std::vector<std::string>{kG});
    CHECK(h.exits.empty());
    CHECK(h.screens == std::vector<Screen>{Screen::None});
    CHECK(h.statuses.empty());
    CHECK(!h.overlap);
}

// Review Focus 3: onLoadEnd every 500 ms for 60 s while shown, each cycle answering 100 ms after it starts.
static void refresh_storm_is_rate_limited() {
    Harness h;
    showPortal(h, kG, kPortal);
    h.autoCompleteMs = 100;
    h.autoReport = captive(kG, kPortal);
    const int64_t t0 = 5000, t1 = t0 + 60000;
    for (int64_t t = t0; t <= t1; t += 500) {
        h.advanceTo(t);
        h.ctl->onLoadEnd(kPortal);
    }
    h.advanceTo(t1 + 100);
    int n = 0;
    for (int64_t s : h.cycleStarts) if (s >= t0 && s <= t1) ++n;
    CHECK(n <= 21);
    CHECK(n >= 20);                            // rate-limited, not starved
    for (size_t i = 1; i < h.cycleStarts.size(); ++i) CHECK(h.cycleStarts[i] - h.cycleStarts[i - 1] >= 3000);
    CHECK(!h.overlap);
    CHECK(!h.inFlight());
    CHECK(schedulerIdle(*h.ctl, h.now));       // every scheduled cycle was finished
    CHECK(h.navs == std::vector<std::string>{kG});
}

static void probe_host_nav_runs_immediately() {
    Harness h;
    showPortal(h, kG, "");
    reprobeAt(h, 4000, captive());             // the view is on the portal's own host now
    h.advanceTo(5000);                         // 1 s after the last start: a page event would wait until 7000
    h.ctl->onNavigationDecision(kG);           // back to a probe host: the portal let the tablet through
    CHECK(h.cycleStarts.size() == 3);
    CHECK(h.cycleStarts.back() == 5000);
}

static void idle_timeout_exits_22() {
    {   // portal shown, re-probing every ~10 s, nobody touches the screen
        Harness h;
        showPortal(h);
        h.autoCompleteMs = 100;
        h.autoReport = captive();
        h.advanceTo(599999);
        CHECK(h.exits.empty());
        h.advanceTo(600000);
        CHECK(h.exits == std::vector<int>{kExitIdle});
        CHECK(h.exitTimes == std::vector<int64_t>{600000});
        CHECK(h.cycleStarts.size() > 50);
    }
    {   // a start-up screen
        Harness h;
        h.ctl->start();
        h.completeAt(100, report(CycleResult::Limited));
        h.advanceTo(599999);
        CHECK(h.exits.empty());
        h.advanceTo(600000);
        CHECK(h.exits == std::vector<int>{kExitIdle});
    }
}

static void activity_resets_idle() {
    {
        Harness h;
        showPortal(h);
        h.autoCompleteMs = 100;
        h.autoReport = captive();
        h.advanceTo(300000);
        h.ctl->onUserActivity();
        h.advanceTo(600000);
        CHECK(h.exits.empty());
        h.advanceTo(899999);
        CHECK(h.exits.empty());
        h.advanceTo(900000);
        CHECK(h.exits == std::vector<int>{kExitIdle});
    }
    {   // on a start-up screen
        Harness h;
        h.ctl->start();
        h.completeAt(100, report(CycleResult::Partial));
        h.advanceTo(400000);
        h.ctl->onUserActivity();
        h.advanceTo(999999);
        CHECK(h.exits.empty());
        h.advanceTo(1000000);
        CHECK(h.exits == std::vector<int>{kExitIdle});
    }
}

static void idle_override_5s_exits_22_at_5s() {
    const CaptiveConfig cfg = parseCaptiveConfig("idle_timeout_s = 5\n");
    CHECK(cfg.idleTimeoutS == 5);
    {   // portal shown
        Harness h(cfg);
        showPortal(h);
        h.advanceTo(4999);
        CHECK(h.exits.empty());
        h.advanceTo(5000);
        CHECK(h.exits == std::vector<int>{kExitIdle});
        CHECK(h.exitTimes == std::vector<int64_t>{5000});
    }
    {   // start-up screen
        Harness h(cfg);
        h.ctl->start();
        h.completeAt(100, report(CycleResult::Limited));
        h.advanceTo(4999);
        CHECK(h.exits.empty());
        h.advanceTo(5000);
        CHECK(h.exits == std::vector<int>{kExitIdle});
    }
    {   // the start-up cycle is still running at the deadline; its late answer is ignored
        Harness h(cfg);
        h.ctl->start();
        h.advanceTo(4999);
        CHECK(h.exits.empty());
        h.advanceTo(5000);
        CHECK(h.exits == std::vector<int>{kExitIdle});
        h.completeAt(9000, report(CycleResult::Online));
        CHECK(h.screens.empty());
        CHECK(h.exits.size() == 1);
    }
}

static void cancel_exits_21() {
    {   // during the start-up cycle
        Harness h;
        h.ctl->start();
        h.advanceTo(50);
        h.ctl->onButton(Button::Cancel);
        CHECK(h.exits == std::vector<int>{kExitCancelled});
        h.completeAt(60, report(CycleResult::Online));
        CHECK(h.screens.empty());
        CHECK(h.exits.size() == 1);
    }
    {   // portal shown
        Harness h;
        showPortal(h);
        h.advanceTo(1000);
        h.ctl->onButton(Button::Cancel);
        CHECK(h.exits == std::vector<int>{kExitCancelled});
        CHECK(h.exitTimes == std::vector<int64_t>{1000});
    }
    {   // start-up screens
        for (CycleResult res : {CycleResult::Limited, CycleResult::Partial, CycleResult::NoNetwork}) {
            Harness h;
            h.ctl->start();
            h.completeAt(100, report(res));
            h.ctl->onButton(Button::Cancel);
            CHECK(h.exits == std::vector<int>{kExitCancelled});
            h.advanceTo(30000);
            CHECK(h.exits.size() == 1);
        }
    }
}

static void use_as_is_exits_24() {
    {
        Harness h;
        showPortal(h);
        h.ctl->onButton(Button::UseAsIs);
        CHECK(h.exits == std::vector<int>{kExitUseAsIs});
    }
    {
        Harness h;
        h.ctl->start();
        h.ctl->onButton(Button::UseAsIs);
        CHECK(h.exits == std::vector<int>{kExitUseAsIs});
    }
    {   // the Limited screen does not list it, but the top bar's Use network as is works everywhere
        Harness h;
        h.ctl->start();
        h.completeAt(100, report(CycleResult::Limited));
        h.ctl->onButton(Button::UseAsIs);
        CHECK(h.exits == std::vector<int>{kExitUseAsIs});
    }
}

static void skew_sets_clock_status() {
    Harness h;
    showPortal(h, kG, "");
    reprobeAt(h, 4000, withSkew(captive(), 301));
    CHECK(h.statuses == std::vector<std::string>{kStatusClockWrong});
    reprobeAt(h, 8000, withSkew(captive(), -400));         // same text: no call
    CHECK(h.statuses.size() == 1);
    reprobeAt(h, 12000, withSkew(captive(), 300));         // 300 s is not "more than 5 minutes"
    CHECK(h.statuses == (std::vector<std::string>{kStatusClockWrong, ""}));
    {
        CycleReport r = captive();
        r.dateKnown = false;                                // no 204 from G: skewSec means nothing
        r.skewSec = 5000;
        reprobeAt(h, 16000, r);
    }
    CHECK(h.statuses.size() == 2);
    reprobeAt(h, 20000, withSkew(captive(), -301));
    CHECK(h.statuses.size() == 3);
    CHECK(h.statuses.back() == kStatusClockWrong);
    reprobeAt(h, 24000, withSkew(report(CycleResult::Partial), 1000));   // Partial wins
    CHECK(h.statuses.size() == 4);
    CHECK(h.statuses.back() == kStatusSecureFailing);
    CHECK(h.navs == std::vector<std::string>{kG});
    CHECK(h.exits.empty());
}

// Cancel (and every other button) after the exit timer is pending does not call exitWith again.
static void exit_called_once() {
    Harness h;
    showPortal(h);
    reprobeAt(h, 4000, report(CycleResult::Online));        // answers at 4100: Connected, exit 20 due at 6100
    CHECK(h.screens == (std::vector<Screen>{Screen::None, Screen::Connected}));
    h.advanceTo(4500);
    h.ctl->onButton(Button::Cancel);
    h.ctl->onButton(Button::UseAsIs);
    h.ctl->onButton(Button::Close);
    h.ctl->onButton(Button::Retry);
    CHECK(h.exits.empty());
    h.advanceTo(6099);
    CHECK(h.exits.empty());
    h.advanceTo(6100);
    CHECK(h.exits == std::vector<int>{kExitOnline});
    h.ctl->onButton(Button::Cancel);
    h.advanceTo(700000);
    CHECK(h.exits == std::vector<int>{kExitOnline});
    CHECK(h.cycleStarts.size() == 2);
}

// ===================================================================================================================
// Rulings 1-8
// ===================================================================================================================

// Ruling 1: never two cycles at once.
static void retry_during_cycle_is_ignored() {
    Harness h;
    h.ctl->start();
    h.completeAt(100, report(CycleResult::Limited));
    h.advanceTo(1000);
    h.ctl->onButton(Button::Retry);
    CHECK(h.cycleStarts.size() == 2);
    h.advanceTo(1200);
    h.ctl->onButton(Button::Retry);            // the Retry cycle is still running
    CHECK(h.cycleStarts.size() == 2);
    CHECK(!h.overlap);
    h.completeAt(1500, report(CycleResult::Partial));
    CHECK(h.screens == (std::vector<Screen>{Screen::Limited, Screen::Partial}));
    h.ctl->onButton(Button::Retry);
    CHECK(h.cycleStarts.size() == 3);
    CHECK(!h.overlap);
}

// Ruling 1: start-up and Retry cycles run while the scheduler is disarmed and never mark it in flight.
static void startup_and_retry_cycles_never_touch_scheduler() {
    Harness h;
    h.ctl->start();
    CHECK(h.ctl->scheduler().dueAtMs() == kNever);
    CHECK(schedulerIdle(*h.ctl, h.now));
    h.completeAt(100, report(CycleResult::Limited));
    h.advanceTo(1000);
    h.ctl->onButton(Button::Retry);
    CHECK(h.ctl->scheduler().dueAtMs() == kNever);
    CHECK(schedulerIdle(*h.ctl, h.now));
    h.completeAt(1500, captive(kG, ""));       // the Retry result goes through the start-up table
    CHECK(h.screens == (std::vector<Screen>{Screen::Limited, Screen::None}));
    CHECK(h.navs == std::vector<std::string>{kG});
    CHECK(h.ctl->scheduler().dueAtMs() == 11500);   // armed at 1500 with nothing in flight
}

// Ruling 1: the 500 ms tick runs only while the portal is shown.
static void tick_runs_only_while_portal_shown() {
    Harness h;
    h.ctl->start();
    CHECK(h.ticksScheduled() == 0);
    h.completeAt(100, report(CycleResult::Limited));
    h.advanceTo(60000);
    CHECK(h.ticksScheduled() == 0);
    CHECK(h.cycleStarts.size() == 1);
    h.ctl->onButton(Button::Retry);
    h.completeAt(60100, captive());
    h.advanceTo(61100);
    CHECK(h.ticksScheduled() == 3);            // armed at 60100, fired at 60600 and 61100
}

// Ruling 1 + Task 4's hard rule: a scheduled cycle that answers after an exit still ends with cycleFinished(), and
// nothing else happens.
static void scheduler_cycle_after_exit_only_finishes() {
    Harness h;
    showPortal(h, kG, kPortal);
    h.advanceTo(4000);
    h.ctl->onLoadEnd(kPortal);
    CHECK(h.cycleStarts.size() == 2);
    CHECK(!schedulerIdle(*h.ctl, h.now));      // the scheduler counts this cycle as in flight
    h.advanceTo(4100);
    h.ctl->onButton(Button::Cancel);
    CHECK(h.exits == std::vector<int>{kExitCancelled});
    CHECK(!schedulerIdle(*h.ctl, h.now));      // disarm() does not end the cycle
    const size_t events = h.events.size();
    h.completeAt(6000, withSkew(report(CycleResult::Online), 1000));
    CHECK(h.events.size() == events);          // no screen, status, navigation or second exit
    CHECK(schedulerIdle(*h.ctl, h.now));       // but cycleFinished() ran
}

// Spec §5 "Cycles are at least 3 s apart": the first scheduled re-probe also waits 3 s from the start-up cycle's start;
// a ProbeHostNav ignores that gap as it ignores the scheduler's own.
static void first_reprobe_keeps_gap_after_startup_cycle() {
    {
        Harness h;
        showPortal(h, kG, kPortal);            // start-up cycle started at 0
        h.advanceTo(200);
        h.ctl->onNavigationDecision(kG);
        h.advanceTo(1000);
        h.ctl->onLoadEnd(kPortal);
        h.advanceTo(2999);
        CHECK(h.cycleStarts.size() == 1);
        h.advanceTo(3100);
        CHECK(h.cycleStarts.size() == 2);
        CHECK(h.cycleStarts.back() == 3100);   // the first tick at or after 3000
    }
    {
        Harness h;
        showPortal(h, kG, kPortal);
        h.advanceTo(200);
        h.ctl->onLoadEnd(kPortal);             // the view is on the portal's own host
        CHECK(h.cycleStarts.size() == 1);
        h.advanceTo(300);
        h.ctl->onNavigationDecision(kA);       // a return to a probe host
        CHECK(h.cycleStarts.size() == 2);
        CHECK(h.cycleStarts.back() == 300);
    }
    {   // the same gap after a Retry cycle
        Harness h;
        h.ctl->start();
        h.completeAt(100, report(CycleResult::Limited));
        h.advanceTo(1000);
        h.ctl->onButton(Button::Retry);        // started at 1000: no scheduled cycle before 4000
        h.completeAt(1200, captive(kG, ""));   // ticks at 1700, 2200, ... 3700, 4200
        h.advanceTo(2000);
        h.ctl->onLoadEnd(kPortal);
        h.advanceTo(3999);
        CHECK(h.cycleStarts.size() == 2);
        h.advanceTo(4200);
        CHECK(h.cycleStarts.size() == 3);
        CHECK(h.cycleStarts.back() == 4200);
    }
}

// A `done` that arrives again (or for a cycle that already ended) changes nothing.
static void repeated_done_is_ignored() {
    Harness h;
    showPortal(h, kG, kPortal);
    h.advanceTo(4000);
    h.ctl->onLoadEnd(kPortal);
    CHECK(h.inFlight());
    const std::function<void(const CycleReport&)> again = h.pendingDone;
    h.completeAt(4100, captive());
    const size_t events = h.events.size();
    again(report(CycleResult::Online));
    CHECK(h.events.size() == events);
    CHECK(h.exits.empty());
    CHECK(schedulerIdle(*h.ctl, h.now));
    h.advanceTo(7000);
    h.ctl->onLoadEnd(kPortal);                 // the controller still runs its next cycle normally
    CHECK(h.cycleStarts.size() == 3);
    CHECK(h.cycleStarts.back() == 7000);
}

// Ruling 2: the status line follows the latest cycle; Partial's text is cleared by the next non-Partial result.
static void status_cleared_after_partial() {
    {
        Harness h;
        showPortal(h);
        reprobeAt(h, 4000, report(CycleResult::Partial));
        CHECK(h.statuses == std::vector<std::string>{kStatusSecureFailing});
        reprobeAt(h, 8000, captive());
        CHECK(h.statuses == (std::vector<std::string>{kStatusSecureFailing, ""}));
        reprobeAt(h, 12000, captive());
        CHECK(h.statuses.size() == 2);
    }
    {   // also on start-up screens
        Harness h;
        h.ctl->start();
        h.completeAt(100, report(CycleResult::Partial));
        h.ctl->onButton(Button::Retry);
        h.completeAt(1000, report(CycleResult::Limited));
        CHECK(h.screens == (std::vector<Screen>{Screen::Partial, Screen::Limited}));
        CHECK(h.statuses == (std::vector<std::string>{kStatusSecureFailing, ""}));
    }
}

// Ruling 3: Close and Retry only on a start-up screen that offers them.
static void close_and_retry_only_on_screens_that_offer_them() {
    {   // start-up cycle running, no screen yet
        Harness h;
        h.ctl->start();
        h.ctl->onButton(Button::Close);
        h.ctl->onButton(Button::Retry);
        CHECK(h.exits.empty());
        CHECK(h.cycleStarts.size() == 1);
    }
    {   // portal shown
        Harness h;
        showPortal(h);
        h.advanceTo(5000);
        h.ctl->onButton(Button::Close);
        h.ctl->onButton(Button::Retry);
        CHECK(h.exits.empty());
        CHECK(h.cycleStarts.size() == 1);
    }
    {   // AlreadyOpen offers neither (and its exit is pending)
        Harness h;
        h.ctl->start();
        h.completeAt(100, report(CycleResult::Online));
        h.ctl->onButton(Button::Close);
        h.ctl->onButton(Button::Retry);
        CHECK(h.cycleStarts.size() == 1);
        h.advanceTo(2100);
        CHECK(h.exits == std::vector<int>{kExitOnline});
    }
    {   // Partial offers Close
        Harness h;
        h.ctl->start();
        h.completeAt(100, report(CycleResult::Partial));
        h.ctl->onButton(Button::Close);
        CHECK(h.exits == std::vector<int>{kExitNoNetwork});
    }
}

// Ruling 3: after an exit nothing else runs.
static void nothing_runs_after_exit() {
    Harness h;
    showPortal(h, kG, kPortal);
    h.advanceTo(1000);
    h.ctl->onButton(Button::Cancel);
    const long ticks = h.ticksScheduled();
    h.ctl->onNavigationDecision(kPortal);
    h.ctl->onNavigationDecision(kG);
    h.ctl->onLoadEnd(kG);
    h.ctl->onSubmitLike();
    h.ctl->onUserActivity();
    h.ctl->onButton(Button::UseAsIs);
    h.ctl->onButton(Button::Retry);
    h.ctl->onButton(Button::Close);
    h.ctl->start();
    h.advanceTo(700000);
    CHECK(h.exits == std::vector<int>{kExitCancelled});
    CHECK(h.cycleStarts.size() == 1);
    CHECK(h.navs == std::vector<std::string>{kG});
    CHECK(h.screens == std::vector<Screen>{Screen::None});
    CHECK(h.ticksScheduled() == ticks);        // the tick pending at the exit fired once and did not re-arm
    CHECK(h.timers.empty());
}

// Rulings 4 / R20: probe hosts are host[:port] of gUrl and aUrl, compared case-insensitively.
static void probe_hosts_compare_host_and_port_case_insensitively() {
    const CaptiveConfig cfg = parseCaptiveConfig("probe_google = http://127.0.0.1:18080/generate_204\n"
                                                 "probe_apple = http://Captive.Example.test:18081/hotspot-detect.html\n");
    Harness h(cfg);
    showPortal(h, cfg.gUrl, "");
    h.advanceTo(4000);
    h.ctl->onLoadEnd("http://127.0.0.1:18082/portal");               // same address, other port: NOT a probe host
    CHECK(h.cycleStarts.size() == 2);
    h.completeAt(4100, captive(cfg.gUrl));
    h.advanceTo(5000);
    h.ctl->onNavigationDecision("http://CAPTIVE.example.TEST:18081/x");   // probe host A in other letter case
    CHECK(h.cycleStarts.size() == 3);
    CHECK(h.cycleStarts.back() == 5000);       // a return: ProbeHostNav, immediate, inside the 3 s gap
    h.completeAt(5100, captive(cfg.gUrl));
    h.advanceTo(6000);
    h.ctl->onNavigationDecision("http://127.0.0.1:18080/");          // probe host G, but no load end since the
    CHECK(h.cycleStarts.size() == 3);                                 // last ProbeHostNav: a plain PageEvent
    h.advanceTo(8100);
    CHECK(h.cycleStarts.size() == 4);
    CHECK(h.cycleStarts.back() == 8100);       // deferred to the gap's end (8000), started on the next tick
}

// R20: a navigation to a probe host is a ProbeHostNav only when the view's current host (from load ends, never from
// decisions — those include sub-frames) is a non-probe host.
static void probe_host_nav_needs_view_to_leave_probe_hosts() {
    Harness h;
    showPortal(h, kG, "");
    h.advanceTo(4000);
    h.ctl->onLoadEnd(kG);                      // the view is still on the probe host
    CHECK(h.cycleStarts.size() == 2);
    h.completeAt(4100, captive());
    h.advanceTo(5000);
    h.ctl->onNavigationDecision(kA);           // another probe host, but the view never left: a plain PageEvent
    CHECK(h.cycleStarts.size() == 2);
    h.advanceTo(5200);
    h.ctl->onNavigationDecision("http://ads.example.org/frame");   // a third-party iframe: decisions teach no host
    h.advanceTo(5400);
    h.ctl->onNavigationDecision(kG);
    CHECK(h.cycleStarts.size() == 2);
    h.advanceTo(7100);
    CHECK(h.cycleStarts.size() == 3);
    CHECK(h.cycleStarts.back() == 7100);       // deferred to the gap's end (7000), started on the next tick
}

// R20 (review Important 1): one return gets one immediate cycle; a stream of decisions to the probe host with no load
// end in between never breaks the 3 s gap again.
static void probe_host_decision_storm_keeps_gap() {
    Harness h;
    showPortal(h, kG, kPortal);
    h.autoCompleteMs = 100;
    h.autoReport = captive(kG, kPortal);
    h.advanceTo(4000);
    h.ctl->onLoadEnd(kPortal);                 // the view is on the portal's host; a PageEvent cycle at 4000
    CHECK(h.cycleStarts.size() == 2);
    h.advanceTo(5000);
    h.ctl->onNavigationDecision(kG);           // the return
    CHECK(h.cycleStarts.size() == 3);
    CHECK(h.cycleStarts.back() == 5000);       // immediate, 1 s after the previous start
    for (int64_t t = 5500; t <= 35000; t += 500) {
        h.advanceTo(t);
        h.ctl->onNavigationDecision(kG);
    }
    h.advanceTo(35100);
    for (size_t i = 3; i < h.cycleStarts.size(); ++i) CHECK(h.cycleStarts[i] - h.cycleStarts[i - 1] >= 3000);
    int n = 0;
    for (int64_t s : h.cycleStarts) if (s >= 5000 && s <= 35000) ++n;
    CHECK(n <= 11);                            // 5000, 8000, ... 35000
    CHECK(n >= 10);
    CHECK(!h.overlap);
}

// R20: a second return is immediate again only after a new load end on a non-probe host.
static void second_return_needs_new_load_on_portal_host() {
    Harness h;
    showPortal(h, kG, "");
    reprobeAt(h, 4000, captive());             // load end on the portal's host at 4000
    h.advanceTo(5000);
    h.ctl->onNavigationDecision(kG);           // first return: immediate
    CHECK(h.cycleStarts.size() == 3);
    CHECK(h.cycleStarts.back() == 5000);
    h.completeAt(5100, captive());
    h.advanceTo(5600);
    h.ctl->onNavigationDecision(kG);           // no load end since: PageEvent
    h.advanceTo(5800);
    h.ctl->onLoadEnd(kG);                      // a load end, but on the probe host
    h.advanceTo(6000);
    h.ctl->onNavigationDecision(kA);           // the view is on a probe host: PageEvent
    CHECK(h.cycleStarts.size() == 3);
    h.advanceTo(8100);                         // the deferred PageEvents start on the first tick after 8000
    CHECK(h.cycleStarts.size() == 4);
    CHECK(h.cycleStarts.back() == 8100);
    h.completeAt(8200, captive());
    h.advanceTo(9000);
    h.ctl->onLoadEnd(kPortal);                 // back on the portal's host (gap until 11100)
    CHECK(h.cycleStarts.size() == 4);
    h.advanceTo(9500);
    h.ctl->onNavigationDecision(kA);           // second return: immediate again
    CHECK(h.cycleStarts.size() == 5);
    CHECK(h.cycleStarts.back() == 9500);
}

// Ruling 4: decisions and load ends while the portal is not shown are ignored, and teach nothing about hosts.
static void page_events_ignored_until_portal_shown() {
    {
        Harness h;
        h.ctl->start();
        h.advanceTo(50);
        h.ctl->onNavigationDecision(kPortal);
        h.ctl->onLoadEnd(kPortal);
        h.ctl->onSubmitLike();
        CHECK(h.cycleStarts.size() == 1);
        h.completeAt(100, captive(kG, ""));
        h.advanceTo(4000);
        h.ctl->onNavigationDecision(kG);       // a PageEvent; no load end since the portal was shown
        CHECK(h.cycleStarts.size() == 2);
        h.completeAt(4100, captive());
        h.advanceTo(5000);
        h.ctl->onNavigationDecision(kA);       // the pre-portal load end on the portal's host was not learned
        CHECK(h.cycleStarts.size() == 2);
    }
    {
        Harness h;
        h.ctl->start();
        h.completeAt(100, report(CycleResult::Limited));
        h.advanceTo(5000);
        h.ctl->onNavigationDecision(kPortal);
        h.ctl->onLoadEnd(kG);
        h.ctl->onSubmitLike();
        h.advanceTo(60000);
        CHECK(h.cycleStarts.size() == 1);
        CHECK(h.navs.empty());
        CHECK(h.screens == std::vector<Screen>{Screen::Limited});
    }
}

// Rulings 5 / R21: the fallback needs the first load end to be exactly the view URL, not another page of the probe
// host; a first load end anywhere else just disarms it.
static void fallback_not_on_same_host_different_path() {
    for (const std::string& other : {std::string("http://connectivitycheck.gstatic.com/other"), kG + "?x=1",
                                     std::string("https://connectivitycheck.gstatic.com/generate_204")}) {
        Harness h;
        showPortal(h, kG, kPortal);
        h.advanceTo(400);
        h.ctl->onLoadEnd(other);
        CHECK(h.navs == std::vector<std::string>{kG});
        h.ctl->onLoadEnd(kG);                  // too late: the first load end consumed the fallback
        CHECK(h.navs == std::vector<std::string>{kG});
    }
    {
        Harness h;
        showPortal(h, kG, kPortal);
        h.advanceTo(400);
        h.ctl->onLoadEnd("http://CONNECTIVITYCHECK.gstatic.com/generate_204#top");   // the same URL (host case, fragment)
        CHECK(h.navs == (std::vector<std::string>{kG, kPortal}));
    }
}

// R21: armed only by the start-up navigate(viewUrl), consumed by its first load end; shown results never re-arm it,
// and a non-Captive result disarms it.
static void fallback_only_on_first_load_after_navigate() {
    {   // the redirect worked: the first load end is the portal's page, and later probe-URL loads never fall back
        Harness h;
        showPortal(h, kG, kPortal);
        h.advanceTo(400);
        h.ctl->onLoadEnd(kPortal);
        h.ctl->onLoadEnd(kG);
        CHECK(h.navs == std::vector<std::string>{kG});
    }
    {   // a Captive result while shown does not re-arm it
        Harness h;
        showPortal(h, kG, kPortal);
        h.advanceTo(400);
        h.ctl->onLoadEnd(kG);
        CHECK(h.navs == (std::vector<std::string>{kG, kPortal}));
        answerNextCycle(h, captive(kG, kPortal));
        h.ctl->onLoadEnd(kG);
        CHECK(h.navs.size() == 2);
    }
    {   // a Captive result before the first load end leaves it armed
        Harness h;
        showPortal(h, kG, kPortal);
        h.advanceTo(200);
        h.ctl->onNavigationDecision(kG);
        answerNextCycle(h, captive(kA, ""));   // started at 3100 (3 s after the start-up cycle)
        h.ctl->onLoadEnd(kG);
        CHECK(h.navs == (std::vector<std::string>{kG, kPortal}));
    }
    {   // a non-Captive result before the first load end disarms it
        for (CycleResult res : {CycleResult::Limited, CycleResult::NoNetwork, CycleResult::Partial}) {
            Harness h;
            showPortal(h, kG, kPortal);
            h.advanceTo(200);
            h.ctl->onNavigationDecision(kG);
            answerNextCycle(h, report(res));
            h.ctl->onLoadEnd(kG);
            CHECK(h.navs == std::vector<std::string>{kG});
            CHECK(h.exits.empty());
        }
    }
    {   // a Retry that answers Captive navigates, so it arms it too
        Harness h;
        h.ctl->start();
        h.completeAt(100, report(CycleResult::Limited));
        h.ctl->onButton(Button::Retry);
        h.completeAt(600, captive(kA, kPortal));
        h.advanceTo(900);
        h.ctl->onLoadEnd(kA);
        CHECK(h.navs == (std::vector<std::string>{kA, kPortal}));
    }
}

// R21: after the login the portal sends the view back to the probe URL; that load end must not fall back to the
// portal's login page.
static void fallback_not_after_login_redirect_back_to_probe() {
    Harness h;
    showPortal(h, kG, kPortal);
    h.autoCompleteMs = 100;
    h.autoReport = captive(kG, kPortal);
    h.advanceTo(400);
    h.ctl->onNavigationDecision(kPortal);      // WebKit followed the portal's redirect
    h.ctl->onLoadEnd(kPortal);                 // the login page is shown
    CHECK(h.navs == std::vector<std::string>{kG});
    h.advanceTo(20000);
    h.ctl->onUserActivity();
    h.ctl->onSubmitLike();                     // the user signs in
    h.ctl->onNavigationDecision(kPortal + "&done=1");
    h.ctl->onLoadEnd(kPortal + "&done=1");
    h.advanceTo(21000);
    const size_t before = h.cycleStarts.size();
    h.ctl->onNavigationDecision(kG);           // the portal redirects back to G: a return, probed at once
    CHECK(h.cycleStarts.size() == before + 1);
    CHECK(h.cycleStarts.back() == 21000);
    h.ctl->onLoadEnd(kG);                      // the probe URL loads (204 or a "success" page)
    CHECK(h.navs == std::vector<std::string>{kG});   // no fallback to the login page
    h.autoReport = report(CycleResult::Online);
    h.advanceTo(21100);                        // the return's cycle answers Online
    CHECK(h.screens == (std::vector<Screen>{Screen::None, Screen::Connected}));
    h.advanceTo(23100);
    CHECK(h.exits == std::vector<int>{kExitOnline});
    CHECK(h.navs == std::vector<std::string>{kG});
}

// Ruling 7: a Captive result while the portal is shown keeps the page.
static void shown_captive_does_not_renavigate() {
    Harness h;
    showPortal(h, kG, "");
    reprobeAt(h, 4000, captive(kA, kPortal));
    CHECK(h.navs == std::vector<std::string>{kG});
    CHECK(h.screens == std::vector<Screen>{Screen::None});
    CHECK(h.exits.empty());
    CHECK(h.ctl->scheduler().dueAtMs() != kNever);   // still armed
}

// Ruling 8: OnlineClockWrong while shown waits 2 s (not the start-up 3 s).
static void shown_clock_wrong_exits_20_after_2s() {
    Harness h;
    showPortal(h);
    reprobeAt(h, 4000, withSkew(report(CycleResult::OnlineClockWrong), 3600));
    CHECK(h.screens == (std::vector<Screen>{Screen::None, Screen::ConnectedClockWrong}));
    CHECK(h.statuses == std::vector<std::string>{kStatusClockWrong});
    CHECK(h.ctl->scheduler().dueAtMs() == kNever);
    h.advanceTo(6099);
    CHECK(h.exits.empty());
    h.advanceTo(6100);
    CHECK(h.exits == std::vector<int>{kExitOnline});
}

// Ruling 8 (start-up table through Retry): Online from a Retry is AlreadyOpen, NoNetwork from a Retry auto-closes.
static void retry_result_uses_startup_table() {
    {
        Harness h;
        h.ctl->start();
        h.completeAt(100, report(CycleResult::Partial));
        h.ctl->onButton(Button::Retry);
        h.completeAt(500, report(CycleResult::Online));
        CHECK(h.screens == (std::vector<Screen>{Screen::Partial, Screen::AlreadyOpen}));
        h.advanceTo(2499);
        CHECK(h.exits.empty());
        h.advanceTo(2500);
        CHECK(h.exits == std::vector<int>{kExitOnline});
    }
    {
        Harness h;
        h.ctl->start();
        h.completeAt(100, report(CycleResult::Limited));
        h.ctl->onButton(Button::Retry);
        h.completeAt(500, report(CycleResult::NoNetwork));
        CHECK(h.screens == (std::vector<Screen>{Screen::Limited, Screen::NoNetwork}));
        h.advanceTo(20499);
        CHECK(h.exits.empty());
        h.advanceTo(20500);
        CHECK(h.exits == std::vector<int>{kExitNoNetwork});
    }
}

// A submit, keyboard Go or control tap asks for a re-probe 5 s later.
static void submit_like_reprobes_after_5s() {
    Harness h;
    showPortal(h, kG, kPortal);
    reprobeAt(h, 4000, captive());             // finished at 4100: the idle re-probe is due at 14100
    h.advanceTo(6000);
    h.ctl->onSubmitLike();                     // due at 11000
    h.advanceTo(10999);
    CHECK(h.cycleStarts.size() == 2);
    h.advanceTo(11100);
    CHECK(h.cycleStarts.size() == 3);
    CHECK(h.cycleStarts.back() == 11100);      // the first tick at or after 11000
}

// Timers and the probe cycle may outlive the controller: their callbacks then do nothing.
static void destroyed_controller_ignores_late_callbacks() {
    Harness h;
    showPortal(h, kG, kPortal);
    h.advanceTo(4000);
    h.ctl->onLoadEnd(kPortal);
    CHECK(h.inFlight());
    const size_t events = h.events.size();
    h.ctl.reset();
    h.advanceTo(700000);                       // the tick and the idle watchdog fire into a dead controller
    h.complete(report(CycleResult::Online));
    CHECK(h.events.size() == events);
    CHECK(h.timers.empty());
}

static void screen_text_and_buttons_match_spec() {
    CHECK(std::string(screenText(Screen::None)) == "");
    CHECK(std::string(screenText(Screen::AlreadyOpen)) == "This network is already open");
    CHECK(std::string(screenText(Screen::ConnectedClockWrong)) == "Connected. The tablet clock looks wrong.");
    CHECK(std::string(screenText(Screen::NoNetwork)) == "Not connected to Wi-Fi. Join the network in Settings first.");
    CHECK(std::string(screenText(Screen::Limited)) == "This network is not responding to sign-in checks");
    CHECK(std::string(screenText(Screen::Partial)) == "Web pages load but secure connections fail on this network");
    CHECK(std::string(screenText(Screen::Connected)) == "Connected");
    CHECK(std::string(kStatusSecureFailing) == "Secure connections still failing");
    CHECK(std::string(kStatusClockWrong) == "Tablet clock looks wrong");
    CHECK(screenButtons(Screen::NoNetwork) == std::vector<Button>{Button::Close});
    CHECK(screenButtons(Screen::Limited) == (std::vector<Button>{Button::Retry, Button::Close}));
    CHECK(screenButtons(Screen::Partial) == (std::vector<Button>{Button::Retry, Button::UseAsIs, Button::Close}));
    for (Screen s : {Screen::None, Screen::AlreadyOpen, Screen::ConnectedClockWrong, Screen::Connected})
        CHECK(screenButtons(s).empty());
    CHECK(CaptiveController::kTickMs == 500);
    CHECK(kG == CaptiveConfig{}.gUrl);
    CHECK(kA == CaptiveConfig{}.aUrl);
}

static void host_and_url_keys() {
    using detail::ctlHostKey;
    using detail::ctlUrlKey;
    CHECK(ctlHostKey("HTTP://User:pw@Example.COM:8080/p?q#f") == "example.com:8080");
    CHECK(ctlHostKey("http://h.test:80/") == "h.test");
    CHECK(ctlHostKey("https://h.test:443") == "h.test");
    CHECK(ctlHostKey("http://h.test:443/") == "h.test:443");
    CHECK(ctlHostKey("http://h.test:/") == "h.test");
    CHECK(ctlHostKey("http://[::1]:8080/x") == "[::1]:8080");
    CHECK(ctlHostKey("http://[::1]/x") == "[::1]");
    CHECK(ctlHostKey("http://h.test?x") == "h.test");
    CHECK(ctlHostKey("http://h.test#x") == "h.test");
    CHECK(ctlHostKey("about:blank") == "");
    CHECK(ctlHostKey("rmweb:tls-continue") == "");
    CHECK(ctlHostKey("data:text/html,http://x.test/") == "");
    CHECK(ctlHostKey("file:///tmp/x") == "");
    CHECK(ctlHostKey("") == "");
    CHECK(ctlUrlKey("HTTP://H.test/a?b#c") == ctlUrlKey("http://h.test/a?b"));
    CHECK(ctlUrlKey("http://h.test") == ctlUrlKey("http://h.test/"));
    CHECK(ctlUrlKey("http://h.test:80/a") == ctlUrlKey("http://h.test/a"));
    CHECK(ctlUrlKey("http://h.test/a") != ctlUrlKey("http://h.test/A"));    // path and query stay exact
    CHECK(ctlUrlKey("http://h.test/a") != ctlUrlKey("http://h.test/a?x"));
    CHECK(ctlUrlKey("http://h.test/a") != ctlUrlKey("https://h.test/a"));
}

int main() {
    startup_online_exits_20_after_2s();
    startup_clock_wrong_exits_20_after_3s();
    startup_no_network_autocloses_23_after_20s();
    startup_no_network_close_exits_23();
    startup_limited_retry_runs_cycle_then_close_23();
    startup_partial_use_as_is_24();
    startup_captive_navigates_to_view_url();
    never_views_probe_ok_url();
    location_fallback_when_view_stays_on_probe_host();
    no_fallback_without_location();
    page_event_then_online_shows_connected_exits_20();
    partial_while_shown_sets_status_keeps_page();
    no_network_while_shown_keeps_page();
    refresh_storm_is_rate_limited();
    probe_host_nav_runs_immediately();
    idle_timeout_exits_22();
    activity_resets_idle();
    idle_override_5s_exits_22_at_5s();
    cancel_exits_21();
    use_as_is_exits_24();
    skew_sets_clock_status();
    exit_called_once();

    retry_during_cycle_is_ignored();
    startup_and_retry_cycles_never_touch_scheduler();
    tick_runs_only_while_portal_shown();
    scheduler_cycle_after_exit_only_finishes();
    first_reprobe_keeps_gap_after_startup_cycle();
    repeated_done_is_ignored();
    status_cleared_after_partial();
    close_and_retry_only_on_screens_that_offer_them();
    nothing_runs_after_exit();
    probe_hosts_compare_host_and_port_case_insensitively();
    probe_host_nav_needs_view_to_leave_probe_hosts();
    probe_host_decision_storm_keeps_gap();
    second_return_needs_new_load_on_portal_host();
    page_events_ignored_until_portal_shown();
    fallback_not_on_same_host_different_path();
    fallback_only_on_first_load_after_navigate();
    fallback_not_after_login_redirect_back_to_probe();
    shown_captive_does_not_renavigate();
    shown_clock_wrong_exits_20_after_2s();
    retry_result_uses_startup_table();
    submit_like_reprobes_after_5s();
    destroyed_controller_ignores_late_callbacks();
    screen_text_and_buttons_match_spec();
    host_and_url_keys();

    if (fails == 0) std::printf("captive_controller_test: OK\n");
    return fails ? 1 : 0;
}
