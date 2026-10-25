// Captive-portal probe runner on libsoup 3: the network side of connectivity detection. One cycle sends the two
// plain-HTTP probes (G = Google generate_204, A = Apple hotspot-detect) in parallel, then — only when its answer can
// change the outcome — the HTTPS confirmation, classifies the replies with the pure functions of probe_classify.h and
// hands one CycleReport to the caller. Header-only so main.cpp can include it; no Qt (main.cpp builds with
// QT_NO_KEYWORDS, and nothing here uses or defines the Qt keywords). Host-tested through tools/probe-cli against
// tools/fake-portal (tests/probe_runner_test.sh).
//
// Per cycle (design §5 "Probe"):
//  - a fresh SoupSession (timeout 5 s, idle-timeout 1 s; tls-database from caFile when one is set), so nothing —
//    connections, TLS sessions — survives from one cycle to the next;
//  - every message is a GET with SOUP_MESSAGE_NO_REDIRECT | SOUP_MESSAGE_NEW_CONNECTION and `Connection: close`,
//    its body read with soup_session_send_and_read_async;
//  - every request also has a HARD 5 s deadline (a GCancellable fired by a one-shot timeout source on the cycle's
//    context), so a stalled DNS lookup or connect cannot outlast it. Cancelled, timed out or failed = no HTTP reply
//    (HttpReply::transportOk = false);
//  - the default route is read from routeFile (/proc/net/route) at the start of the cycle. Without one the probes
//    still run and combineCycle() reports NoNetwork;
//  - the confirmation is sent only when combineCycle() would use it: a default route, at least one probe ProbeOk and
//    NO probe Captive (a captive verdict is conclusive, and without a route the result is NoNetwork anyway).
//    Otherwise it is not sent and counts as TlsConfirm::OtherFailure;
//  - the confirmation needs verified TLS, so a confirmUrl that is not https:// is never sent either: it counts as
//    OtherFailure, i.e. Partial when the HTTP probes pass (a mistyped `confirm = http://…` override shows Partial);
//  - a certificate the TLS backend rejects is refused in `accept-certificate`, and its GTlsCertificateFlags are
//    kept, so it reports transportOk = false plus the flags (never transportOk = true with flags);
//  - gAddress / aAddress / confirmAddress: the peer address of the probe's (or the confirmation's) connection
//    (`network-event`, G_SOCKET_CLIENT_CONNECTED), empty when it never connected or the confirmation was not sent;
//  - clock skew comes ONLY from a 204 answer to G (design §5): dateKnown = true and skewSec = G's Date header minus
//    the device clock (time(nullptr)) when that reply arrived — a server ahead of the device gives a positive value —
//    only when G answered 204 with a Date that parses (IMF-fixdate). Every other case (a captive 302, a 200 or 511
//    portal page, Limited, NoNetwork, an unparseable Date) gives dateKnown = false, skewSec = 0: a portal's own Date
//    reflects the portal's clock and must not drive "Tablet clock looks wrong".
//
// libsoup: only API present in libsoup 3.0 is used (the device ships 3.6, the Mac 3.8). Host-tested against
// Homebrew's 3.8; CI builds and runs the same test against the Ubuntu runner's libsoup 3.
//
// Threading and lifetime contract:
//  - runCycle() is asynchronous: it returns at once and `done` runs later, from the ProbeRunner's GMainContext,
//    never from inside runCycle(). Call it on the thread that runs that context, while it is that thread's default
//    (the worker's own loop, or the global default context's thread). The runner pushes the context as
//    thread-default around its libsoup calls, so the session and its async callbacks are bound to it.
//  - One cycle at a time. A runCycle() while a cycle is in flight is a programming error: it logs a g_critical,
//    starts nothing and never calls that `done`. (Task 8's controller never overlaps cycles.)
//  - All per-cycle state lives in one heap object, freed exactly once after the last of its (up to three) requests
//    has finished. `done` runs after that, so it may start the next cycle or destroy the runner.
//  - Destroying the runner mid-cycle cancels the in-flight requests and drops `done` (it will not be called). The
//    cycle state then frees itself when the cancelled requests report back on the context, which therefore has to
//    be iterated again (if it never is, that state leaks, along with libsoup's own pending tasks).
#pragma once
#include <gio/gio.h>
#include <libsoup/soup.h>

