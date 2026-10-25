// Host unit test for the pure captive-portal config / flag / exit-code headers (no Qt, no device). Build+run on the dev host:
//   clang++ -std=c++17 -Wall -Wextra -o build/captive_config_test tests/captive_config_test.cpp && ./build/captive_config_test
// Covers engine/wpeqt/captive/config.h (override file, RMWEB_KIOSK flag, --captive argv strip) and
// engine/wpeqt/captive/exit_codes.h (exit-code constants and their log meanings).
#include "../engine/wpeqt/captive/config.h"
#include "../engine/wpeqt/captive/exit_codes.h"
#include <cstdio>
#include <cstring>
#include <string>
using namespace rmweb::captive;

static int fails = 0;
#define CHECK(c) do { if(!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while(0)

// ---- the brief's checks ----

// Empty text gives the defaults with overridden == false.
static void emptyTextGivesDefaults() {
    const CaptiveConfig c = parseCaptiveConfig("");
    CHECK(c.gUrl == "http://connectivitycheck.gstatic.com/generate_204");
    CHECK(c.aUrl == "http://captive.apple.com/hotspot-detect.html");
    CHECK(c.confirmUrl == "https://www.google.com/generate_204");
    CHECK(c.idleTimeoutS == 600);
    CHECK(c.caFile.empty());
    CHECK(!c.overridden);
}

// Each key overrides its field and sets overridden.
static void eachKeyOverridesItsField() {
    {
        const CaptiveConfig c = parseCaptiveConfig("probe_google = http://127.0.0.1:8080/g\n");
        CHECK(c.gUrl == "http://127.0.0.1:8080/g");
        CHECK(c.aUrl == "http://captive.apple.com/hotspot-detect.html");   // the rest stay default
        CHECK(c.confirmUrl == "https://www.google.com/generate_204");
        CHECK(c.overridden);
    }
    {
        const CaptiveConfig c = parseCaptiveConfig("probe_apple = http://127.0.0.1:8080/a\n");
        CHECK(c.aUrl == "http://127.0.0.1:8080/a");
        CHECK(c.gUrl == "http://connectivitycheck.gstatic.com/generate_204");
        CHECK(c.overridden);
    }
    {
        const CaptiveConfig c = parseCaptiveConfig("confirm = https://127.0.0.1:8443/c\n");
        CHECK(c.confirmUrl == "https://127.0.0.1:8443/c");
        CHECK(c.overridden);
    }
    {
        const CaptiveConfig c = parseCaptiveConfig("idle_timeout_s = 42\n");
        CHECK(c.idleTimeoutS == 42);
        CHECK(c.overridden);
    }
    {
        const CaptiveConfig c = parseCaptiveConfig("ca_file = /tmp/fake-portal-ca.pem\n");
        CHECK(c.caFile == "/tmp/fake-portal-ca.pem");
        CHECK(c.overridden);
    }
    {   // all five in one file, in any order
        const CaptiveConfig c = parseCaptiveConfig(
            "ca_file = /ca.pem\nidle_timeout_s = 7\nconfirm = https://x/c\nprobe_apple = http://x/a\nprobe_google = http://x/g\n");
        CHECK(c.gUrl == "http://x/g");
        CHECK(c.aUrl == "http://x/a");
        CHECK(c.confirmUrl == "https://x/c");
        CHECK(c.idleTimeoutS == 7);
        CHECK(c.caFile == "/ca.pem");
        CHECK(c.overridden);
    }
}

// "  confirm = https://x/c  # c" gives confirmUrl == "https://x/c" (comment and surrounding whitespace stripped).
static void commentAndWhitespaceAreStripped() {
    const CaptiveConfig c = parseCaptiveConfig("  confirm = https://x/c  # c");
    CHECK(c.confirmUrl == "https://x/c");
    CHECK(c.overridden);
}

// idle_timeout_s = 0 gives 600 (but the file is still an override).
static void zeroIdleTimeoutKeepsDefault() {
    const CaptiveConfig c = parseCaptiveConfig("idle_timeout_s = 0");
    CHECK(c.idleTimeoutS == 600);
    CHECK(c.overridden);
}

// captiveModeFromEnv: true only for exactly "captive".
static void captiveModeFromEnvAcceptsOnlyExactValue() {
    CHECK(captiveModeFromEnv("captive"));
    CHECK(!captiveModeFromEnv(nullptr));
    CHECK(!captiveModeFromEnv(""));
    CHECK(!captiveModeFromEnv("Captive"));
}

