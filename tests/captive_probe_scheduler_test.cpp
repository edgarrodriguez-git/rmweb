// Host unit test for the pure captive-portal re-probe scheduler (no Qt, no timers, no device). Build+run on the dev host:
//   clang++ -std=c++17 -o build/captive_probe_scheduler_test tests/captive_probe_scheduler_test.cpp && ./build/captive_probe_scheduler_test
// Time is always passed in explicitly (ms, int64_t), so every case below replays an exact timeline.
#include "../engine/wpeqt/captive/probe_scheduler.h"
#include <cstdint>
#include <cstdio>
using namespace rmweb::captive;

static int fails = 0;
#define CHECK(c) do { if(!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while(0)

// ---- the brief's eight cases ----

// 1. arm(0): the first idle cycle is due at 10000 — not a millisecond earlier.
static void idleCycleFiresAfterTenSeconds() {
    ProbeScheduler s;
    s.arm(0);
    CHECK(s.dueAtMs() == 10000);
    CHECK(!s.startIfDue(9999));
    CHECK(s.startIfDue(10000));
}

// 2. A page event inside the 3 s gap is deferred, never dropped.
static void pageEventInsideGapIsDeferredNotDropped() {
    ProbeScheduler s;
    s.arm(0);
    s.trigger(Trigger::PageEvent, 0);
    CHECK(s.startIfDue(0));                      // request at 0 is not gap-blocked before any cycle has run
    s.cycleFinished(500);
    s.trigger(Trigger::PageEvent, 1000);         // 1 s after the start: inside the gap
    CHECK(s.dueAtMs() == 3000);                  // lastStart(0) + kMinGapMs
    CHECK(!s.startIfDue(2999));
    CHECK(s.startIfDue(3000));
}

// 3. A page event during flight waits for cycleFinished, then starts at max(finish, lastStart + 3000).
static void pageEventDuringFlightWaitsForFinish() {
    {   // finish before the gap is over: the gap decides
        ProbeScheduler s;
        s.arm(0);
        s.trigger(Trigger::PageEvent, 0);
        CHECK(s.startIfDue(0));
        s.trigger(Trigger::PageEvent, 1000);     // while the cycle is in flight
        CHECK(!s.startIfDue(1000));
        CHECK(!s.startIfDue(3000));              // gap is over, but still in flight
        CHECK(s.dueAtMs() == INT64_MAX);         // nothing can start while in flight
        s.cycleFinished(2000);
        CHECK(s.dueAtMs() == 3000);              // max(2000, 0 + 3000)
        CHECK(!s.startIfDue(2999));
        CHECK(s.startIfDue(3000));
    }
    {   // finish after the gap is over: the finish decides
        ProbeScheduler s;
        s.arm(0);
        s.trigger(Trigger::PageEvent, 0);
        CHECK(s.startIfDue(0));
        s.trigger(Trigger::PageEvent, 1000);
        s.cycleFinished(4000);
        CHECK(s.dueAtMs() == 4000);              // max(4000, 0 + 3000)
        CHECK(s.startIfDue(4000));
    }
}

// 4. A submit-like action asks for a cycle 5 s later.
static void submitLikeWaitsFiveSeconds() {
    ProbeScheduler s;
    s.arm(0);
    s.trigger(Trigger::SubmitLike, 1000);
    CHECK(s.dueAtMs() == 6000);
    CHECK(!s.startIfDue(5999));
    CHECK(s.startIfDue(6000));
}

// 5. A probe-host navigation ignores the 3 s gap.
static void probeHostNavIgnoresMinimumGap() {
    ProbeScheduler s;
    s.arm(0);
    s.trigger(Trigger::PageEvent, 0);
    CHECK(s.startIfDue(0));
    s.cycleFinished(500);
    s.trigger(Trigger::ProbeHostNav, 1000);      // 1 s after the start: a page event would wait until 3000
    CHECK(s.dueAtMs() == 1000);
    CHECK(s.startIfDue(1000));
}