#include <ctime>
#include <functional>
#include <initializer_list>
#include <limits>
#include <string>
#include <utility>

#include "config.h"
#include "probe_classify.h"

namespace rmweb::captive {

// probe_classify.h's TlsFlag mirrors GTlsCertificateFlags bit for bit, so the flags pass through unchanged.
static_assert(static_cast<unsigned>(kTlsUnknownCa) == static_cast<unsigned>(G_TLS_CERTIFICATE_UNKNOWN_CA), "TlsFlag");
static_assert(static_cast<unsigned>(kTlsBadIdentity) == static_cast<unsigned>(G_TLS_CERTIFICATE_BAD_IDENTITY), "TlsFlag");
static_assert(static_cast<unsigned>(kTlsNotActivated) == static_cast<unsigned>(G_TLS_CERTIFICATE_NOT_ACTIVATED), "TlsFlag");
static_assert(static_cast<unsigned>(kTlsExpired) == static_cast<unsigned>(G_TLS_CERTIFICATE_EXPIRED), "TlsFlag");
static_assert(static_cast<unsigned>(kTlsRevoked) == static_cast<unsigned>(G_TLS_CERTIFICATE_REVOKED), "TlsFlag");
static_assert(static_cast<unsigned>(kTlsInsecure) == static_cast<unsigned>(G_TLS_CERTIFICATE_INSECURE), "TlsFlag");
static_assert(static_cast<unsigned>(kTlsGenericError) == static_cast<unsigned>(G_TLS_CERTIFICATE_GENERIC_ERROR), "TlsFlag");

struct ProbeEndpoints {
    std::string gUrl, aUrl, confirmUrl, caFile;
    std::string routeFile = "/proc/net/route";
    std::string iface = "wlan0";
};

inline ProbeEndpoints endpointsFrom(const CaptiveConfig& c) {
    ProbeEndpoints ep;
    ep.gUrl = c.gUrl;
    ep.aUrl = c.aUrl;
    ep.confirmUrl = c.confirmUrl;
    ep.caFile = c.caFile;
    return ep;
}

namespace detail {

inline constexpr guint kProbeDeadlineMs = 5000;    // hard per-request deadline
inline constexpr guint kProbeSessionTimeoutS = 5;  // SoupSession:timeout (I/O)
inline constexpr guint kProbeIdleTimeoutS = 1;     // SoupSession:idle-timeout

// Content-Length header value: optional surrounding blanks around 1+ ASCII digits. -1 when absent or invalid
// (empty, a sign, a list like "5, 5", trailing junk, overflow).
inline long long parseContentLength(const char* value) {
    if (value == nullptr) return -1;
    std::string v(value);
    while (!v.empty() && isBlank(v.back())) v.pop_back();
    std::size_t i = 0;
    while (i < v.size() && isBlank(v[i])) ++i;
    if (i == v.size()) return -1;
    long long n = 0;
    for (; i < v.size(); ++i) {
        if (!isDigit(v[i])) return -1;
        if (n > (std::numeric_limits<long long>::max() - 9) / 10) return -1;
        n = n * 10 + (v[i] - '0');
    }
    return n;
}

inline bool isHttpsUrl(const std::string& url) { return g_ascii_strncasecmp(url.c_str(), "https://", 8) == 0; }

}  // namespace detail

class ProbeRunner {
public:
    // ctx: the context the cycles run on (nullptr = the global default context). The runner keeps a reference.
    ProbeRunner(GMainContext* ctx, ProbeEndpoints ep)
        : m_ctx(g_main_context_ref(ctx != nullptr ? ctx : g_main_context_default())), m_ep(std::move(ep)) {}