// stripCaptiveFlag on {"app","--captive","x"} returns 2 with argv[1] == "x".
static void stripCaptiveFlagBasic() {
    char a0[] = "app", a1[] = "--captive", a2[] = "x";
    char* argv[] = {a0, a1, a2, nullptr};
    const int n = stripCaptiveFlag(3, argv);
    CHECK(n == 2);
    CHECK(std::strcmp(argv[0], "app") == 0);
    CHECK(std::strcmp(argv[1], "x") == 0);
}

// exitMeaning spot checks from the brief.
static void exitMeaningBriefCases() {
    CHECK(std::string(exitMeaning(20)) == "online");
    CHECK(std::string(exitMeaning(134)) == "killed by signal 6");
    CHECK(std::string(exitMeaning(63)) == "GUI watchdog");
}

// ---- pins for the rulings the brief leaves open ----

// parse: whitespace trimming covers tabs and CR (CRLF files), on both key and value.
static void tabsAndCrlfAreTrimmed() {
    const CaptiveConfig c = parseCaptiveConfig("\tprobe_google\t=\thttp://x/g\t\r\nconfirm=https://x/c\r\n");
    CHECK(c.gUrl == "http://x/g");
    CHECK(c.confirmUrl == "https://x/c");
}

// parse: a '#' anywhere starts a comment; a whole-line comment sets nothing.
static void commentsAnywhereOnTheLine() {
    CHECK(!parseCaptiveConfig("# confirm = https://x/c\n").overridden);
    CHECK(!parseCaptiveConfig("   # confirm = https://x/c\n").overridden);
    const CaptiveConfig c = parseCaptiveConfig("confirm = https://x/c#frag\n");   // '#' cuts the value
    CHECK(c.confirmUrl == "https://x/c");
    const CaptiveConfig d = parseCaptiveConfig("conf#irm = https://x/c\n");       // '#' in the key cuts the line
    CHECK(!d.overridden);
}

// parse: split on the FIRST '=', so the value may contain more '=' (query strings).
static void splitsOnFirstEquals() {
    const CaptiveConfig c = parseCaptiveConfig("probe_google = http://x/g?a=b&c=d\n");
    CHECK(c.gUrl == "http://x/g?a=b&c=d");
    CHECK(c.overridden);
}

// parse: a line without '=' is ignored.
static void lineWithoutEqualsIsIgnored() {
    const CaptiveConfig c = parseCaptiveConfig("confirm\njust some words\n\n   \n");
    CHECK(!c.overridden);
    CHECK(c.confirmUrl == "https://www.google.com/generate_204");
}

// parse: an unknown key is ignored and does not set overridden; known keys around it still apply.
static void unknownKeyIsIgnored() {
    const CaptiveConfig c = parseCaptiveConfig("no_such_key = 1\n");
    CHECK(!c.overridden);
    CHECK(c.idleTimeoutS == 600);
    const CaptiveConfig d = parseCaptiveConfig("no_such_key = 1\nconfirm = https://x/c\nother = 2\n");
    CHECK(d.confirmUrl == "https://x/c");
    CHECK(d.overridden);
    CHECK(!parseCaptiveConfig("Confirm = https://x/c\n").overridden);   // keys are case-sensitive
}

// parse: a known key with an empty value is ignored — nothing changes, overridden stays false.
static void emptyValueIsIgnored() {
    for (const char* text : {"confirm =\n", "confirm = \n", "probe_google=   # nothing\n", "probe_apple =\n",
                             "ca_file =\n", "idle_timeout_s =\n"}) {
        const CaptiveConfig c = parseCaptiveConfig(text);
        CHECK(!c.overridden);
        CHECK(c.gUrl == "http://connectivitycheck.gstatic.com/generate_204");
        CHECK(c.aUrl == "http://captive.apple.com/hotspot-detect.html");
        CHECK(c.confirmUrl == "https://www.google.com/generate_204");
        CHECK(c.idleTimeoutS == 600);
        CHECK(c.caFile.empty());
    }
    // An empty line does not wipe a value set by an earlier line.
    const CaptiveConfig d = parseCaptiveConfig("confirm = https://x/c\nconfirm =\n");
    CHECK(d.confirmUrl == "https://x/c");
    CHECK(d.overridden);
}

// parse: idle_timeout_s accepts a positive base-10 integer; anything else keeps 600 but still sets overridden.
static void idleTimeoutParsing() {
    CHECK(parseCaptiveConfig("idle_timeout_s = 1").idleTimeoutS == 1);
    CHECK(parseCaptiveConfig("idle_timeout_s = 3600").idleTimeoutS == 3600);
    CHECK(parseCaptiveConfig("idle_timeout_s = 2147483647").idleTimeoutS == 2147483647);   // INT_MAX still fits
    for (const char* text : {"idle_timeout_s = 0", "idle_timeout_s = -5", "idle_timeout_s = +5", "idle_timeout_s = abc",
                             "idle_timeout_s = 12abc", "idle_timeout_s = 1.5", "idle_timeout_s = 1 2",
                             "idle_timeout_s = 0x10",
                             "idle_timeout_s = 2147483648",                       // INT_MAX + 1
                             "idle_timeout_s = 99999999999999999999999999"}) {    // far past any integer type
        const CaptiveConfig c = parseCaptiveConfig(text);
        CHECK(c.idleTimeoutS == 600);
        CHECK(c.overridden);
    }
    // Leading zeros are plain digits.
    CHECK(parseCaptiveConfig("idle_timeout_s = 007").idleTimeoutS == 7);
}