// 6. A page event at t restarts the idle clock: the next idle cycle is due at t + 10000, not at lastFinish + 10000.
static void pageEventResetsIdle() {
    ProbeScheduler s;
    s.arm(0);
    CHECK(s.startIfDue(10000));                  // first idle cycle
    s.cycleFinished(10500);
    CHECK(s.dueAtMs() == 20500);                 // idle clock runs from the finish
    s.trigger(Trigger::PageEvent, 15000);        // gap (13000) is over, so the event is served at once
    CHECK(s.startIfDue(15000));
    s.cycleFinished(15000);
    CHECK(s.dueAtMs() == 25000);                 // 15000 + 10000, not 10500 + 10000
    CHECK(!s.startIfDue(24999));
    CHECK(s.startIfDue(25000));
}

// 7. Five page events inside one 3 s window start exactly one cycle (500 ms tick, like the controller).
static void fivePageEventsStartOneCycle() {
    ProbeScheduler s;
    s.arm(0);
    CHECK(s.startIfDue(10000));                  // idle cycle sets lastStart = 10000
    s.cycleFinished(10200);
    for (int64_t t = 10300; t <= 10700; t += 100) s.trigger(Trigger::PageEvent, t);   // five events
    int starts = 0;
    int64_t startedAt = -1;
    for (int64_t t = 10500; t < 23000; t += 500) {   // idle cycle after the burst would be due at 23200
        if (s.startIfDue(t)) { ++starts; startedAt = t; s.cycleFinished(t + 200); }
    }
    CHECK(starts == 1);
    CHECK(startedAt == 13000);                   // lastStart + kMinGapMs
}

// 8. disarm: nothing is due.
static void disarmMakesNothingDue() {
    ProbeScheduler s;
    s.arm(0);
    s.disarm();
    CHECK(s.dueAtMs() == INT64_MAX);
    CHECK(!s.startIfDue(0));
    CHECK(!s.startIfDue(1000000));
}

// ---- extra cases pinning the rulings the brief leaves open ----

// Before arm(), and after disarm(), trigger() is ignored and nothing is due.
static void triggersAreIgnoredWhileDisarmed() {
    ProbeScheduler s;                            // never armed
    CHECK(s.dueAtMs() == INT64_MAX);
    s.trigger(Trigger::PageEvent, 0);
    s.trigger(Trigger::SubmitLike, 0);
    s.trigger(Trigger::ProbeHostNav, 0);
    CHECK(s.dueAtMs() == INT64_MAX);
    CHECK(!s.startIfDue(0));
    CHECK(!s.startIfDue(INT64_MAX));             // even a "never" time stamp must not start a cycle

    s.arm(1000);
    s.disarm();
    s.trigger(Trigger::PageEvent, 2000);
    s.trigger(Trigger::SubmitLike, 2000);
    s.trigger(Trigger::ProbeHostNav, 2000);
    CHECK(s.dueAtMs() == INT64_MAX);
    CHECK(!s.startIfDue(100000));
}

// disarm() clears every pending request; a later arm() starts clean.
static void disarmClearsPendingRequests() {
    ProbeScheduler s;
    s.arm(0);
    s.trigger(Trigger::PageEvent, 1000);
    s.trigger(Trigger::SubmitLike, 1000);
    s.trigger(Trigger::ProbeHostNav, 1000);
    s.disarm();
    CHECK(s.dueAtMs() == INT64_MAX);
    s.arm(20000);                                // none of the old requests may come back
    CHECK(s.dueAtMs() == 30000);
    CHECK(!s.startIfDue(29999));
    CHECK(s.startIfDue(30000));
}

// arm(now) sets the idle base to now, and re-arming clears pending requests.
static void armSetsIdleBaseAndClearsPending() {
    ProbeScheduler s;
    s.arm(5000);
    CHECK(s.dueAtMs() == 15000);
    s.trigger(Trigger::SubmitLike, 5000);        // due 10000
    CHECK(s.dueAtMs() == 10000);
    s.arm(6000);                                 // re-arm: the submit request is gone, idle restarts at 6000
    CHECK(s.dueAtMs() == 16000);
    s.trigger(Trigger::ProbeHostNav, 7000);
    s.arm(8000);
    CHECK(s.dueAtMs() == 18000);
}