    ~ProbeRunner() {
        if (m_cycle != nullptr) {                      // detach: no `done`, the cycle frees itself when it drains
            Cycle* c = m_cycle;
            m_cycle = nullptr;
            c->owner = nullptr;
            c->done = nullptr;
            // Collect first: a cancel may complete its request at once and free the cycle, so `c` is not touched
            // after the first g_cancellable_cancel().
            GCancellable* pendingCancels[3] = {nullptr, nullptr, nullptr};
            int n = 0;
            for (Request* r : {&c->g, &c->a, &c->confirm})
                if (r->cancel != nullptr) pendingCancels[n++] = G_CANCELLABLE(g_object_ref(r->cancel));
            for (int i = 0; i < n; ++i) {
                g_cancellable_cancel(pendingCancels[i]);
                g_object_unref(pendingCancels[i]);
            }
        }
        g_main_context_unref(m_ctx);
    }

    ProbeRunner(const ProbeRunner&) = delete;
    ProbeRunner& operator=(const ProbeRunner&) = delete;

    // Starts one probe cycle; `done` gets its report later, on the runner's context. See the contract at the top.
    void runCycle(std::function<void(const CycleReport&)> done) {
        if (m_cycle != nullptr) {                      // g_return_if_fail-style guard, kept under G_DISABLE_CHECKS
            g_critical("rmweb::captive::ProbeRunner::runCycle: a probe cycle is already in flight; call ignored");
            return;
        }
        Cycle* c = new Cycle;
        c->owner = this;
        c->ctx = g_main_context_ref(m_ctx);
        c->done = std::move(done);
        c->route = readDefaultRoute(m_ep);
        c->g.url = m_ep.gUrl;
        c->a.url = m_ep.aUrl;
        c->confirm.url = m_ep.confirmUrl;
        m_cycle = c;

        g_main_context_push_thread_default(m_ctx);
        c->session = newSession(m_ep.caFile);
        startRequest(c, c->g, false);
        startRequest(c, c->a, false);
        if (c->pending == 0) {                         // both URLs unusable: finish from the context, not from here
            GSource* idle = g_idle_source_new();
            g_source_set_callback(idle, onNothingStarted, c, nullptr);
            g_source_attach(idle, c->ctx);
            g_source_unref(idle);
        }
        g_main_context_pop_thread_default(m_ctx);
    }

private:
    struct Cycle;

    struct Request {
        Cycle* cycle = nullptr;
        std::string url;
        SoupMessage* msg = nullptr;                    // owned while in flight
        GCancellable* cancel = nullptr;                // owned while in flight
        GSource* deadline = nullptr;                   // owned while in flight
        HttpReply reply;                               // transportOk stays false unless an HTTP reply arrived
        unsigned tlsFlags = 0;                         // GTlsCertificateFlags seen in accept-certificate
        std::string address;                           // peer address of the connection, "" if it never connected
        std::string date;                              // the reply's Date header
        long long answeredAt = 0;                      // device clock (time(nullptr)) when the reply arrived
    };

    struct Cycle {
        ProbeRunner* owner = nullptr;                  // nullptr once the runner is gone
        GMainContext* ctx = nullptr;                   // ref
        SoupSession* session = nullptr;                // ref
        std::function<void(const CycleReport&)> done;
        bool route = false;
        bool confirmPhase = false;
        int pending = 0;                               // requests started and not yet finished
        Request g, a, confirm;
        ProbeVerdict gv, av;
    };

    static bool readDefaultRoute(const ProbeEndpoints& ep) {
        gchar* text = nullptr;
        gsize len = 0;
        if (!g_file_get_contents(ep.routeFile.c_str(), &text, &len, nullptr)) return false;
        const std::string s(text, len);
        g_free(text);
        return hasDefaultRoute(s, ep.iface);
    }

