// Log hygiene for the captive-portal sign-in mode (tests/captive_log_filter_test.cpp). Portal URLs routinely carry room
// numbers, surnames, voucher codes and session tokens, and the on-flash log must never hold form data or a full URL:
//   redactLogLine() filters ONE log line before it is written — it drops form-entry lines whole and cuts every
//                   scheme://... URL down to scheme://host[:port];
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
// What follows the authority (path, query, fragment) is cut up to the first of these; here ']' does end the URL
// ("[nav] [https://h/p?q] done" keeps its closing bracket).
inline bool logRedactEndsTail(char c) {
    return logRedactIsWs(c) || c == '"' || c == '\'' || c == ')' || c == ']' || c == '>';
}

// Every "scheme://authority tail" becomes "scheme://host[:port]": userinfo (up to the LAST '@' of the authority) and the
// tail are removed. Text outside URLs is copied unchanged. Only URLs with a scheme and "://" are recognised.
inline std::string logRedactUrls(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;                                   // s[0, i) has been emitted already
    while (i < s.size()) {
        const std::size_t sep = s.find("://", i);
        if (sep == std::string::npos) break;
        std::size_t b = sep;                             // the run of scheme characters right before "://"
        while (b > i && logRedactIsSchemeChar(s[b - 1])) --b;
        bool hasLetter = false;                          // a scheme starts with a letter; leading digits/+/-/. are plain text
        for (std::size_t k = b; k < sep; ++k) hasLetter = hasLetter || logRedactIsAlpha(s[k]);
        out.append(s, i, sep + 3 - i);                   // text up to and including "scheme://" (or a bare "://")
        i = sep + 3;
        if (!hasLetter) continue;                        // "://" without a scheme: not a URL

        std::size_t e = i;
        while (e < s.size() && !logRedactEndsAuthority(s[e])) ++e;
        std::size_t hostBegin = i;                       // userinfo = everything up to the LAST '@' of the authority
        for (std::size_t k = i; k < e; ++k) {
            if (s[k] == '@') hostBegin = k + 1;
        }
        out.append(s, hostBegin, e - hostBegin);         // host[:port]
        while (e < s.size() && !logRedactEndsTail(s[e])) ++e;   // path / query / fragment: gone
        i = e;
    }
    out.append(s, i, std::string::npos);
    return out;
}

}  // namespace detail

// Filters one log line. nullopt = drop the line (it mentions a form entry: "[form]" or "setFieldText" anywhere, in a
// multi-line message too); otherwise the line with every URL reduced to scheme://host[:port].
inline std::optional<std::string> redactLogLine(const std::string& line) {
    if (line.find("[form]") != std::string::npos || line.find("setFieldText") != std::string::npos) return std::nullopt;
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