// parse: the last assignment of a key wins.
static void lastAssignmentWins() {
    const CaptiveConfig c = parseCaptiveConfig("confirm = https://x/1\nconfirm = https://x/2\n");
    CHECK(c.confirmUrl == "https://x/2");
}

// parse: a final line without a trailing newline is still read.
static void lastLineWithoutNewline() {
    const CaptiveConfig c = parseCaptiveConfig("probe_apple = http://x/a");
    CHECK(c.aUrl == "http://x/a");
}

// env: exactly "captive" — no case folding, no trimming.
static void captiveModeFromEnvIsStrict() {
    CHECK(!captiveModeFromEnv("captive "));
    CHECK(!captiveModeFromEnv(" captive"));
    CHECK(!captiveModeFromEnv("CAPTIVE"));
    CHECK(!captiveModeFromEnv("captive\n"));
    CHECK(!captiveModeFromEnv("captiv"));
    CHECK(!captiveModeFromEnv("captive1"));
    CHECK(!captiveModeFromEnv("kiosk"));
}

// argv: every exact "--captive" goes, the rest keep their order, argv[newArgc] == nullptr, argv[0] is never removed.
static void stripCaptiveFlagCompactsAndTerminates() {
    {   // the brief's case, plus the terminator
        char a0[] = "app", a1[] = "--captive", a2[] = "x";
        char* argv[] = {a0, a1, a2, nullptr};
        const int n = stripCaptiveFlag(3, argv);
        CHECK(n == 2);
        CHECK(argv[n] == nullptr);
    }
    {   // several copies, mixed with other args; order is preserved
        char a0[] = "app", a1[] = "--captive", a2[] = "a", a3[] = "--captive", a4[] = "b", a5[] = "--captive";
        char* argv[] = {a0, a1, a2, a3, a4, a5, nullptr};
        const int n = stripCaptiveFlag(6, argv);
        CHECK(n == 3);
        CHECK(std::strcmp(argv[0], "app") == 0);
        CHECK(std::strcmp(argv[1], "a") == 0);
        CHECK(std::strcmp(argv[2], "b") == 0);
        CHECK(argv[n] == nullptr);
    }
    {   // no flag: nothing moves
        char a0[] = "app", a1[] = "x", a2[] = "y";
        char* argv[] = {a0, a1, a2, nullptr};
        const int n = stripCaptiveFlag(3, argv);
        CHECK(n == 3);
        CHECK(argv[1] == a1 && argv[2] == a2);
        CHECK(argv[n] == nullptr);
    }
    {   // only exact matches are removed
        char a0[] = "app", a1[] = "--captive=1", a2[] = "--Captive", a3[] = "-captive", a4[] = "captive", a5[] = "--captive ";
        char* argv[] = {a0, a1, a2, a3, a4, a5, nullptr};
        const int n = stripCaptiveFlag(6, argv);
        CHECK(n == 6);
        CHECK(argv[5] == a5);
        CHECK(argv[n] == nullptr);
    }
    {   // argv[0] is the program name: never removed, even if it is literally "--captive"
        char a0[] = "--captive", a1[] = "--captive", a2[] = "x";
        char* argv[] = {a0, a1, a2, nullptr};
        const int n = stripCaptiveFlag(3, argv);
        CHECK(n == 2);
        CHECK(argv[0] == a0);
        CHECK(std::strcmp(argv[1], "x") == 0);
        CHECK(argv[n] == nullptr);
    }
    {   // the flag is the only argument
        char a0[] = "app", a1[] = "--captive";
        char* argv[] = {a0, a1, nullptr};
        const int n = stripCaptiveFlag(2, argv);
        CHECK(n == 1);
        CHECK(argv[0] == a0);
        CHECK(argv[n] == nullptr);
    }
    {   // degenerate argc: nothing to do, nothing to crash on
        char* argv[] = {nullptr};
        CHECK(stripCaptiveFlag(0, argv) == 0);
    }
}