    static SoupSession* newSession(const std::string& caFile) {
        SoupSession* s = soup_session_new_with_options("timeout", detail::kProbeSessionTimeoutS,
                                                       "idle-timeout", detail::kProbeIdleTimeoutS, nullptr);
        if (!caFile.empty()) {
            GError* err = nullptr;
            GTlsDatabase* db = g_tls_file_database_new(caFile.c_str(), &err);
            if (db != nullptr) {
                g_object_set(s, "tls-database", db, nullptr);
                g_object_unref(db);
            } else {                                   // the confirmation then fails verification -> Partial
                g_warning("rmweb::captive::ProbeRunner: cannot load CA file %s: %s", caFile.c_str(),
                          err != nullptr ? err->message : "unknown error");
                g_clear_error(&err);
            }
        }
        return s;
    }

    // Sends r.url; false (nothing in flight, r.reply stays "no transport") when libsoup cannot parse the URL.
    // Must run with c->ctx pushed as the thread-default context.
    static bool startRequest(Cycle* c, Request& r, bool tlsConfirm) {
        r.cycle = c;
        r.msg = soup_message_new("GET", r.url.c_str());
        if (r.msg == nullptr) return false;
        soup_message_set_flags(r.msg, static_cast<SoupMessageFlags>(SOUP_MESSAGE_NO_REDIRECT | SOUP_MESSAGE_NEW_CONNECTION));
        soup_message_headers_replace(soup_message_get_request_headers(r.msg), "Connection", "close");
        g_signal_connect(r.msg, "network-event", G_CALLBACK(onNetworkEvent), &r);
        if (tlsConfirm) g_signal_connect(r.msg, "accept-certificate", G_CALLBACK(onAcceptCertificate), &r);
        r.cancel = g_cancellable_new();
        r.deadline = g_timeout_source_new(detail::kProbeDeadlineMs);
        g_source_set_callback(r.deadline, onDeadline, &r, nullptr);
        g_source_attach(r.deadline, c->ctx);
        ++c->pending;
        soup_session_send_and_read_async(c->session, r.msg, G_PRIORITY_DEFAULT, r.cancel, onReply, &r);
        return true;
    }

    static void onNetworkEvent(SoupMessage*, GSocketClientEvent event, GIOStream* connection, gpointer user) {
        if (event != G_SOCKET_CLIENT_CONNECTED || !G_IS_SOCKET_CONNECTION(connection)) return;
        GSocketAddress* peer = g_socket_connection_get_remote_address(G_SOCKET_CONNECTION(connection), nullptr);
        if (peer == nullptr) return;
        if (G_IS_INET_SOCKET_ADDRESS(peer)) {
            gchar* s = g_inet_address_to_string(g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(peer)));
            static_cast<Request*>(user)->address = s != nullptr ? s : "";
            g_free(s);
        }
        g_object_unref(peer);
    }

    static gboolean onAcceptCertificate(SoupMessage*, GTlsCertificate*, GTlsCertificateFlags errors, gpointer user) {
        static_cast<Request*>(user)->tlsFlags |= static_cast<unsigned>(errors);
        return FALSE;                                  // never accept: the confirmation needs a verified chain
    }

    static gboolean onDeadline(gpointer user) {
        g_cancellable_cancel(static_cast<Request*>(user)->cancel);   // onReply still runs, with G_IO_ERROR_CANCELLED
        return G_SOURCE_REMOVE;
    }

