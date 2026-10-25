// Pure captive-portal re-probe scheduler — no Qt, no timers, no device deps, so it is unit-tested off-device
// (tests/captive_probe_scheduler_test.cpp). While the network's sign-in page is shown, the controller calls
// startIfDue() from a 500 ms tick; this class only decides WHEN the next probe cycle may start, so the app
// re-checks connectivity at the right moments without flooding the network. All times are caller-supplied
// milliseconds on one monotonic clock (int64_t), which makes every timeline replayable in a test.
//
// Rules (spec §5, "Re-probe triggers"):
//  - A PageEvent asks for a cycle at `now`; a SubmitLike asks for one at `now + 5000`; a ProbeHostNav asks at `now`
//    and ignores the minimum gap between cycle starts.
//  - While armed, an idle request is always standing: due at max(armed, lastFinish, lastPageEvent) + 10000.
//    A SubmitLike or ProbeHostNav does not count as a page event for that idle base; a PageEvent does.
//  - A request is due at max(requestAt, lastStart + 3000) (no gap for ProbeHostNav) and never while a cycle is in
//    flight. Requests are deferred, never dropped; one cycle start satisfies every request due at or before it,
//    later requests stay pending.
#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace rmweb::captive {

enum class Trigger { PageEvent, SubmitLike, ProbeHostNav };

class ProbeScheduler {
public:
    static constexpr int64_t kMinGapMs = 3000, kSubmitDelayMs = 5000, kIdleMs = 10000;

    // Portal shown: start accepting triggers and start the idle timer. Clears pending requests. Does NOT touch
    // lastStart or the in-flight flag, so the minimum gap holds across disarm()/arm() and a running cycle is not
    // overlapped by a new one.
    void arm(int64_t nowMs) {
        armed_ = true;
        armedAt_ = nowMs;
        pending_.clear();
        bypassAt_ = kNever;
    }

    // Start-up screens / exiting: nothing is due, every pending request is dropped, trigger() is ignored.
    void disarm() {
        armed_ = false;
        pending_.clear();
        bypassAt_ = kNever;
    }

    void trigger(Trigger t, int64_t nowMs) {
        if (!armed_) return;
        switch (t) {
            case Trigger::PageEvent:    pending_.push_back(nowMs);
                                        lastPageEvent_ = std::max(lastPageEvent_, nowMs); break;
            case Trigger::SubmitLike:   pending_.push_back(nowMs + kSubmitDelayMs); break;
            case Trigger::ProbeHostNav: bypassAt_ = std::min(bypassAt_, nowMs); break;
        }
    }

    // Earliest allowed start of the next cycle; INT64_MAX if disarmed or a cycle is in flight. A value in the past
    // means "due now".
    int64_t dueAtMs() const {
        if (!armed_ || inFlight_) return kNever;
        const int64_t idleAt = std::max({armedAt_, lastFinish_, lastPageEvent_}) + kIdleMs;
        int64_t normalAt = idleAt;
        if (!pending_.empty()) normalAt = std::min(normalAt, *std::min_element(pending_.begin(), pending_.end()));
        int64_t due = std::max(normalAt, lastStart_ + kMinGapMs);   // normal requests wait out the gap ...
        due = std::min(due, bypassAt_);                             // ... a ProbeHostNav does not
        return std::max(due, lastFinish_);                          // and nothing starts before the last cycle ended
    }

    // True => the caller starts a cycle now; the scheduler is then in flight until cycleFinished().
    bool startIfDue(int64_t nowMs) {
        if (!armed_ || inFlight_ || nowMs < dueAtMs()) return false;
        inFlight_ = true;
        lastStart_ = nowMs;
        pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [nowMs](int64_t at) { return at <= nowMs; }),
                       pending_.end());
        if (bypassAt_ <= nowMs) bypassAt_ = kNever;
        return true;
    }

    // The started cycle is over (any outcome). A call with no cycle in flight is ignored.
    void cycleFinished(int64_t nowMs) {
        if (!inFlight_) return;
        inFlight_ = false;
        lastFinish_ = nowMs;
    }

private:
    static constexpr int64_t kNever = std::numeric_limits<int64_t>::max();
    // "No cycle yet": far enough back that a request at time 0 is not gap-blocked, yet lastStart_ + kMinGapMs
    // cannot overflow.
    static constexpr int64_t kLongAgo = std::numeric_limits<int64_t>::min() / 2;

    bool armed_ = false, inFlight_ = false;
    int64_t armedAt_ = 0;
    int64_t lastStart_ = kLongAgo, lastFinish_ = kLongAgo, lastPageEvent_ = kLongAgo;
    std::vector<int64_t> pending_;   // normal requests (PageEvent: now, SubmitLike: now + 5000); only due ones are cleared by a start
    int64_t bypassAt_ = kNever;      // earliest pending ProbeHostNav request; always <= the next start, so one slot is enough
};

}  // namespace rmweb::captive
