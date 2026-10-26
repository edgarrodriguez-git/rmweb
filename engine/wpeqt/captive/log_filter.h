// Log hygiene for the captive-portal sign-in mode (tests/captive_log_filter_test.cpp). Portal URLs routinely carry room
// numbers, surnames, voucher codes and session tokens, and the on-flash log must never hold page content, form data or
// credentials:
//   redactLogLine() filters ONE log line before it is written. It drops, whole, every line that carries page content
//                   (see kDroppedLogTags below), cuts every scheme://... URL down to scheme://host[:port] and every opaque
//                   data:/javascript:/mailto:/... URL down to scheme: (see kLogRedactOpaqueSchemes);
//   RotatingLog     is the size-capped file the filtered lines go to (one rotation, path -> path.1).
// Standard library plus POSIX open/fstat/write/rename only — no Qt — so it is unit-tested off-device. The app's Qt
// message handler (Task 13) calls both: redactLogLine() first, RotatingLog::append() on whatever survives.
#pragma once
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <fcntl.h>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace rmweb::captive {

namespace detail {

// Own helper names (prefixed log...): sibling headers define isDigit/trimWs/... in this same namespace.
inline bool logRedactIsAlpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
inline char logRedactLower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
inline bool logRedactIsWs(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }
// RFC 3986 scheme = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ); whether it STARTS with a letter is checked by the caller.
inline bool logRedactIsSchemeChar(char c) {
    return logRedactIsAlpha(c) || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
}
// The authority ends at the path/query/fragment start or at where the surrounding text closes the URL. NOT at ']':
// that closes a bracketed IPv6 host ("[::1]:8080") and so belongs to the authority.
inline bool logRedactEndsAuthority(char c) {
    return c == '/' || c == '?' || c == '#' || logRedactIsWs(c) || c == '"' || c == '\'' || c == ')' || c == '>';
}
// The RFC 3986 authority end — the path/query/fragment start, or whitespace — bounds the search for the '@' that ends
// userinfo. It ignores the closers above: WebKit leaves ) and ' raw in userinfo ("https://user:pa)ss@h.x/"), so a closer
// before the '@' must not end the authority there and emit the userinfo's prefix as the host.
inline bool logRedactEndsRfcAuthority(char c) { return c == '/' || c == '?' || c == '#' || logRedactIsWs(c); }

// Can s[begin, at) — everything before the authority's last '@' (at), which ends at aend — be userinfo that WebKit
// logged? Not if it holds a character WebKit always percent-encodes there (the WHATWG userinfo set: " < > ` { } | \ ^
// [ ] ; = and every ':' after the first), and not if it holds a closer (" ' ) >) while the authority ends at whitespace or
// the end: the '@' may then sit in text glued on after the URL ("(https://h.x)joe@hotel.example"). A canonical http(s)
// URI always has a path, so neither case drops a real host. When this is false the caller logs no host at all: the
// part before the '@' may be a password, the part after it log text.
inline bool logRedactIsUserinfo(const std::string& s, std::size_t begin, std::size_t at, std::size_t aend) {
    const bool pathFollows = aend < s.size() && !logRedactIsWs(s[aend]);   // aend is at '/', '?' or '#'
    bool colon = false;
    for (std::size_t k = begin; k < at; ++k) {
        switch (s[k]) {
            case '"': case '<': case '>': case '`': case '{': case '}': case '|': case '\\': case '^': case '[': case ']':
            case ';': case '=':
                return false;
            case ':':
                if (colon) return false;
                colon = true;
                break;
            case '\'': case ')':
                if (!pathFollows) return false;
                break;
            default:
                break;
        }
    }
    return true;
}
// The whole URL tail (path, query, fragment) is removed up to the next space, tab, CR or LF, or the end of the string.
// It is NOT cut at ')' ']' '>' '"' or an apostrophe: WebKit keeps those raw in canonical URIs ("/wiki/Foo_(bar)?token=...",
// "/(S(abc123))/login.aspx?room=12", "/O'Brien/in?room=12"), and stopping at one would leave the rest of the URL in the log.
inline bool logRedactIsTailWs(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// The one character that is kept out of a removed tail so prose stays readable: a closer, if the URL sits inside its opener.
inline char logRedactOpenerFor(char closer) {
    switch (closer) {
        case ')': return '(';
        case ']': return '[';
        case '>': return '<';
        case '"': return '"';
        case '\'': return '\'';
        default:  return '\0';
    }
}

// The openers still unclosed on the current line, counted forward: ( [ < by nesting depth (a closer only counts while
// its opener is open), " and ' by odd count, all reset at a line feed. URLs are found left to right, so the count only
// ever moves forward and a whole line costs O(length) however many URLs it holds.
class LogRedactOpeners {
public:
    // Is the URL whose scheme starts at `at` wrapped by an unclosed `opener`? True when the opener is the character just
    // before the scheme ("(https://..."), or an earlier one on the same line is still open ("(see https://..."). Keeping
    // the closer costs nothing privacy-wise: it is one fixed punctuation character.
    bool isOpenAt(const std::string& s, std::size_t at, char opener) {
        if (at == 0) return false;
        if (s[at - 1] == opener) return true;
        if (at < m_pos) *this = LogRedactOpeners();     // a URL left of the last one: recount (callers never do this)
        for (; m_pos < at; ++m_pos) count(s[m_pos]);
        switch (opener) {
            case '(':  return m_paren > 0;
            case '[':  return m_bracket > 0;
            case '<':  return m_angle > 0;
            case '"':  return m_dquote;
            case '\'': return m_squote;
            default:   return false;
        }
    }

private:
    void count(char c) {
        switch (c) {
            case '\n': m_paren = m_bracket = m_angle = 0; m_dquote = m_squote = false; break;
            case '(':  ++m_paren; break;
            case ')':  if (m_paren > 0) --m_paren; break;
            case '[':  ++m_bracket; break;
            case ']':  if (m_bracket > 0) --m_bracket; break;
            case '<':  ++m_angle; break;
            case '>':  if (m_angle > 0) --m_angle; break;
            case '"':  m_dquote = !m_dquote; break;
            case '\'': m_squote = !m_squote; break;
            default:   break;
        }
    }

    std::size_t m_pos = 0;                               // s[0, m_pos) has been counted
    std::size_t m_paren = 0, m_bracket = 0, m_angle = 0;
    bool m_dquote = false, m_squote = false;
};

// Opaque URLs — a scheme, ':' and no "//" — of these schemes are cut down to "scheme:" (R29). WebKit leaves an opaque
// path unencoded, raw spaces included ("data:text/html,Mr Smith", "tel:+44 20 7946 0123", "mailto:Joe Smith <joe@...>"),
// so their tail runs to the end of the line, any text after the URL on that line included. blob: URLs are made by the
// browser and hold no spaces: that tail ends at whitespace like a scheme:// tail. Names in lower case.
// wsTail says how a tail that starts with a space or tab is read. WebKit keeps that whitespace in the opaque path as well
// ("data: text/html,...", "mailto: joe@..." serialise unchanged), but log prose has it too ("Error receiving data: %s"):
//   Prose      — never a URL: javascript: and vbscript: never reach WebKit's URI logs, and blob: URLs hold no spaces;
//   UrlIfComma — a URL when a ',' follows on the same line: a data URL needs one, GIO's message has none;
//   Url        — always a URL (the address schemes).
enum class LogRedactWsTail { Prose, UrlIfComma, Url };
struct LogRedactOpaqueScheme { const char* name; std::size_t len; bool toLineEnd; LogRedactWsTail wsTail; };
inline constexpr LogRedactOpaqueScheme kLogRedactOpaqueSchemes[] = {
    {"data", 4, true, LogRedactWsTail::UrlIfComma}, {"javascript", 10, true, LogRedactWsTail::Prose},
    {"vbscript", 8, true, LogRedactWsTail::Prose},  {"mailto", 6, true, LogRedactWsTail::Url},
    {"blob", 4, false, LogRedactWsTail::Prose},     {"tel", 3, true, LogRedactWsTail::Url},
    {"sms", 3, true, LogRedactWsTail::Url}};
inline constexpr std::size_t kLogRedactOpaqueMaxLen = 10;   // "javascript"

// The listed opaque scheme that ends at the ':' at s[colon], or nullptr. The whole run of scheme characters right before
// the ':' must be that scheme (ASCII case-insensitive), so it starts at a token boundary: the start of the string or a
// character that is not alphanumeric and not + - . ("metadata:", "x-data:", "1data:" are not data:). At most
// kLogRedactOpaqueMaxLen + 1 characters are looked at, which keeps the redaction linear in the line length.
inline const LogRedactOpaqueScheme* logRedactOpaqueSchemeAt(const std::string& s, std::size_t colon) {
    std::size_t b = colon;
    while (b > 0 && colon - b <= kLogRedactOpaqueMaxLen && logRedactIsSchemeChar(s[b - 1])) --b;
    for (const LogRedactOpaqueScheme& o : kLogRedactOpaqueSchemes) {
        if (colon - b != o.len) continue;
        std::size_t k = 0;
        while (k < o.len && logRedactLower(s[b + k]) == o.name[k]) ++k;
        if (k == o.len) return &o;
    }
    return nullptr;
}

// Removes the URL tail that starts at s[from]: everything up to the next space, tab, CR or LF (toLineEnd: up to the next
// CR or LF), or the end of the string — except a closer that matches the opener wrapping the URL whose scheme starts at
// `scheme`, which is appended to out. Returns where the text after the URL resumes.
inline std::size_t logRedactCutTail(const std::string& s, std::size_t from, std::size_t scheme, bool toLineEnd,
                                    LogRedactOpeners& openers, std::string& out) {
    std::size_t t = from;
    while (t < s.size() && !(toLineEnd ? (s[t] == '\r' || s[t] == '\n') : logRedactIsTailWs(s[t]))) ++t;
    if (t > from) {
        const char last = s[t - 1], opener = logRedactOpenerFor(last);
        if (opener != '\0' && openers.isOpenAt(s, scheme, opener)) out += last;
    }
    return t;
}

// Every "scheme://authority tail" becomes "scheme://host[:port]": userinfo (up to the LAST '@' before the authority's
// / ? # or whitespace) and the whole tail are removed; when the text before that '@' cannot be userinfo
// (logRedactIsUserinfo), the URL becomes "scheme://" with no host. Every opaque URL of kLogRedactOpaqueSchemes becomes
// "scheme:" (an empty tail — the ':' followed by the end, a CR or LF — is prose and stays, and so is a tail that starts
// with a space or tab unless the scheme's wsTail makes it a URL: "data: 5 bytes" stays). Text outside URLs is copied
// unchanged. Linear in the length of s.
inline std::string logRedactUrls(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    LogRedactOpeners openers;
    std::size_t i = 0;                                   // s[0, i) has been emitted already
    std::size_t c = 0;                                   // where to look for the next ':'
    std::size_t comma = 0, eol = 0;                      // the next ',' and the next CR or LF, as last looked up
    while ((c = s.find(':', c)) != std::string::npos) {
        if (s.compare(c, 3, "://") != 0) {               // an opaque URL, or a plain ':'
            const LogRedactOpaqueScheme* o = logRedactOpaqueSchemeAt(s, c);
            if (o == nullptr || c + 1 == s.size() || s[c + 1] == '\r' || s[c + 1] == '\n') { ++c; continue; }
            if (s[c + 1] == ' ' || s[c + 1] == '\t') {   // a tail that starts with whitespace: as the scheme says
                bool url = o->wsTail == LogRedactWsTail::Url;
                if (o->wsTail == LogRedactWsTail::UrlIfComma) {
                    // Each lookup is redone only once c has passed its result, so they only move forward and a line
                    // full of "data: " near misses still costs O(length).
                    if (comma <= c) comma = s.find(',', c + 1);
                    if (eol <= c) eol = s.find_first_of("\r\n", c + 1);
                    url = comma < eol;                   // npos when there is none
                }
                if (!url) { ++c; continue; }
            }
            out.append(s, i, c + 1 - i);                 // text up to and including "scheme:"
            i = c = logRedactCutTail(s, c + 1, c - o->len, o->toLineEnd, openers, out);
            continue;
        }
        const std::size_t sep = c;
        std::size_t scheme = sep;                        // the run of scheme characters right before "://" ...
        while (scheme > i && logRedactIsSchemeChar(s[scheme - 1])) --scheme;
        while (scheme < sep && !logRedactIsAlpha(s[scheme])) ++scheme;   // ... a scheme starts with a letter
        out.append(s, i, sep + 3 - i);                   // text up to and including "scheme://" (or a bare "://")
        i = c = sep + 3;
        if (scheme == sep) continue;                     // "://" without a scheme: not a URL

        std::size_t aend = i;                            // the RFC 3986 authority, up to / ? # or whitespace
        while (aend < s.size() && !logRedactEndsRfcAuthority(s[aend])) ++aend;
        std::size_t hostBegin = i;                       // userinfo = everything up to the LAST '@' of the authority
        for (std::size_t k = i; k < aend; ++k) {
            if (s[k] == '@') hostBegin = k + 1;
        }
        if (hostBegin > i && !logRedactIsUserinfo(s, i, hostBegin - 1, aend)) {   // not userinfo: no host, all tail
            i = c = logRedactCutTail(s, i, scheme, /*toLineEnd=*/false, openers, out);
            continue;
        }
        std::size_t e = hostBegin;                      // the host[:port] ends where the surrounding text closes the URL
        while (e < s.size() && !logRedactEndsAuthority(s[e])) ++e;
        out.append(s, hostBegin, e - hostBegin);         // host[:port]
        i = c = logRedactCutTail(s, e, scheme, /*toLineEnd=*/false, openers, out);   // the tail: gone, up to whitespace
    }
    out.append(s, i, std::string::npos);
    return out;
}

}  // namespace detail

// A line containing any of these is dropped whole: each carries page content (or a page-controlled string) that must
// never reach the flash. [form]/setFieldText: typed field values. [meta]: the page title ("Welcome, Mr Smith — room 412").
// [dl] and [library]: a server-supplied file name (the download, and its import into the xochitl library).
// [find]/[search]: in-page find and address-bar search terms. "[t] page JS done": the page-script answer, which echoes
// DOM state. Matching is a plain, case-sensitive substring test anywhere in the message, so a multi-line message is
// dropped as a whole when any of its lines carries one.
inline constexpr const char* kDroppedLogTags[] = {"[form]", "setFieldText", "[meta]", "[dl]", "[library]", "[find]", "[search]",
                                                  "[t] page JS done"};

// Filters one log line. nullopt = drop the line (it carries one of kDroppedLogTags); otherwise the line with every URL
// reduced to scheme://host[:port], or to scheme: for an opaque one.
inline std::optional<std::string> redactLogLine(const std::string& line) {
    for (const char* tag : kDroppedLogTags) {
        if (line.find(tag) != std::string::npos) return std::nullopt;
    }
    return detail::logRedactUrls(line);
}

namespace detail {

// write(2) until everything is out; false on error. EINTR is retried, a short write continues.
inline bool logRotateWriteAll(int fd, const char* p, std::size_t n) {
    while (n > 0) {
        const ssize_t w = ::write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (w == 0) return false;
        p += w;
        n -= static_cast<std::size_t>(w);
    }
    return true;
}

}  // namespace detail

// A log file capped at about maxBytes with ONE generation of history: when the next line would push a non-empty file past
// the cap, path is renamed to path + ".1" (replacing any older .1) and a fresh path is started. The file is created with
// mode 0600. A single line longer than maxBytes is still written (into a fresh file) — the cap is never a reason to lose
// a line. The parent directory must already exist (it is NOT created). The file is opened per append, so an externally
// removed or replaced log is simply recreated.
//
// NOT thread-safe: two concurrent append() calls can interleave their rotation steps. The caller serialises (the Qt
// message handler holds one mutex around redactLogLine + append).
class RotatingLog {
public:
    explicit RotatingLog(std::string path, std::size_t maxBytes = 262144) : m_path(std::move(path)), m_max(maxBytes) {}

    // Appends line + '\n'. False if the open, the write or the rename fails (the line is then not logged); never throws.
    bool append(const std::string& line) {
        try {
            const std::string data = line + '\n';
            const std::string rotated = m_path + ".1";
            int fd = openLog();
            if (fd < 0) return false;
            struct stat st;
            if (::fstat(fd, &st) != 0) { ::close(fd); return false; }
            const unsigned long long size = static_cast<unsigned long long>(st.st_size);
            const unsigned long long need = data.size();
            const unsigned long long cap = m_max;
            if (size > 0 && (size >= cap || need > cap - size)) {   // size + need > cap, without overflow
                ::close(fd);
                if (::rename(m_path.c_str(), rotated.c_str()) != 0) return false;
                fd = openLog();                                      // path is gone now: this creates a fresh file
                if (fd < 0) return false;
            }
            const bool ok = detail::logRotateWriteAll(fd, data.data(), data.size());
            const bool closed = ::close(fd) == 0 || errno == EINTR;
            return ok && closed;
        } catch (...) {
            return false;
        }
    }

private:
    int openLog() const { return ::open(m_path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, S_IRUSR | S_IWUSR); }

    std::string m_path;
    std::size_t m_max;
};

}  // namespace rmweb::captive