// A cycle start satisfies every request due at or before it; a later request (a submit 5 s out) stays pending.
static void startSatisfiesDueRequestsOnly() {
    {   // a page event starts a cycle; the earlier submit request (due 5000) stays pending
        ProbeScheduler s;
        s.arm(0);
        s.trigger(Trigger::SubmitLike, 0);       // due 5000
        s.trigger(Trigger::PageEvent, 1000);     // due 1000
        CHECK(s.dueAtMs() == 1000);
        CHECK(s.startIfDue(1000));
        s.cycleFinished(1500);
        CHECK(s.dueAtMs() == 5000);              // the submit request survived
        CHECK(!s.startIfDue(4999));
        CHECK(s.startIfDue(5000));
        s.cycleFinished(5500);
        CHECK(s.dueAtMs() == 15500);             // and was consumed: only the idle request is left
    }
    {   // one late start covers both requests (5000 and 6000 are both <= 6000): exactly one cycle
        ProbeScheduler s;
        s.arm(0);
        s.trigger(Trigger::SubmitLike, 0);       // due 5000
        s.trigger(Trigger::PageEvent, 6000);     // due 6000
        CHECK(s.startIfDue(6000));
        s.cycleFinished(6500);
        CHECK(s.dueAtMs() == 16500);
    }
    {   // two submits 1 s apart are two requests: the cycle at 5000 does not swallow the one due at 6000
        ProbeScheduler s;
        s.arm(0);
        s.trigger(Trigger::SubmitLike, 0);       // due 5000
        s.trigger(Trigger::SubmitLike, 1000);    // due 6000
        CHECK(s.startIfDue(5000));
        s.cycleFinished(5200);
        CHECK(s.dueAtMs() == 8000);              // 6000 pushed out by the gap: max(6000, 5000 + 3000)
        CHECK(s.startIfDue(8000));
    }
}

// A probe-host navigation never starts a cycle while one is in flight, and then starts the moment it ends.
static void probeHostNavWaitsForFlightButNotForGap() {
    ProbeScheduler s;
    s.arm(0);
    s.trigger(Trigger::PageEvent, 0);
    CHECK(s.startIfDue(0));
    s.trigger(Trigger::ProbeHostNav, 1000);
    CHECK(s.dueAtMs() == INT64_MAX);
    CHECK(!s.startIfDue(1000));
    s.cycleFinished(1500);
    CHECK(s.dueAtMs() == 1500);                  // gap (3000) ignored; only the finish holds it back
    CHECK(s.startIfDue(1500));
}

// A bypass start also serves a gap-blocked normal request that is already due (time <= start).
static void probeHostNavStartAlsoSatisfiesGapBlockedPageEvent() {
    ProbeScheduler s;
    s.arm(0);
    s.trigger(Trigger::PageEvent, 0);
    CHECK(s.startIfDue(0));
    s.cycleFinished(100);
    s.trigger(Trigger::PageEvent, 1000);         // gap-blocked until 3000
    s.trigger(Trigger::ProbeHostNav, 1500);
    CHECK(s.dueAtMs() == 1500);
    CHECK(s.startIfDue(1500));
    s.cycleFinished(1600);
    CHECK(s.dueAtMs() == 11600);                 // the page-event request was served by that start: idle only
}

// A submit-like action is not a page event: it does not push the idle clock back.
static void submitLikeDoesNotDelayIdle() {
    ProbeScheduler s;
    s.arm(0);
    s.trigger(Trigger::SubmitLike, 8000);        // due 13000
    CHECK(s.dueAtMs() == 10000);                 // idle still due at arm + 10000
    CHECK(s.startIfDue(10000));
    s.cycleFinished(10100);
    CHECK(s.dueAtMs() == 13000);                 // the submit request (13000) is still pending after the idle cycle
    CHECK(s.startIfDue(13000));
}