// exit codes: the numeric values are part of the launcher contract.
static void exitCodeConstants() {
    static_assert(kExitOnline == 20 && kExitCancelled == 21 && kExitIdle == 22 && kExitNoNetwork == 23 && kExitUseAsIs == 24,
                  "captive exit codes are a contract with the launcher");
    CHECK(kExitOnline == 20);
    CHECK(kExitCancelled == 21);
    CHECK(kExitIdle == 22);
    CHECK(kExitNoNetwork == 23);
    CHECK(kExitUseAsIs == 24);
}

static void exitMeaningCaptiveCodes() {
    CHECK(std::string(exitMeaning(kExitOnline)) == "online");
    CHECK(std::string(exitMeaning(kExitCancelled)) == "cancelled");
    CHECK(std::string(exitMeaning(kExitIdle)) == "idle timeout");
    CHECK(std::string(exitMeaning(kExitNoNetwork)) == "no network");
    CHECK(std::string(exitMeaning(kExitUseAsIs)) == "use network as is");
}

static void exitMeaningAppCodes() {
    CHECK(std::string(exitMeaning(0)) == "SIGTERM drain");
    CHECK(std::string(exitMeaning(1)) == "touch grab failed");
    CHECK(std::string(exitMeaning(2)) == "start-up failure");
    CHECK(std::string(exitMeaning(3)) == "start-up failure");
    CHECK(std::string(exitMeaning(63)) == "GUI watchdog");
}

// The launcher's own codes (Task 9's shell ws_exit_meaning uses the identical strings).
static void exitMeaningLauncherCodes() {
    CHECK(std::string(exitMeaning(11)) == "already running");
    CHECK(std::string(exitMeaning(12)) == "display busy");
    CHECK(std::string(exitMeaning(13)) == "systemd-run missing");
}

static void exitMeaningSignals() {
    CHECK(std::string(exitMeaning(128)) == "killed by signal 0");
    CHECK(std::string(exitMeaning(129)) == "killed by signal 1");
    CHECK(std::string(exitMeaning(134)) == "killed by signal 6");
    CHECK(std::string(exitMeaning(137)) == "killed by signal 9");
    CHECK(std::string(exitMeaning(139)) == "killed by signal 11");
    CHECK(std::string(exitMeaning(143)) == "killed by signal 15");
    CHECK(std::string(exitMeaning(255)) == "killed by signal 127");
    CHECK(std::string(exitMeaning(1000)) == "killed by signal 872");   // >= 128 is the rule, whatever the magnitude
}

static void exitMeaningUnknown() {
    CHECK(std::string(exitMeaning(4)) == "unknown");
    CHECK(std::string(exitMeaning(10)) == "unknown");
    CHECK(std::string(exitMeaning(14)) == "unknown");
    CHECK(std::string(exitMeaning(19)) == "unknown");
    CHECK(std::string(exitMeaning(25)) == "unknown");
    CHECK(std::string(exitMeaning(62)) == "unknown");
    CHECK(std::string(exitMeaning(64)) == "unknown");
    CHECK(std::string(exitMeaning(127)) == "unknown");
    CHECK(std::string(exitMeaning(-1)) == "unknown");
    CHECK(std::string(exitMeaning(-128)) == "unknown");
}

// exitMeaning never returns null, and the signal text is a stable thread-local buffer (never freed by the caller).
static void exitMeaningPointerContract() {
    for (int code = -3; code < 300; ++code) {
        CHECK(exitMeaning(code) != nullptr);
    }
    CHECK(std::string(exitMeaning(128 + 9)) == "killed by signal 9");
    CHECK(std::string(exitMeaning(128 + 15)) == "killed by signal 15");   // the buffer is rewritten per call
}

int main() {
    // ---- the brief's checks ----
    emptyTextGivesDefaults();
    eachKeyOverridesItsField();
    commentAndWhitespaceAreStripped();
    zeroIdleTimeoutKeepsDefault();
    captiveModeFromEnvAcceptsOnlyExactValue();
    stripCaptiveFlagBasic();
    exitMeaningBriefCases();

    // ---- pins for the rulings the brief leaves open ----
    tabsAndCrlfAreTrimmed();
    commentsAnywhereOnTheLine();
    splitsOnFirstEquals();
    lineWithoutEqualsIsIgnored();
    unknownKeyIsIgnored();
    emptyValueIsIgnored();
    idleTimeoutParsing();
    lastAssignmentWins();
    lastLineWithoutNewline();
    captiveModeFromEnvIsStrict();
    stripCaptiveFlagCompactsAndTerminates();
    exitCodeConstants();
    exitMeaningCaptiveCodes();
    exitMeaningAppCodes();
    exitMeaningLauncherCodes();
    exitMeaningSignals();
    exitMeaningUnknown();
    exitMeaningPointerContract();

    if (fails == 0) std::printf("captive_config_test: OK\n");
    return fails ? 1 : 0;
}
