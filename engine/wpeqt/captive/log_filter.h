// Log hygiene for the captive-portal sign-in mode (tests/captive_log_filter_test.cpp). Portal URLs routinely carry room
// numbers, surnames, voucher codes and session tokens, and the on-flash log must never hold page content, form data or
// credentials:
//   redactLogLine() filters ONE log line before it is written. It drops, whole, every line that carries page content
//                   (see kDroppedLogTags below) and cuts every scheme://... URL down to scheme://host[:port];
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

// Is the URL whose scheme starts at `at` wrapped by an unclosed `opener`? True when the opener is the character just
// before the scheme ("(https://..."), or an earlier one on the same line is still open ("(see https://..."): brackets by
// nesting depth, quotes by odd count. Keeping the closer costs nothing privacy-wise: it is one fixed punctuation character.
inline bool logRedactIsOpenAt(const std::string& s, std::size_t at, char opener, char closer) {
    if (at == 0) return false;
    if (s[at - 1] == opener) return true;
    const std::size_t nl = s.rfind('\n', at - 1);
    std::size_t depth = 0;
    for (std::size_t k = (nl == std::string::npos) ? 0 : nl + 1; k < at; ++k) {
        if (opener == closer) {
            if (s[k] == opener) depth = depth ? 0 : 1;
        } else if (s[k] == opener) {
            ++depth;
        } else if (s[k] == closer && depth > 0) {
            --depth;
        }
    }
    return depth > 0;
}

// Every "scheme://authority tail" becomes "scheme://host[:port]": userinfo (up to the LAST '@' of the authority) and the
// whole tail are removed. Text outside URLs is copied unchanged. Only URLs with a scheme and "://" are recognised.
inline std::string logRedactUrls(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;                                   // s[0, i) has been emitted already
    while (i < s.size()) {
        const std::size_t sep = s.find("://", i);
        if (sep == std::string::npos) break;
        std::size_t scheme = sep;                        // the run of scheme characters right before "://" ...
        while (scheme > i && logRedactIsSchemeChar(s[scheme - 1])) --scheme;
        while (scheme < sep && !logRedactIsAlpha(s[scheme])) ++scheme;   // ... a scheme starts with a letter
        out.append(s, i, sep + 3 - i);                   // text up to and including "scheme://" (or a bare "://")
        i = sep + 3;
        if (scheme == sep) continue;                     // "://" without a scheme: not a URL

        std::size_t e = i;
        while (e < s.size() && !logRedactEndsAuthority(s[e])) ++e;
        std::size_t hostBegin = i;                       // userinfo = everything up to the LAST '@' of the authority
        for (std::size_t k = i; k < e; ++k) {
            if (s[k] == '@') hostBegin = k + 1;
        }
        out.append(s, hostBegin, e - hostBegin);         // host[:port]

        std::size_t t = e;                               // the tail: gone, up to the next whitespace
        while (t < s.size() && !logRedactIsTailWs(s[t])) ++t;
        if (t > e) {                                     // ... except a closer that matches the opener wrapping the URL
            const char last = s[t - 1], opener = logRedactOpenerFor(last);
            if (opener != '\0' && logRedactIsOpenAt(s, scheme, opener, last)) out += last;
        }
        i = t;
    }
    out.append(s, i, std::string::npos);
    return out;
}

}  // namespace detail

// A line containing any of these is dropped whole: each carries page content (or a page-controlled string) that must
// never reach the flash. [form]/setFieldText: typed field values. [meta]: the page title ("Welcome, Mr Smith — room 412").
// [dl]: a server-supplied file name. [find]/[search]: in-page find and address-bar search terms. "[t] page JS done": the
// page-script answer, which echoes DOM state. Matching is a plain, case-sensitive substring test anywhere in the message,
// so a multi-line message is dropped as a whole when any of its lines carries one.
inline constexpr const char* kDroppedLogTags[] = {"[form]", "setFieldText", "[meta]", "[dl]", "[find]", "[search]", "[t] page JS done"};

// Filters one log line. nullopt = drop the line (it carries one of kDroppedLogTags); otherwise the line with every URL
// reduced to scheme://host[:port].
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
