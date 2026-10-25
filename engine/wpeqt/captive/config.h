// Pure captive-portal configuration helpers — standard library only, so they are unit-tested off-device
// (tests/captive_config_test.cpp). Three small, independent pieces:
//
//  1. parseCaptiveConfig(): the OPTIONAL override file that tests use to point the probes at a fake portal
//     (tools/fake-portal). Real devices have no such file and get the defaults below. Format, one `key = value` per line:
//       - a `#` anywhere on a line starts a comment (it also cuts a value, so a URL fragment cannot be written here);
//       - the line is split on the FIRST `=`, so a value may itself contain `=` (query strings);
//       - whitespace around the key and around the value is trimmed (spaces, tabs, CR — CRLF files work);
//       - a line without `=`, an unknown key, or a known key with an empty value is ignored and does NOT set
//         `overridden`;
//       - keys: probe_google probe_apple confirm idle_timeout_s ca_file. The last assignment of a key wins;
//       - `overridden` means "a known key carried a value", which is what lights the "Test probe" badge. An
//         idle_timeout_s that is not a positive base-10 int (non-digit, sign, empty, zero, overflow) keeps the
//         current value (600 by default) but still sets `overridden`: the file was clearly meant as an override.
//     Integers are parsed by hand, never with std::stoi, so malformed input cannot throw.
//  2. captiveModeFromEnv(): RMWEB_KIOSK=captive selects the sign-in mode. Exact match only — no case folding, no trim.
//  3. stripCaptiveFlag(): removes the `--captive` marker from argv so the rest of the app's argument handling (and
//     whatever it hands argc/argv to) never sees it.
#pragma once
#include <cstring>
#include <limits>
#include <string>

namespace rmweb::captive {

struct CaptiveConfig {
    std::string gUrl = "http://connectivitycheck.gstatic.com/generate_204";
    std::string aUrl = "http://captive.apple.com/hotspot-detect.html";
    std::string confirmUrl = "https://www.google.com/generate_204";
    int idleTimeoutS = 600;
    std::string caFile;            // test-only trust anchor
    bool overridden = false;       // any key present => "Test probe" badge
};

namespace detail {

inline std::string trimWs(const std::string& s) {
    static const char* const kWs = " \t\r\n\f\v";
    const std::string::size_type b = s.find_first_not_of(kWs);
    if (b == std::string::npos) return std::string();
    const std::string::size_type e = s.find_last_not_of(kWs);
    return s.substr(b, e - b + 1);
}

// Positive base-10 int, digits only (no sign, no whitespace, no hex, no fraction). False on empty, a non-digit, zero,
// or anything above INT_MAX. The bound is checked per digit, so even a 30-digit value cannot overflow `v`.
inline bool parsePositiveInt(const std::string& s, int& out) {
    if (s.empty()) return false;
    long long v = 0;
    for (const char ch : s) {
        if (ch < '0' || ch > '9') return false;
        v = v * 10 + (ch - '0');
        if (v > std::numeric_limits<int>::max()) return false;
    }
    if (v <= 0) return false;
    out = static_cast<int>(v);
    return true;
}

}  // namespace detail

inline CaptiveConfig parseCaptiveConfig(const std::string& text) {
    CaptiveConfig cfg;
    std::string::size_type pos = 0;
    while (pos <= text.size()) {
        std::string::size_type nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;

        const std::string::size_type hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);

        const std::string::size_type eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = detail::trimWs(line.substr(0, eq));
        const std::string val = detail::trimWs(line.substr(eq + 1));
        if (val.empty()) continue;   // nothing to override with; `key =` must not look like a test run

        if (key == "probe_google") {
            cfg.gUrl = val;
        } else if (key == "probe_apple") {
            cfg.aUrl = val;
        } else if (key == "confirm") {
            cfg.confirmUrl = val;
        } else if (key == "ca_file") {
            cfg.caFile = val;
        } else if (key == "idle_timeout_s") {
            int n = 0;
            if (detail::parsePositiveInt(val, n)) cfg.idleTimeoutS = n;   // invalid: keep the current value
        } else {
            continue;                // unknown key: ignored, not an override
        }
        cfg.overridden = true;
    }
    return cfg;
}

// True only for exactly "captive"; false for nullptr, "", "Captive", "captive " ...
inline bool captiveModeFromEnv(const char* rmwebKiosk) {
    return rmwebKiosk != nullptr && std::strcmp(rmwebKiosk, "captive") == 0;
}

// Removes every argument that is exactly "--captive", compacting argv in place (order of the rest is kept) and
// returning the new argc. argv[0] is the program name and is never removed. When anything was removed argv[newArgc] is
// set to nullptr; otherwise nothing is written (argv[argc] is already nullptr by the C/C++ main() contract).
inline int stripCaptiveFlag(int argc, char** argv) {
    if (argv == nullptr || argc < 1) return argc < 0 ? 0 : argc;
    int out = 1;
    for (int i = 1; i < argc; ++i) {
        if (argv[i] != nullptr && std::strcmp(argv[i], "--captive") == 0) continue;
        argv[out++] = argv[i];
    }
    if (out < argc) argv[out] = nullptr;
    return out;
}

}  // namespace rmweb::captive
