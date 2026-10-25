// Pure captive-portal probe classification — no Qt, no libsoup, no device deps, so it is unit-tested off-device
// (tests/captive_probe_classify_test.cpp). The libsoup probe runner fills an HttpReply per probe; this header turns
// each reply into a ProbeVerdict and folds one G+A probe cycle (plus the TLS confirmation) into a CycleResult.
// Also here: RFC 3986 reference resolution (Location headers), /proc/net/route default-route detection and an
// IMF-fixdate parser (HTTP Date header) for the device-clock-skew check — all stdlib-only and timegm-free.
#pragma once
#include <cstddef>
#include <string>

namespace rmweb::captive {

inline constexpr const char* kAppleSuccessBody = "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>";

enum class ProbeKind { Google204, AppleSuccess };
enum class ProbeClass { ProbeOk, Limited, Captive, NoNetwork };

// What the probe runner saw. transportOk=false means no HTTP response at all (DNS/connect/timeout failure).
struct HttpReply { bool transportOk = false; int status = 0; std::string body; long long contentLength = -1; std::string location; };
// location: the reply's Location header resolved against probeUrl, "" if the reply had none.
struct ProbeVerdict { ProbeClass cls = ProbeClass::NoNetwork; std::string probeUrl; std::string location; };

namespace detail {

inline bool isAlpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
inline bool isDigit(char c) { return c >= '0' && c <= '9'; }
inline bool isBlank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f'; }   // whitespace within a line
inline bool isSpace(char c) { return isBlank(c) || c == '\n'; }

// A URI reference split into its RFC 3986 §3 components. The has* flags keep "absent" distinct from "empty"
// (a reference "?" has an empty query, "" has none) — the resolution algorithm needs that difference.
struct UrlRef {
    bool hasScheme = false, hasAuthority = false, hasQuery = false, hasFragment = false;
    std::string scheme, authority, path, query, fragment;
};

inline UrlRef splitUrl(const std::string& s) {
    UrlRef u;
    std::size_t i = 0;
    if (!s.empty() && isAlpha(s[0])) {                                   // scheme = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) ":"
        std::size_t j = 1;
        while (j < s.size() && (isAlpha(s[j]) || isDigit(s[j]) || s[j] == '+' || s[j] == '-' || s[j] == '.')) ++j;
        if (j < s.size() && s[j] == ':') { u.hasScheme = true; u.scheme = s.substr(0, j); i = j + 1; }
    }
    if (s.compare(i, 2, "//") == 0) {                                    // authority runs to the next / ? #
        const std::size_t e = s.find_first_of("/?#", i + 2);
        u.hasAuthority = true;
        u.authority = s.substr(i + 2, e == std::string::npos ? std::string::npos : e - (i + 2));
        i = (e == std::string::npos) ? s.size() : e;
    }
    const std::size_t pe = s.find_first_of("?#", i);                     // path runs to the next ? or #
    u.path = s.substr(i, pe == std::string::npos ? std::string::npos : pe - i);
    i = (pe == std::string::npos) ? s.size() : pe;
    if (i < s.size() && s[i] == '?') {                                   // query runs to the next #
        const std::size_t fe = s.find('#', i + 1);
        u.hasQuery = true;
        u.query = s.substr(i + 1, fe == std::string::npos ? std::string::npos : fe - (i + 1));
        i = (fe == std::string::npos) ? s.size() : fe;
    }
    if (i < s.size() && s[i] == '#') { u.hasFragment = true; u.fragment = s.substr(i + 1); }
    return u;
}

// RFC 3986 §5.2.4 remove_dot_segments.
inline std::string removeDotSegments(std::string in) {
    auto startsWith = [&in](const char* p) { return in.rfind(p, 0) == 0; };
    auto popSegment = [](std::string& out) {                             // drop the last segment and the "/" before it
        const std::size_t p = out.rfind('/');
        out.erase(p == std::string::npos ? 0 : p);
    };
    std::string out;
    while (!in.empty()) {
        if (startsWith("../"))      in.erase(0, 3);                      // A
        else if (startsWith("./"))  in.erase(0, 2);
        else if (startsWith("/./")) in.replace(0, 3, "/");               // B
        else if (in == "/.")        in = "/";
        else if (startsWith("/../")) { in.replace(0, 4, "/"); popSegment(out); }   // C
        else if (in == "/..")       { in = "/"; popSegment(out); }
        else if (in == "." || in == "..") in.clear();                    // D
        else {                                                           // E: move one segment, with its leading "/"
            std::size_t n = in.find('/', in[0] == '/' ? 1 : 0);
            if (n == std::string::npos) n = in.size();
            out.append(in, 0, n);
            in.erase(0, n);
        }
    }
    return out;
}

// RFC 3986 §5.2.3 merge.
inline std::string mergePaths(const UrlRef& base, const std::string& refPath) {
    if (base.hasAuthority && base.path.empty()) return "/" + refPath;
    const std::size_t slash = base.path.rfind('/');
    if (slash == std::string::npos) return refPath;
    return base.path.substr(0, slash + 1) + refPath;
}

}  // namespace detail

// RFC 3986 §5.2.2 reference resolution of `ref` against the absolute URL `base`, recomposed per §5.3.
// Covers scheme / authority / path / query / fragment; no URL normalisation (case, percent-encoding) is done.
inline std::string resolveUrl(const std::string& base, const std::string& ref) {
    const detail::UrlRef B = detail::splitUrl(base), R = detail::splitUrl(ref);
    detail::UrlRef T;
    if (R.hasScheme) {
        T = R;
        T.path = detail::removeDotSegments(R.path);
    } else {
        if (R.hasAuthority) {
            T.hasAuthority = true; T.authority = R.authority;
            T.path = detail::removeDotSegments(R.path);
            T.hasQuery = R.hasQuery; T.query = R.query;
        } else {
            T.hasAuthority = B.hasAuthority; T.authority = B.authority;
            if (R.path.empty()) {
                T.path = B.path;
                if (R.hasQuery) { T.hasQuery = true;       T.query = R.query; }
                else            { T.hasQuery = B.hasQuery; T.query = B.query; }
            } else {
                T.path = (R.path[0] == '/') ? detail::removeDotSegments(R.path)
                                            : detail::removeDotSegments(detail::mergePaths(B, R.path));
                T.hasQuery = R.hasQuery; T.query = R.query;
            }
        }
        T.hasScheme = B.hasScheme; T.scheme = B.scheme;
    }
    T.hasFragment = R.hasFragment; T.fragment = R.fragment;

    std::string out;                                                     // §5.3 component recomposition
    if (T.hasScheme)    out += T.scheme + ":";
    if (T.hasAuthority) out += "//" + T.authority;
    out += T.path;
    if (T.hasQuery)     out += "?" + T.query;
    if (T.hasFragment)  out += "#" + T.fragment;
    return out;
}

// Classify one probe reply.
//  - NoNetwork: no HTTP response at all.
//  - ProbeOk:   the G probe answered 204, or the A probe answered 200 with exactly kAppleSuccessBody
//               (trailing whitespace ignored). A body that merely looks like success is a portal faking it.
//  - Limited:   status 200 or 400..599 with an empty body or a declared length of 0..4 bytes — the network answers
//               but with an (almost) empty page, i.e. no portal to sign in to, just no internet.
//  - Captive:   anything else (typically a redirect, or a real page where a 204 / the Apple body was expected).
// `location` is the reply's Location header (if non-empty), resolved against probeUrl.
inline ProbeVerdict classifyProbe(ProbeKind kind, const std::string& probeUrl, const HttpReply& r) {
    ProbeVerdict v;
    v.probeUrl = probeUrl;
    if (!r.transportOk) { v.cls = ProbeClass::NoNetwork; return v; }
    if (!r.location.empty()) v.location = resolveUrl(probeUrl, r.location);

    std::string trimmed = r.body;
    while (!trimmed.empty() && detail::isSpace(trimmed.back())) trimmed.pop_back();

    const bool statusMayBeLimited = r.status == 200 || (r.status >= 400 && r.status <= 599);
    if (kind == ProbeKind::Google204 && r.status == 204) v.cls = ProbeClass::ProbeOk;
    else if (kind == ProbeKind::AppleSuccess && r.status == 200 && trimmed == kAppleSuccessBody) v.cls = ProbeClass::ProbeOk;
    else if (statusMayBeLimited && (r.body.empty() || (r.contentLength >= 0 && r.contentLength <= 4))) v.cls = ProbeClass::Limited;
    else v.cls = ProbeClass::Captive;
    return v;
}

// Same bit values as GTlsCertificateFlags, so the libsoup runner can pass them through unchanged.
enum TlsFlag : unsigned { kTlsUnknownCa = 1u << 0, kTlsBadIdentity = 1u << 1, kTlsNotActivated = 1u << 2, kTlsExpired = 1u << 3,
                          kTlsRevoked = 1u << 4, kTlsInsecure = 1u << 5, kTlsGenericError = 1u << 6 };

// Outcome of the HTTPS confirmation probe (G over TLS):
//  - Ok204:        the TLS fetch worked and answered 204.
//  - ValidityOnly: the TLS handshake failed ONLY on certificate validity dates (expired / not yet valid) — the
//                  signature of a device whose clock is wrong while the network is fine.
//  - OtherFailure: anything else (TLS interference, other certificate errors, unexpected status, no connection).
enum class TlsConfirm { Ok204, ValidityOnly, OtherFailure };

inline TlsConfirm tlsConfirmFrom(bool transportOk, unsigned tlsFlags, int status) {
    if (transportOk) return status == 204 ? TlsConfirm::Ok204 : TlsConfirm::OtherFailure;
    constexpr unsigned kValidityOnly = kTlsNotActivated | kTlsExpired;
    if (tlsFlags != 0 && (tlsFlags & ~kValidityOnly) == 0) return TlsConfirm::ValidityOnly;
    return TlsConfirm::OtherFailure;
}

enum class CycleResult { Online, OnlineClockWrong, Captive, Partial, Limited, NoNetwork };

inline const char* cycleResultName(CycleResult r) {
    switch (r) {
        case CycleResult::Online:           return "Online";
        case CycleResult::OnlineClockWrong: return "OnlineClockWrong";
        case CycleResult::Captive:          return "Captive";
        case CycleResult::Partial:          return "Partial";
        case CycleResult::Limited:          return "Limited";
        case CycleResult::NoNetwork:        return "NoNetwork";
    }
    return "NoNetwork";
}

// viewUrl / locationUrl are only filled for Captive: the probe URL to open in the sign-in view and the portal's
// own redirect target (may be empty).
struct CycleOutcome { CycleResult result = CycleResult::NoNetwork; std::string viewUrl; std::string locationUrl; };

// Fold one probe cycle, g = Google probe verdict and a = Apple probe verdict, in this order:
//  1. no default route                 -> NoNetwork
//  2. any probe Captive                -> Captive (a captive verdict is conclusive even if the other probe passed);
//                                         view = G's probe URL when G is captive, else A's, location from that probe
//  3. any probe ProbeOk                -> Online / OnlineClockWrong / Partial per the TLS confirmation
//  4. any probe Limited                -> Limited
//  5. otherwise                        -> NoNetwork
inline CycleOutcome combineCycle(bool hasDefaultRoute, const ProbeVerdict& g, const ProbeVerdict& a, TlsConfirm confirm) {
    CycleOutcome o;
    if (!hasDefaultRoute) return o;                                      // result defaults to NoNetwork
    if (g.cls == ProbeClass::Captive || a.cls == ProbeClass::Captive) {
        const ProbeVerdict& src = (g.cls == ProbeClass::Captive) ? g : a;
        o.result = CycleResult::Captive;
        o.viewUrl = src.probeUrl;
        o.locationUrl = src.location;
        return o;
    }
    if (g.cls == ProbeClass::ProbeOk || a.cls == ProbeClass::ProbeOk) {
        switch (confirm) {
            case TlsConfirm::Ok204:        o.result = CycleResult::Online; break;
            case TlsConfirm::ValidityOnly: o.result = CycleResult::OnlineClockWrong; break;
            case TlsConfirm::OtherFailure: o.result = CycleResult::Partial; break;
        }
        return o;
    }
    if (g.cls == ProbeClass::Limited || a.cls == ProbeClass::Limited) o.result = CycleResult::Limited;
    return o;
}

struct CycleReport { CycleOutcome outcome; std::string gAddress, aAddress; bool dateKnown = false; long long skewSec = 0; };

// Does /proc/net/route text hold a default route (Destination 00000000) for `iface`? Columns are whitespace-
// separated: Iface Destination Gateway ... The header row never matches (its Destination column is a word).
inline bool hasDefaultRoute(const std::string& procNetRoute, const std::string& iface) {
    const std::string& s = procNetRoute;
    std::size_t lineStart = 0;
    while (lineStart < s.size()) {
        std::size_t lineEnd = s.find('\n', lineStart);
        if (lineEnd == std::string::npos) lineEnd = s.size();
        std::size_t p = lineStart;
        auto column = [&](std::string& out) {
            while (p < lineEnd && detail::isBlank(s[p])) ++p;
            if (p >= lineEnd) return false;
            const std::size_t b = p;
            while (p < lineEnd && !detail::isBlank(s[p])) ++p;
            out.assign(s, b, p - b);
            return true;
        };
        std::string name, dest;
        if (column(name) && column(dest) && name == iface && dest == "00000000") return true;
        lineStart = lineEnd + 1;
    }
    return false;
}

namespace detail {

// Days since 1970-01-01 of a proleptic Gregorian date (m = 1..12, d = 1..31) — Howard Hinnant's days_from_civil.
inline long long daysFromCivil(long long y, unsigned m, unsigned d) {
    if (m <= 2) --y;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);                           // [0, 399]
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;                // [0, 365]
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;                          // [0, 146096]
    return era * 146097 + static_cast<long long>(doe) - 719468;
}

// n ASCII digits at s[pos..pos+n) as a number, -1 if any is not a digit.
inline int digitsAt(const std::string& s, std::size_t pos, std::size_t n) {
    int v = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (!isDigit(s[pos + i])) return -1;
        v = v * 10 + (s[pos + i] - '0');
    }
    return v;
}

}  // namespace detail