// Idle cycles repeat every 10 s from each finish.
static void idleRepeatsFromEachFinish() {
    ProbeScheduler s;
    s.arm(0);
    CHECK(s.startIfDue(10000));
    CHECK(s.dueAtMs() == INT64_MAX);             // in flight
    s.cycleFinished(10400);
    CHECK(s.dueAtMs() == 20400);
    CHECK(s.startIfDue(20400));
    s.cycleFinished(21000);
    CHECK(s.dueAtMs() == 31000);
}

// A stray cycleFinished (no cycle in flight) must not disturb the idle clock.
static void strayCycleFinishedIsIgnored() {
    ProbeScheduler s;
    s.arm(0);
    s.cycleFinished(5000);
    CHECK(s.dueAtMs() == 10000);
}

// disarm()/arm() around an in-flight cycle: the cycle is still running, so arm() does not start a second one on top
// of it; once the caller reports the finish (even while disarmed) the scheduler works again.
static void disarmWhileInFlight() {
    {
        ProbeScheduler s;
        s.arm(0);
        s.trigger(Trigger::PageEvent, 0);
        CHECK(s.startIfDue(0));
        s.disarm();
        s.cycleFinished(400);                    // late finish, reported while disarmed
        CHECK(s.dueAtMs() == INT64_MAX);
        s.arm(500);
        CHECK(s.dueAtMs() == 10500);             // not stuck "in flight"
    }
    {
        ProbeScheduler s;
        s.arm(0);
        s.trigger(Trigger::PageEvent, 0);
        CHECK(s.startIfDue(0));
        s.disarm();
        s.arm(500);                              // the cycle has not finished yet
        CHECK(s.dueAtMs() == INT64_MAX);
        CHECK(!s.startIfDue(100000));
        s.cycleFinished(600);
        CHECK(s.dueAtMs() == 10600);
    }
}

// The minimum gap between starts survives a disarm()/arm() round trip (flood protection).
static void minimumGapSurvivesRearm() {
    ProbeScheduler s;
    s.arm(0);
    s.trigger(Trigger::PageEvent, 0);
    CHECK(s.startIfDue(0));
    s.cycleFinished(100);
    s.disarm();
    s.arm(200);
    s.trigger(Trigger::PageEvent, 300);
    CHECK(s.dueAtMs() == 3000);                  // lastStart(0) + kMinGapMs
    CHECK(!s.startIfDue(2999));
    CHECK(s.startIfDue(3000));
}

// The published constants match the spec.
static void constantsMatchSpec() {
    CHECK(ProbeScheduler::kMinGapMs == 3000);
    CHECK(ProbeScheduler::kSubmitDelayMs == 5000);
    CHECK(ProbeScheduler::kIdleMs == 10000);
}

int main() {
    idleCycleFiresAfterTenSeconds();
    pageEventInsideGapIsDeferredNotDropped();
    pageEventDuringFlightWaitsForFinish();
    submitLikeWaitsFiveSeconds();
    probeHostNavIgnoresMinimumGap();
    pageEventResetsIdle();
    fivePageEventsStartOneCycle();
    disarmMakesNothingDue();

    triggersAreIgnoredWhileDisarmed();
    disarmClearsPendingRequests();
    armSetsIdleBaseAndClearsPending();
    startSatisfiesDueRequestsOnly();
    probeHostNavWaitsForFlightButNotForGap();
    probeHostNavStartAlsoSatisfiesGapBlockedPageEvent();
    submitLikeDoesNotDelayIdle();
    idleRepeatsFromEachFinish();
    strayCycleFinishedIsIgnored();
    disarmWhileInFlight();
    minimumGapSurvivesRearm();
    constantsMatchSpec();

    if (fails == 0) std::printf("captive_probe_scheduler_test: OK\n");
    return fails ? 1 : 0;
}
