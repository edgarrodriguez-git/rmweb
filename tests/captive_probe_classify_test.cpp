// Host unit test for the pure captive-portal probe classifier (no Qt, no libsoup, no device). Build+run on the dev host:
//   clang++ -std=c++17 -o build/captive_probe_classify_test tests/captive_probe_classify_test.cpp && ./build/captive_probe_classify_test
#include "../engine/wpeqt/captive/probe_classify.h"
#include <cstdio>
#include <string>
using namespace rmweb::captive;

static int fails = 0;
#define CHECK(c) do { if(!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while(0)

int main() {
    const std::string G = "http://connectivitycheck.gstatic.com/generate_204", A = "http://captive.apple.com/hotspot-detect.html";
    CHECK(classifyProbe(ProbeKind::Google204, G, {true,204,"",0,""}).cls == ProbeClass::ProbeOk);
    CHECK(classifyProbe(ProbeKind::AppleSuccess, A, {true,200,std::string(kAppleSuccessBody)+"\n",69,""}).cls == ProbeClass::ProbeOk);
    CHECK(classifyProbe(ProbeKind::AppleSuccess, A, {true,200,"<html>loginSuccess</html>",25,""}).cls == ProbeClass::Captive);
    CHECK(classifyProbe(ProbeKind::Google204, G, {true,200,"",0,""}).cls == ProbeClass::Limited);
    CHECK(classifyProbe(ProbeKind::Google204, G, {true,200,"abc",3,""}).cls == ProbeClass::Limited);
    CHECK(classifyProbe(ProbeKind::Google204, G, {true,403,"",0,""}).cls == ProbeClass::Limited);
    CHECK(classifyProbe(ProbeKind::Google204, G, {true,403,"<form>login</form>",18,""}).cls == ProbeClass::Captive);
    CHECK(classifyProbe(ProbeKind::Google204, G, {true,511,"<a href=\"/p\">Sign in</a>",24,""}).cls == ProbeClass::Captive);
    CHECK(classifyProbe(ProbeKind::Google204, G, {false,0,"",-1,""}).cls == ProbeClass::NoNetwork);
    auto v = classifyProbe(ProbeKind::Google204, G, {true,302,"",0,"portal/x/"});
    CHECK(v.cls == ProbeClass::Captive && v.location == "http://connectivitycheck.gstatic.com/portal/x/" && v.probeUrl == G);
    CHECK(resolveUrl("http://h/a/b?q", "//o/p")  == "http://o/p");
    CHECK(resolveUrl("http://h/a/b?q", "?x=1")   == "http://h/a/b?x=1");
    CHECK(resolveUrl("http://h/a/b",   "#f")     == "http://h/a/b#f");
    CHECK(resolveUrl("http://h/a/b/c", "../d")   == "http://h/a/d");
    CHECK(resolveUrl("http://h/a",     "https://x/y") == "https://x/y");
    CHECK(resolveUrl("http://h:8080/a", "/z")    == "http://h:8080/z");
    CHECK(tlsConfirmFrom(true, 0, 204) == TlsConfirm::Ok204);
    CHECK(tlsConfirmFrom(true, 0, 200) == TlsConfirm::OtherFailure);
    CHECK(tlsConfirmFrom(false, kTlsExpired, 0) == TlsConfirm::ValidityOnly);
    CHECK(tlsConfirmFrom(false, kTlsNotActivated, 0) == TlsConfirm::ValidityOnly);
    CHECK(tlsConfirmFrom(false, kTlsExpired|kTlsUnknownCa, 0) == TlsConfirm::OtherFailure);
    CHECK(tlsConfirmFrom(false, 0, 0) == TlsConfirm::OtherFailure);
    ProbeVerdict ok{ProbeClass::ProbeOk,G,""}, okA{ProbeClass::ProbeOk,A,""}, cap{ProbeClass::Captive,A,"http://p/"},
                 lim{ProbeClass::Limited,G,""}, nn{ProbeClass::NoNetwork,G,""};
    CHECK(combineCycle(true, ok, okA, TlsConfirm::Ok204).result == CycleResult::Online);
    CHECK(combineCycle(true, ok, okA, TlsConfirm::ValidityOnly).result == CycleResult::OnlineClockWrong);
    CHECK(combineCycle(true, ok, okA, TlsConfirm::OtherFailure).result == CycleResult::Partial);
    auto c = combineCycle(true, ok, cap, TlsConfirm::Ok204);   // any captive is conclusive; never view a ProbeOk URL
    CHECK(c.result == CycleResult::Captive && c.viewUrl == A && c.locationUrl == "http://p/");
    CHECK(combineCycle(true, lim, nn, TlsConfirm::OtherFailure).result == CycleResult::Limited);
    CHECK(combineCycle(false, ok, okA, TlsConfirm::Ok204).result == CycleResult::NoNetwork);
    CHECK(hasDefaultRoute("Iface\tDestination\tGateway\nwlan0\t00000000\t0101A8C0\n", "wlan0"));
    CHECK(!hasDefaultRoute("Iface\tDestination\tGateway\nwlan0\t0001A8C0\t00000000\n", "wlan0"));
    CHECK(parseHttpDate("Sun, 25 Oct 2026 14:02:12 GMT") == 1792936932LL);
    CHECK(parseHttpDate("garbage") == -1);
    CHECK(clockSkewed(1000, 1301) && !clockSkewed(1000, 1300));
    CHECK(std::string(cycleResultName(CycleResult::Partial)) == "Partial");

    // ---- extra edge cases (beyond the brief's required set) ----

    // classifyProbe: Location handling
    {
        auto p = classifyProbe(ProbeKind::Google204, "http://h:8080/generate_204", {true,302,"",0,"/portal"});
        CHECK(p.cls == ProbeClass::Captive && p.location == "http://h:8080/portal");           // Location resolved with a port
        auto q = classifyProbe(ProbeKind::Google204, G, {true,302,"",0,"https://portal.example/login?x=1#top"});
        CHECK(q.cls == ProbeClass::Captive && q.location == "https://portal.example/login?x=1#top");  // absolute kept
        auto n = classifyProbe(ProbeKind::Google204, G, {true,302,"",0,""});
        CHECK(n.cls == ProbeClass::Captive && n.location.empty() && n.probeUrl == G);          // 302 + empty body is Captive, no Location
        auto f = classifyProbe(ProbeKind::Google204, G, {false,0,"",-1,""});
        CHECK(f.cls == ProbeClass::NoNetwork && f.location.empty() && f.probeUrl == G);
        auto o = classifyProbe(ProbeKind::Google204, G, {true,204,"",0,""});
        CHECK(o.location.empty() && o.probeUrl == G);
    }
    // classifyProbe: a Location whose resolved scheme is not http or https counts as absent (R28) — still Captive, and
    // the view stays on the probe URL
    for (const char* loc : {"javascript:alert(1)", "data:text/html,<b>x</b>", "file:///etc/passwd", "rmweb:tls-continue",
                            "rmweb:settings", "JavaScript:alert(1)", "ftp://h/x", "about:blank", "httpx://h/x"}) {
        auto p = classifyProbe(ProbeKind::Google204, G, {true,302,"",0,loc});
        CHECK(p.cls == ProbeClass::Captive && p.location.empty() && p.probeUrl == G);
        auto q = classifyProbe(ProbeKind::AppleSuccess, A, {true,200,"<html>portal</html>",19,loc});
        CHECK(q.cls == ProbeClass::Captive && q.location.empty());
        auto c = combineCycle(true, p, ProbeVerdict{ProbeClass::ProbeOk,A,""}, TlsConfirm::Ok204);
        CHECK(c.result == CycleResult::Captive && c.viewUrl == G && c.locationUrl.empty());
    }
    {
        auto s = classifyProbe(ProbeKind::Google204, G, {true,302,"",0,"//evil.example/p"});   // scheme-relative: inherits http
        CHECK(s.cls == ProbeClass::Captive && s.location == "http://evil.example/p");
        auto u = classifyProbe(ProbeKind::Google204, G, {true,302,"",0,"HTTPS://Portal/x"});   // scheme case is ignored
        CHECK(u.cls == ProbeClass::Captive && u.location == "HTTPS://Portal/x");
        auto h = classifyProbe(ProbeKind::Google204, G, {true,302,"",0,"Http://portal.example/login"});
        CHECK(h.location == "Http://portal.example/login");
        auto r = classifyProbe(ProbeKind::Google204, "https://h/generate_204", {true,302,"",0,"/login"});
        CHECK(r.location == "https://h/login");                                                // https base kept
    }
    // classifyProbe: Limited rule boundaries (status 200 or 400..599, empty body or 0 <= contentLength <= 4)
    CHECK(classifyProbe(ProbeKind::Google204, G, {true,399,"",0,""}).cls == ProbeClass::Captive);
    CHECK(classifyProbe(ProbeKind::Google204, G, {true,400,"",0,""}).cls == ProbeClass::Limited);
    CHECK(classifyProbe(ProbeKind::Google204, G, {true,599,"",0,""}).cls == ProbeClass::Limited);
    CHECK(classifyProbe(ProbeKind::Google204, G, {true,600,"",0,""}).cls == ProbeClass::Captive);
    CHECK(classifyProbe(ProbeKind::Google204, G, {true,301,"",0,""}).cls == ProbeClass::Captive);
    CHECK(classifyProbe(ProbeKind::Google204, G, {true,200,"abcd",4,""}).cls == ProbeClass::Limited);
    CHECK(classifyProbe(ProbeKind::Google204, G, {true,200,"abcde",5,""}).cls == ProbeClass::Captive);
    CHECK(classifyProbe(ProbeKind::Google204, G, {true,200,"abc",-1,""}).cls == ProbeClass::Captive);   // unknown length + body
    CHECK(classifyProbe(ProbeKind::Google204, G, {true,500,"",-1,""}).cls == ProbeClass::Limited);      // empty body wins
    // classifyProbe: ProbeOk is kind-specific; the Apple body ignores trailing (not leading) whitespace
    CHECK(classifyProbe(ProbeKind::Google204, G, {true,200,std::string(kAppleSuccessBody),69,""}).cls == ProbeClass::Captive);
    CHECK(classifyProbe(ProbeKind::AppleSuccess, A, {true,200,std::string(kAppleSuccessBody)+"\r\n \t",72,""}).cls == ProbeClass::ProbeOk);
    CHECK(classifyProbe(ProbeKind::AppleSuccess, A, {true,200,std::string(" ")+kAppleSuccessBody,70,""}).cls == ProbeClass::Captive);
    CHECK(classifyProbe(ProbeKind::AppleSuccess, A, {true,200,"",0,""}).cls == ProbeClass::Limited);

    // resolveUrl: RFC 3986 §5.4 examples against http://a/b/c/d;p?q
    {
        const std::string B = "http://a/b/c/d;p?q";
        CHECK(resolveUrl(B, "g;x?y#s") == "http://a/b/c/g;x?y#s");
        CHECK(resolveUrl(B, "./g")     == "http://a/b/c/g");
        CHECK(resolveUrl(B, "g/")      == "http://a/b/c/g/");
        CHECK(resolveUrl(B, "/g")      == "http://a/g");
        CHECK(resolveUrl(B, ".")       == "http://a/b/c/");
        CHECK(resolveUrl(B, "./")      == "http://a/b/c/");
        CHECK(resolveUrl(B, "..")      == "http://a/b/");
        CHECK(resolveUrl(B, "../")     == "http://a/b/");
        CHECK(resolveUrl(B, "../g")    == "http://a/b/g");
        CHECK(resolveUrl(B, "../..")   == "http://a/");
        CHECK(resolveUrl(B, "../../g") == "http://a/g");
        CHECK(resolveUrl(B, "../../../g") == "http://a/g");        // cannot climb above the root
        CHECK(resolveUrl(B, "/./g")    == "http://a/g");
        CHECK(resolveUrl(B, "g/./h")   == "http://a/b/c/g/h");
        CHECK(resolveUrl(B, "g/../h")  == "http://a/b/c/h");
        CHECK(resolveUrl(B, "?y")      == "http://a/b/c/d;p?y");
        CHECK(resolveUrl(B, "#s")      == "http://a/b/c/d;p?q#s"); // keeps the base query
        CHECK(resolveUrl(B, "")        == B);                      // empty reference = the base itself
    }
    CHECK(resolveUrl("http://h/a/b?q#frag", "") == "http://h/a/b?q");   // ... minus the base fragment
    CHECK(resolveUrl("http://h", "foo") == "http://h/foo");            // authority with an empty path merges to "/ref"
    CHECK(resolveUrl("http://h", "?x") == "http://h?x");
    CHECK(resolveUrl("http://h/a", "https://x/y/../z") == "https://x/z");  // dot segments removed in an absolute reference too
    CHECK(resolveUrl("http://h/a/b", "//o:81/p?q#f") == "http://o:81/p?q#f");

    // tlsConfirmFrom
    CHECK(tlsConfirmFrom(false, kTlsExpired|kTlsNotActivated, 0) == TlsConfirm::ValidityOnly);
    CHECK(tlsConfirmFrom(false, kTlsGenericError, 0) == TlsConfirm::OtherFailure);
    CHECK(tlsConfirmFrom(false, kTlsNotActivated|kTlsBadIdentity, 0) == TlsConfirm::OtherFailure);
    CHECK(tlsConfirmFrom(true, 0, 204) == TlsConfirm::Ok204 && tlsConfirmFrom(true, 0, 511) == TlsConfirm::OtherFailure);

    // combineCycle: ordering of the rules
    {
        ProbeVerdict capG{ProbeClass::Captive,G,"http://g-portal/"};
        auto both = combineCycle(true, capG, cap, TlsConfirm::Ok204);               // G captive wins the view URL
        CHECK(both.result == CycleResult::Captive && both.viewUrl == G && both.locationUrl == "http://g-portal/");
        auto lc = combineCycle(true, lim, cap, TlsConfirm::OtherFailure);           // captive beats limited
        CHECK(lc.result == CycleResult::Captive && lc.viewUrl == A);
        CHECK(combineCycle(false, capG, cap, TlsConfirm::Ok204).result == CycleResult::NoNetwork);   // no route beats everything
        CHECK(combineCycle(true, ok, nn, TlsConfirm::OtherFailure).result == CycleResult::Partial);  // one ProbeOk is enough
        CHECK(combineCycle(true, nn, okA, TlsConfirm::Ok204).result == CycleResult::Online);
        CHECK(combineCycle(true, ok, lim, TlsConfirm::ValidityOnly).result == CycleResult::OnlineClockWrong);  // ProbeOk beats limited
        ProbeVerdict nnA{ProbeClass::NoNetwork,A,""};
        CHECK(combineCycle(true, nn, nnA, TlsConfirm::Ok204).result == CycleResult::NoNetwork);
        auto online = combineCycle(true, ok, okA, TlsConfirm::Ok204);
        CHECK(online.viewUrl.empty() && online.locationUrl.empty());                // URLs only for Captive
        CHECK(combineCycle(false, ok, okA, TlsConfirm::Ok204).viewUrl.empty());
    }

    // hasDefaultRoute
    CHECK(!hasDefaultRoute("Iface\tDestination\tGateway\neth0\t00000000\t0101A8C0\n", "wlan0"));              // default is on another iface
    CHECK(!hasDefaultRoute("Iface\tDestination\tGateway\nwlan01\t00000000\t0101A8C0\n", "wlan0"));            // whole-name match
    CHECK(hasDefaultRoute("Iface\tDestination\tGateway\neth0\t0001A8C0\t00000000\nwlan0\t00000000\t0101A8C0\n", "wlan0"));
    CHECK(hasDefaultRoute("Iface Destination Gateway\r\nwlan0 00000000 0101A8C0\r\n", "wlan0"));             // spaces + CRLF
    CHECK(hasDefaultRoute("Iface\tDestination\tGateway\nwlan0\t00000000\t0101A8C0", "wlan0"));                // no trailing newline
    CHECK(!hasDefaultRoute("Iface\tDestination\tGateway\n", "wlan0"));                                        // header only
    CHECK(!hasDefaultRoute("", "wlan0"));
    CHECK(!hasDefaultRoute("Iface\tDestination\tGateway\nwlan0\n", "wlan0"));                                 // truncated row

    // parseHttpDate (IMF-fixdate only, UTC without timegm)
    CHECK(parseHttpDate("Thu, 01 Jan 1970 00:00:00 GMT") == 0);
    CHECK(parseHttpDate("Sat, 01 Jan 2000 00:00:00 GMT") == 946684800LL);
    CHECK(parseHttpDate("Sat, 29 Feb 2020 23:59:59 GMT") == 1583020799LL);                // leap day
    CHECK(parseHttpDate("Mon, 29 Feb 2021 00:00:00 GMT") == -1);                          // not a leap year
    CHECK(parseHttpDate("Sun, 25 Oct 2026 14:02:12 UTC") == -1);                          // non-GMT zone
    CHECK(parseHttpDate("Sun, 25 Oct 2026 14:02:12 +0000") == -1);
    CHECK(parseHttpDate("Sun, 25 Oct 2026 14:02:12 gmt") == -1);
    CHECK(parseHttpDate("Sunday, 06-Nov-94 08:49:37 GMT") == -1);                         // obsolete RFC 850 form
    CHECK(parseHttpDate("Sun Nov  6 08:49:37 1994") == -1);                               // obsolete asctime form
    CHECK(parseHttpDate("Sun, 25 Oct 2026 14:02:12 GMT ") == -1);                         // trailing junk
    CHECK(parseHttpDate("Sun, 25 Oct 2026 14:02:1 GMT") == -1);                           // short field
    CHECK(parseHttpDate("Sun, 25 Foo 2026 14:02:12 GMT") == -1);                          // bad month
    CHECK(parseHttpDate("Xyz, 25 Oct 2026 14:02:12 GMT") == -1);                          // bad weekday
    CHECK(parseHttpDate("Sun, 32 Oct 2026 14:02:12 GMT") == -1);                          // day out of range
    CHECK(parseHttpDate("Sun, 00 Oct 2026 14:02:12 GMT") == -1);
    CHECK(parseHttpDate("Sun, 25 Oct 2026 24:02:12 GMT") == -1);                          // hour out of range
    CHECK(parseHttpDate("Sun, 25 Oct 2026 14:60:12 GMT") == -1);
    CHECK(parseHttpDate("Sun, 25 Oct 2026 14:02:6a GMT") == -1);                          // non-digit
    CHECK(parseHttpDate("Sun, 25 Oct 1969 14:02:12 GMT") == -1);                          // before the epoch
    CHECK(parseHttpDate("") == -1);

    // clockSkewed is symmetric
    CHECK(clockSkewed(1301, 1000) && !clockSkewed(1300, 1000) && !clockSkewed(1000, 1000));

    // cycleResultName: every enumerator
    CHECK(std::string(cycleResultName(CycleResult::Online)) == "Online");
    CHECK(std::string(cycleResultName(CycleResult::OnlineClockWrong)) == "OnlineClockWrong");
    CHECK(std::string(cycleResultName(CycleResult::Captive)) == "Captive");
    CHECK(std::string(cycleResultName(CycleResult::Limited)) == "Limited");
    CHECK(std::string(cycleResultName(CycleResult::NoNetwork)) == "NoNetwork");

    if (fails == 0) std::printf("captive_probe_classify_test: OK\n");
    return fails ? 1 : 0;
}