    static void onReply(GObject* source, GAsyncResult* result, gpointer user) {
        Request& r = *static_cast<Request*>(user);
        GError* err = nullptr;
        GBytes* body = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &err);
        if (body != nullptr) {
            r.reply.transportOk = true;
            r.reply.status = static_cast<int>(soup_message_get_status(r.msg));
            gsize n = 0;
            const char* data = static_cast<const char*>(g_bytes_get_data(body, &n));
            if (data != nullptr && n > 0) r.reply.body.assign(data, n);
            SoupMessageHeaders* h = soup_message_get_response_headers(r.msg);
            r.reply.contentLength = detail::parseContentLength(soup_message_headers_get_one(h, "Content-Length"));
            const char* location = soup_message_headers_get_one(h, "Location");
            r.reply.location = location != nullptr ? location : "";
            const char* date = soup_message_headers_get_one(h, "Date");
            r.date = date != nullptr ? date : "";
            r.answeredAt = static_cast<long long>(std::time(nullptr));
            r.tlsFlags = 0;                            // an answered request carries no certificate errors
            g_bytes_unref(body);
        }
        g_clear_error(&err);                           // no reply: transportOk stays false, tlsFlags as seen
        finishRequest(r);                              // may free the whole cycle: r is dead after this
    }

    static gboolean onNothingStarted(gpointer user) {
        advance(static_cast<Cycle*>(user));
        return G_SOURCE_REMOVE;
    }

    static void finishRequest(Request& r) {
        Cycle* c = r.cycle;
        if (r.deadline != nullptr) {
            g_source_destroy(r.deadline);              // a no-op when the deadline already fired
            g_source_unref(r.deadline);
            r.deadline = nullptr;
        }
        g_clear_object(&r.cancel);
        if (r.msg != nullptr) {
            g_signal_handlers_disconnect_by_data(r.msg, &r);
            g_clear_object(&r.msg);
        }
        if (--c->pending == 0) advance(c);
    }

    // All requests of the current phase are done: start the confirmation or finish the cycle.
    static void advance(Cycle* c) {
        if (c->confirmPhase) {
            const Request& r = c->confirm;
            finish(c, tlsConfirmFrom(r.reply.transportOk, r.reply.transportOk ? 0u : r.tlsFlags, r.reply.status));
            return;
        }
        c->gv = classifyProbe(ProbeKind::Google204, c->g.url, c->g.reply);
        c->av = classifyProbe(ProbeKind::AppleSuccess, c->a.url, c->a.reply);
        const bool anyOk = c->gv.cls == ProbeClass::ProbeOk || c->av.cls == ProbeClass::ProbeOk;
        const bool anyCaptive = c->gv.cls == ProbeClass::Captive || c->av.cls == ProbeClass::Captive;
        if (c->owner != nullptr && c->route && anyOk && !anyCaptive && detail::isHttpsUrl(c->confirm.url)) {
            c->confirmPhase = true;
            g_main_context_push_thread_default(c->ctx);
            const bool started = startRequest(c, c->confirm, true);
            g_main_context_pop_thread_default(c->ctx);
            if (started) return;
        }
        finish(c, TlsConfirm::OtherFailure);           // not sent: combineCycle ignores it, or it cannot be sent
    }

    // Builds the report, frees the cycle, then (runner still alive) calls `done`.
    static void finish(Cycle* c, TlsConfirm confirm) {
        CycleReport report;
        report.outcome = combineCycle(c->route, c->gv, c->av, confirm);
        report.gAddress = c->g.address;
        report.aAddress = c->a.address;
        report.confirmAddress = c->confirm.address;
        if (c->g.reply.transportOk && c->g.reply.status == 204) {   // only a real 204 from G carries the clock
            const long long serverEpoch = parseHttpDate(c->g.date);
            if (serverEpoch >= 0) {
                report.dateKnown = true;
                report.skewSec = serverEpoch - c->g.answeredAt;
            }
        }
        ProbeRunner* owner = c->owner;
        std::function<void(const CycleReport&)> done = std::move(c->done);
        if (owner != nullptr) owner->m_cycle = nullptr;
        g_object_unref(c->session);
        g_main_context_unref(c->ctx);
        delete c;
        if (owner != nullptr && done) done(report);    // last: it may start a new cycle or destroy the runner
    }

    GMainContext* m_ctx;
    ProbeEndpoints m_ep;
    Cycle* m_cycle = nullptr;                          // the cycle in flight, if any
};

}  // namespace rmweb::captive