// Parse an HTTP Date header in IMF-fixdate form ("Sun, 25 Oct 2026 14:02:12 GMT") to Unix epoch seconds.
// Returns -1 for anything else: the obsolete RFC 850 / asctime forms, a zone other than GMT, out-of-range or
// non-existent dates (Feb 30, hour 24, second 60), years before 1970, wrong length, trailing junk. The weekday
// name must be a real one but is not cross-checked against the date. UTC is computed from days-from-civil —
// never timegm/mktime, so it is locale-, timezone- and platform-independent.
inline long long parseHttpDate(const std::string& imfFixdate) {
    const std::string& s = imfFixdate;
    if (s.size() != 29) return -1;                                       // "Sun, 25 Oct 2026 14:02:12 GMT"
    if (s[3] != ',' || s[4] != ' ' || s[7] != ' ' || s[11] != ' ' || s[16] != ' ' ||
        s[19] != ':' || s[22] != ':' || s.compare(25, 4, " GMT") != 0) return -1;

    static const char* const kDays[7]    = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    static const char* const kMonths[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    bool dayOk = false;
    for (const char* d : kDays) if (s.compare(0, 3, d) == 0) dayOk = true;
    if (!dayOk) return -1;
    unsigned month = 0;                                                  // 1..12, 0 = unknown
    for (unsigned i = 0; i < 12; ++i) if (s.compare(8, 3, kMonths[i]) == 0) month = i + 1;
    if (month == 0) return -1;

    const int day = detail::digitsAt(s, 5, 2), year = detail::digitsAt(s, 12, 4);
    const int hour = detail::digitsAt(s, 17, 2), minute = detail::digitsAt(s, 20, 2), second = detail::digitsAt(s, 23, 2);
    if (day < 1 || year < 1970 || hour < 0 || minute < 0 || second < 0) return -1;    // also catches non-digits (-1)
    if (hour > 23 || minute > 59 || second > 59) return -1;

    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    static const int kMonthDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const int monthDays = (month == 2 && leap) ? 29 : kMonthDays[month - 1];
    if (day > monthDays) return -1;

    return detail::daysFromCivil(year, month, static_cast<unsigned>(day)) * 86400LL + hour * 3600LL + minute * 60LL + second;
}

// Is the device clock off from the server's by more than 5 minutes (either direction)?
inline bool clockSkewed(long long serverEpoch, long long deviceEpoch) {
    const long long diff = serverEpoch - deviceEpoch;
    return diff > 300 || diff < -300;
}

}  // namespace rmweb::captive
