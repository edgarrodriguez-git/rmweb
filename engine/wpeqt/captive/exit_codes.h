// Exit codes of the captive-portal sign-in app and their human meanings, used only to make log lines readable
// (tests/captive_config_test.cpp). Standard library only. The launcher's shell side has its own ws_exit_meaning(),
// which must keep returning the identical strings.
//
//   20..24  the sign-in session's verdict, returned by the captive controller;
//   0..3, 63  the app's own codes (SIGTERM drain, touch-grab failure, start-up failure, GUI watchdog);
//   11..13  the launcher's codes (it never reaches the app: already running, display busy, systemd-run missing);
//   >= 128  the shell's 128+N convention for "killed by signal N".
#pragma once
#include <cstdio>

namespace rmweb::captive {

inline constexpr int kExitOnline = 20, kExitCancelled = 21, kExitIdle = 22, kExitNoNetwork = 23, kExitUseAsIs = 24;

// Never returns nullptr. The result is a string literal, except for signal codes: that text lives in a thread-local
// buffer which the NEXT exitMeaning(>=128) call on the same thread overwrites — copy it if it must outlive that.
inline const char* exitMeaning(int code) {
    switch (code) {
        case kExitOnline:    return "online";
        case kExitCancelled: return "cancelled";
        case kExitIdle:      return "idle timeout";
        case kExitNoNetwork: return "no network";
        case kExitUseAsIs:   return "use network as is";
        case 0:              return "SIGTERM drain";
        case 1:              return "touch grab failed";
        case 2:
        case 3:              return "start-up failure";
        case 63:             return "GUI watchdog";
        case 11:             return "already running";
        case 12:             return "display busy";
        case 13:             return "systemd-run missing";
        default:             break;
    }
    if (code >= 128) {
        static thread_local char buf[40];   // "killed by signal " + up to 10 digits fits with room to spare
        std::snprintf(buf, sizeof buf, "killed by signal %d", code - 128);
        return buf;
    }
    return "unknown";
}

}  // namespace rmweb::captive
