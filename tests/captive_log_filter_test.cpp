// Host unit test for the captive-portal log filter (redactLogLine) and the size-capped RotatingLog (no Qt, no device).
// Build+run on the dev host:
//   clang++ -std=c++17 -o build/captive_log_filter_test tests/captive_log_filter_test.cpp && ./build/captive_log_filter_test
#include "../engine/wpeqt/captive/log_filter.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
using namespace rmweb::captive;

static int fails = 0;
#define CHECK(c) do { if(!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while(0)

// The redacted text of `line`, or "<dropped>" when the whole line is dropped.
static std::string red(const std::string& line) {
    const std::optional<std::string> r = redactLogLine(line);
    return r ? *r : std::string("<dropped>");
}

static bool exists(const std::string& p) { struct stat st; return ::stat(p.c_str(), &st) == 0; }
static long long sizeOf(const std::string& p) { struct stat st; return ::stat(p.c_str(), &st) == 0 ? (long long)st.st_size : -1; }
static std::string slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// A private scratch directory under $TMPDIR (or /tmp); the destructor removes it with the few files the tests create in it.
struct TmpDir {
    std::string dir;
    TmpDir() {
        const char* t = std::getenv("TMPDIR");
        std::string base = (t && *t) ? t : "/tmp";
        while (base.size() > 1 && base.back() == '/') base.pop_back();
        std::string tpl = base + "/rmweb_logtest_XXXXXX";
        if (::mkdtemp(&tpl[0]) != nullptr) dir = tpl;
    }
    ~TmpDir() {
        if (dir.empty()) return;
        for (const char* n : {"/log", "/log.1", "/dirlog"}) ::unlink((dir + n).c_str());
        for (const char* n : {"/log.1", "/dirlog", "/missing"}) ::rmdir((dir + n).c_str());   // directories some tests plant
        ::rmdir(dir.c_str());
    }
    std::string path(const char* leaf) const { return dir + "/" + leaf; }
};

int main() {
    // ---- redactLogLine: URLs reduced to scheme://host[:port] ----
    CHECK(red("[nav] uri=https://portal.x/login?room=12&name=Smith") == "[nav] uri=https://portal.x");
    CHECK(red("[tls] cert error: https://u:p@h:8443/a#f (x)") == "[tls] cert error: https://h:8443 (x)");
    CHECK(red("[nav] a=http://a.x/p?q=1 b=https://b.y:8080/r#f") == "[nav] a=http://a.x b=https://b.y:8080");
    CHECK(red("[t] display connected @12ms") == "[t] display connected @12ms");
    CHECK(red("") == "");
    CHECK(red("no url here: a/b?c=d#e") == "no url here: a/b?c=d#e");

    // ---- redactLogLine: dropped whole ----
    CHECK(!redactLogLine("[form] setFieldText -> pw\nbob").has_value());   // multi-line: still dropped whole
    CHECK(!redactLogLine("[form] x").has_value());
    CHECK(!redactLogLine("x setFieldText y").has_value());
    CHECK(!redactLogLine("[nav] https://portal.x/?room=1\n[form] filled").has_value());   // the marker may sit on any line

    // ---- redactLogLine: IPv6 hosts stay intact ----
    CHECK(red("http://[::1]:8080/a?b") == "http://[::1]:8080");
    CHECK(red("x http://[::1]:8080 y") == "x http://[::1]:8080 y");
    CHECK(red("(http://[fe80::1]/p?q)") == "(http://[fe80::1])");
    CHECK(red("http://u:p@[::1]:8080/a") == "http://[::1]:8080");

    // ---- redactLogLine: terminators ----
    CHECK(red("(see https://h.x/a?b=1) end") == "(see https://h.x) end");
    CHECK(red("(https://h.x) end") == "(https://h.x) end");
    CHECK(red("\"https://h.x/a?b=1\" end") == "\"https://h.x\" end");
    CHECK(red("'https://u:p@h.x:81/a' end") == "'https://h.x:81' end");
    CHECK(red("<https://h.x/a?b=1> end") == "<https://h.x> end");
    CHECK(red("[https://h.x/a?b=1] end") == "[https://h.x] end");   // the closing bracket matches the opener, so it survives
    CHECK(red("a https://h.x/p?q\tb") == "a https://h.x\tb");
    CHECK(red("https://h.x/p?q\nnext line") == "https://h.x\nnext line");
    CHECK(red("https://h.x?token=abc") == "https://h.x");
    CHECK(red("https://h.x#frag") == "https://h.x");

    // ---- redactLogLine: userinfo, nesting, odd shapes ----
    CHECK(red("https://u:p@ss@h.x/a") == "https://h.x");                            // userinfo runs to the LAST '@'
    CHECK(red("https://h.x/a@b") == "https://h.x");                                 // an '@' in the path is not userinfo
    CHECK(red("http://a.x/go?next=http://b.y/c?d=1 tail") == "http://a.x tail");    // a URL inside a query goes with it
    CHECK(red("file:///etc/hosts") == "file://");
    CHECK(red("HTTPS://H.X/Path") == "HTTPS://H.X");
    CHECK(red("x-app+v1.0://host/p") == "x-app+v1.0://host");
    CHECK(red("1http://h.x/p") == "1http://h.x");                                   // a scheme starts with a letter
    CHECK(red("a:// b") == "a:// b");                                               // empty authority, nothing to cut
    CHECK(red("1234://h.x/p") == "1234://h.x/p");                                   // no letter before "://": not a scheme
    CHECK(red("://h.x/p") == "://h.x/p");
    CHECK(red("http://") == "http://");

    // ---- redactLogLine: the whole tail goes, even past ) ] ' " > (R12) ----
    CHECK(red("[nav] uri=https://h.x/wiki/Foo_(bar)?token=SECRET") == "[nav] uri=https://h.x");
    CHECK(red("https://portal.x/(S(abc123))/login.aspx?room=12&name=Smith") == "https://portal.x");
    CHECK(red("https://portal.x/O'Brien/in?room=12&name=Smith") == "https://portal.x");
    CHECK(red("https://h.x/p?q=a]b&room=12") == "https://h.x");
    CHECK(red("[nav] load failed: https://h.x/p?q=1&r=(2) (err)") == "[nav] load failed: https://h.x (err)");
    CHECK(red("(see https://h.x/a?b=1) end") == "(see https://h.x) end");
    CHECK(red("(https://h.x/Foo_(bar)?token=SECRET)") == "(https://h.x)");
    CHECK(red("https://h.x/wiki/Foo_(bar)?token=SECRET") == "https://h.x");
    CHECK(red("https://portal.x/O'Brien/in?room=12") == "https://portal.x");
    CHECK(red("\"https://h.x/a?b\"") == "\"https://h.x\"");
    CHECK(red("[tls] cert error: https://u:p@h:8443/a#f (x)") == "[tls] cert error: https://h:8443 (x)");
    CHECK(red("a <https://h.x/p?q>") == "a <https://h.x>");
    CHECK(red("x https://h.x/p?a>b=SECRET end") == "x https://h.x end");
    CHECK(red("https://h.x/p?q=1\r\nnext line") == "https://h.x\r\nnext line");     // CR and LF end the tail
    CHECK(red("\"https://h.x/a?b\" and \"https://i.y/c?d\" end") == "\"https://h.x\" and \"https://i.y\" end");
    // The kept closer is only the one that matches the opener around the URL; anything else is removed with the tail.
    CHECK(red("see https://h.x/Foo_(bar)") == "see https://h.x");                    // no opener before the URL
    CHECK(red("(note) see https://h.x/Foo_(bar)") == "(note) see https://h.x");      // the earlier '(' is already closed
    CHECK(red("(a https://h.x/p?q=(1)) b") == "(a https://h.x) b");
    CHECK(red("(https://h.x/p?q]") == "(https://h.x");                               // ']' does not close '('
    CHECK(red("(https://h.x),SECRET") == "(https://h.x");                            // the tail's last char is not a closer
    CHECK(red("[nav] (https://h.x/p?q)\n(https://i.y/r?s)") == "[nav] (https://h.x)\n(https://i.y)");

    // ---- redactLogLine: userinfo holding an authority terminator ) ' " > never leaks its prefix ----
    // The '@' search runs to the RFC 3986 authority end (/ ? # whitespace) first; WebKit leaves ) and ' raw in userinfo.
    CHECK(red("[nav] uri=https://user:pa)ss@h.x/p?room=12") == "[nav] uri=https://h.x");
    CHECK(red("[nav] uri=https://user:pa'ss@h.x/p?room=12") == "[nav] uri=https://h.x");
    CHECK(red("[nav] uri=https://user:pa\"ss@h.x/p?room=12") == "[nav] uri=https://h.x");
    CHECK(red("[nav] uri=https://user:pa>ss@h.x/p?room=12") == "[nav] uri=https://h.x");
    CHECK(red("https://roomguest:Smith'412@portal.example:8443/login") == "https://portal.example:8443");
    // no path: the authority runs to whitespace or the end
    CHECK(red("https://user:pa)ss@h.x") == "https://h.x");
    CHECK(red("https://user:pa'ss@h.x:81") == "https://h.x:81");
    CHECK(red("x https://user:pa\"ss@h.x y") == "x https://h.x y");
    CHECK(red("x https://user:pa>ss@h.x\ty") == "x https://h.x\ty");
    CHECK(red("https://u:p)w@[::1]:8080") == "https://[::1]:8080");
    // inside parentheses / quotes: the matching closer is still kept (R14)
    CHECK(red("(see https://u:p)w@h.x/a) end") == "(see https://h.x) end");
    CHECK(red("(https://u:p)w@h.x) end") == "(https://h.x) end");
    CHECK(red("'https://u:p'w@h.x/a' end") == "'https://h.x' end");
    CHECK(red("\"https://u:p\"w@h.x/a?b\" end") == "\"https://h.x\" end");
    CHECK(red("<https://u:p>w@h.x/a> end") == "<https://h.x> end");
    // the [tls] form
    CHECK(red("[tls] cert error: https://roomguest:Smith'412@portal.example:8443/login (x)")
          == "[tls] cert error: https://portal.example:8443 (x)");
    CHECK(red("[tls] cert error: https://u:p)q@h:8443/a#f (x)") == "[tls] cert error: https://h:8443 (x)");

    // ---- redactLogLine: opaque URLs of a fixed scheme list keep only "scheme:" (R29) ----
    CHECK(red("[nav] uri=data:text/html,<b>Welcome Mr Smith room 412</b>") == "[nav] uri=data:");
    // (the brief's "mailto: (unsupported)": an address tail now runs to the end of the line too, error text included)
    CHECK(red("[nav] load failed: mailto:joe.smith@hotel.example (unsupported)") == "[nav] load failed: mailto:");
    CHECK(red("javascript:alert(document.cookie)") == "javascript:");
    CHECK(red("(mailto:a@b.c)") == "(mailto:)");
    // every listed scheme, ASCII case-insensitive; the scheme is kept as written
    CHECK(red("a vbscript:msgbox(1) b") == "a vbscript:");
    CHECK(red("a blob:https://h.x/550e8400-e29b b") == "a blob: b");
    CHECK(red("a tel:+1-555-0100 b") == "a tel:");
    CHECK(red("a sms:+15550100?body=room%20412 b") == "a sms:");
    CHECK(red("DATA:text/plain,x") == "DATA:");
    CHECK(red("[nav] uri=JavaScript:void(0)") == "[nav] uri=JavaScript:");
    CHECK(red("MailTo:x@y.z end") == "MailTo:");
    // WebKit leaves raw spaces in an opaque path ("tel:+44 20 7946 0123", "mailto:Joe Smith <joe@...>"), so every tail
    // but blob:'s runs to the end of the line; blob: URLs are browser-made and end at whitespace like a scheme:// tail
    CHECK(red("[nav] uri=javascript:alert('Mr Smith, room 412')") == "[nav] uri=javascript:");
    CHECK(red("[nav] uri=data:text/plain,Mr Smith\nnext line") == "[nav] uri=data:\nnext line");
    CHECK(red("[nav] uri=data:a b\r\nnext") == "[nav] uri=data:\r\nnext");
    CHECK(red("a mailto:x@y.z b tel:+1555 c") == "a mailto:");
    CHECK(red("[nav] load failed: tel:+44 20 7946 0123 (x)") == "[nav] load failed: tel:");
    CHECK(red("[nav] load failed: mailto:Joe Smith <joe.smith@hotel.example> (x)") == "[nav] load failed: mailto:");
    CHECK(red("[nav] uri=sms:+1 555 0100?body=Room%20412") == "[nav] uri=sms:");
    CHECK(red("[nav] uri=mailto:Joe Smith\nnext line") == "[nav] uri=mailto:\nnext line");
    CHECK(red("(mailto:Joe Smith <j@h.x>)") == "(mailto:)");                     // R14 closer, end-of-line tail
    CHECK(red("a blob:https://h.x/x b tel:+1555 c") == "a blob: b tel:");
    CHECK(red("(see data:text/plain,x)") == "(see data:)");                       // R14 closer, end-of-line tail
    CHECK(red("(data:a b) end") == "(data:");                                     // the cost: glued prose goes too
    // token boundary: start of the string or a character that is not alphanumeric and not + - .
    CHECK(red("ok\ndata:x") == "ok\ndata:");
    CHECK(red("uri=data:x") == "uri=data:");
    CHECK(red("\"tel:+1555\"") == "\"tel:\"");
    CHECK(red("x-data:abc") == "x-data:abc");
    CHECK(red("x+data:abc") == "x+data:abc");
    CHECK(red("x.data:abc") == "x.data:abc");
    CHECK(red("1data:abc") == "1data:abc");
    CHECK(red("_data:abc") == "_data:");                                          // '_' is not a scheme character
    // near misses stay
    CHECK(red("metadata:x") == "metadata:x");
    CHECK(red("[nav] data: 5 bytes") == "[nav] data: 5 bytes");                    // whitespace, and no ',' follows
    CHECK(red("error: x") == "error: x");
    CHECK(red("data:") == "data:");
    CHECK(red("datax:abc") == "datax:abc");
    CHECK(red("telephone:+1555") == "telephone:+1555");
    CHECK(red("[t] load 12:30:01 ok") == "[t] load 12:30:01 ok");
    // a tail that starts with whitespace is a URL too where WebKit keeps that whitespace in the opaque path
    // ("data: text/html,..." and "mailto: joe@..." serialise unchanged): for data: when a ',' follows on the same line
    // (a data URL needs one; GIO's "Error receiving data: %s" has none), for mailto:, tel: and sms: always
    CHECK(red("[nav] uri=data: text/html,<b>Welcome Mr Smith room 412</b>") == "[nav] uri=data:");
    CHECK(red("[nav] load failed: mailto: joe.smith@hotel.example (x)") == "[nav] load failed: mailto:");
    CHECK(red("[nav] uri=tel: +44 20 7946 0123") == "[nav] uri=tel:");
    CHECK(red("a sms:\t+1555 b") == "a sms:");
    CHECK(red("mailto:\tx") == "mailto:");
    CHECK(red("DATA:\ttext/plain,x") == "DATA:");
    CHECK(red("(see data:  a,b)") == "(see data:)");                             // R14 closer
    CHECK(red("[nav] uri=data: a,b\nnext, line") == "[nav] uri=data:\nnext, line");
    // ... and prose otherwise
    CHECK(red("[nav] load failed: https://h.x/p (Error receiving data: Connection reset by peer)")
          == "[nav] load failed: https://h.x (Error receiving data: Connection reset by peer)");
    CHECK(red("[nav] data: 5 bytes\nnext, line") == "[nav] data: 5 bytes\nnext, line");   // the ',' is on another line
    CHECK(red("data: 5\r,x") == "data: 5\r,x");
    CHECK(red("mailto:\r\nx") == "mailto:\r\nx");                                // CR/LF: an empty tail
    CHECK(red("a blob: x, y") == "a blob: x, y");
    // the scheme:// form stays with the authority path; an opaque scheme inside a scheme:// tail goes with that tail
    CHECK(red("data://h.x/p?q") == "data://h.x");
    CHECK(red("https://h.x/?next=javascript:alert(1) end") == "https://h.x end");
    CHECK(red("https://u:data:x@h.x/p") == "https://h.x");
    CHECK(red("https://h.x/p?q mailto:a@b.c") == "https://h.x mailto:");
    CHECK(red("blob:x https://h.x/p?q") == "blob: https://h.x");
    // linear time: long lines full of colons, near misses and URLs, each URL inside a still-open '(' (a rescan per match
    // or a look-behind per URL takes tens of seconds here; one forward pass takes milliseconds)
    {
        std::string big, want;
        for (int k = 0; k < 40000; ++k) {
            big += "metadata:x a:b:c:d blob:u@h.x data: 5 (a blob:x) (a http://u:p@h.x/p?q) ";
            want += "metadata:x a:b:c:d blob: data: 5 (a blob:) (a http://h.x) ";
        }
        big += "(a mailto:u@h.x tel:+1555)";                  // an end-of-line tail, still inside the first '('
        want += "(a mailto:)";
        const auto t0 = std::chrono::steady_clock::now();
        const std::string got = red(big);
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        CHECK(got == want);
        CHECK(secs < 2.0);
    }
    {   // a ',' far behind many whitespace-led data: near misses is looked up once, not once per near miss
        std::string big;
        for (int k = 0; k < 100000; ++k) big += "data: 5 x ";
        big += "\n,";
        const auto t0 = std::chrono::steady_clock::now();
        const std::string got = red(big);
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        CHECK(got == big);
        CHECK(secs < 2.0);
    }

    // ---- redactLogLine: lines carrying page content are dropped whole (R13) ----
    CHECK(!redactLogLine("[meta] title=Welcome Mr Smith, room 412").has_value());
    CHECK(!redactLogLine("[dl] -> /home/root/Downloads/Smith-invoice.pdf").has_value());
    CHECK(!redactLogLine("[find][dbg] term=smith").has_value());
    CHECK(!redactLogLine("[find] no match").has_value());
    CHECK(!redactLogLine("[search][dbg] words=room 412").has_value());
    CHECK(!redactLogLine("[t] page JS done sy=120 sm=900").has_value());
    CHECK(!redactLogLine("[form] x").has_value() && !redactLogLine("setFieldText").has_value());
    CHECK(!redactLogLine("[nav] uri=https://h.x/\n[meta] title=T").has_value());     // any line of a multi-line message
    CHECK(!redactLogLine("prefix [dl] suffix").has_value());                         // anywhere in the line
    CHECK(!redactLogLine("[library] imported /home/root/Downloads/Smith-invoice.pdf").has_value());   // server file name
    CHECK(!redactLogLine("[library] import failed for /home/root/Downloads/Smith-invoice.pdf: no space").has_value());
    // near misses are not page content and stay
    CHECK(red("[libraryx] x") == "[libraryx] x");
    CHECK(red("[metadata] ok") == "[metadata] ok");
    CHECK(red("[t] page JS started") == "[t] page JS started");
    CHECK(red("[t] pageBy 1 ok") == "[t] pageBy 1 ok");
    CHECK(red("[dlx] ok [find ok") == "[dlx] ok [find ok");

    // ---- redactLogLine: a bare host without a scheme is not a URL ----
    CHECK(red("www.example.com/x?y") == "www.example.com/x?y");
    CHECK(red("see www.example.com/x?y=1 now") == "see www.example.com/x?y=1 now");

    // ---- RotatingLog ----
    umask(0);   // a permissive umask: the 0600 below then comes from the code, not from the environment
    TmpDir tmp;
    CHECK(!tmp.dir.empty());
    if (tmp.dir.empty()) { std::printf("captive_log_filter_test: no temp dir\n"); return 1; }

    {   // brief: maxBytes=100, 150 bytes of lines in total -> path.1 exists, path <= 100
        const std::string p = tmp.path("log");
        RotatingLog log(p, 100);
        for (int i = 0; i < 15; ++i) CHECK(log.append("line-" + std::string(1, char('a' + i)) + "xyz"));   // 9 chars + '\n' = 10 bytes each
        CHECK(exists(p + ".1"));
        CHECK(sizeOf(p) >= 0 && sizeOf(p) <= 100);
        CHECK(sizeOf(p + ".1") == 100);       // 10 lines fill the cap exactly; the 11th (100 + 10 > 100) rotates
        CHECK(sizeOf(p) == 50);
        CHECK(slurp(p + ".1").substr(0, 11) == "line-axyz\nl");
        CHECK(slurp(p).substr(0, 10) == "line-kxyz\n");
        // mode: created 0600
        struct stat st;
        CHECK(::stat(p.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
        CHECK(::stat((p + ".1").c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
        ::unlink(p.c_str()); ::unlink((p + ".1").c_str());
    }

    {   // a fresh file gets the line plus '\n'; an exact fit does not rotate, one byte more does
        const std::string p = tmp.path("log");
        RotatingLog log(p, 20);
        CHECK(log.append("0123456789012345678"));          // 19 + '\n' = 20 = maxBytes
        CHECK(slurp(p) == "0123456789012345678\n" && !exists(p + ".1"));
        CHECK(log.append("x"));                              // 20 + 2 > 20 -> rotate
        CHECK(slurp(p + ".1") == "0123456789012345678\n" && slurp(p) == "x\n");
        ::unlink(p.c_str()); ::unlink((p + ".1").c_str());
    }

    {   // a single line longer than maxBytes is still written, alone in a fresh file
        const std::string p = tmp.path("log");
        RotatingLog log(p, 20);
        const std::string big(50, 'B');
        CHECK(log.append(big));
        CHECK(slurp(p) == big + "\n" && !exists(p + ".1"));   // empty file: nothing to rotate
        CHECK(log.append("s"));                               // next line rotates the oversize one out
        CHECK(slurp(p + ".1") == big + "\n" && slurp(p) == "s\n");
        CHECK(log.append(big));                               // oversize with a non-empty file: rotate first, then write
        CHECK(slurp(p + ".1") == "s\n" && slurp(p) == big + "\n");
        ::unlink(p.c_str()); ::unlink((p + ".1").c_str());
    }

    {   // an old path.1 is replaced on every rotation
        const std::string p = tmp.path("log");
        RotatingLog log(p, 20);
        const std::string a(16, 'a'), b(16, 'b'), c(16, 'c');
        CHECK(log.append(a));
        CHECK(log.append(b));
        CHECK(slurp(p + ".1") == a + "\n" && slurp(p) == b + "\n");
        CHECK(log.append(c));
        CHECK(slurp(p + ".1") == b + "\n" && slurp(p) == c + "\n");   // a is gone
        ::unlink(p.c_str()); ::unlink((p + ".1").c_str());
    }

    {   // an existing file is appended to, not truncated (a second RotatingLog on the same path)
        const std::string p = tmp.path("log");
        { RotatingLog log(p, 100); CHECK(log.append("one")); }
        { RotatingLog log(p, 100); CHECK(log.append("two")); }
        CHECK(slurp(p) == "one\ntwo\n");
        ::unlink(p.c_str());
    }

    {   // default cap is 256 KiB
        const std::string p = tmp.path("log");
        RotatingLog log(p);
        const std::string kb(1023, 'k');                      // 1024 bytes with '\n'
        for (int i = 0; i < 256; ++i) CHECK(log.append(kb));
        CHECK(sizeOf(p) == 262144 && !exists(p + ".1"));
        CHECK(log.append("z"));
        CHECK(exists(p + ".1") && sizeOf(p) == 2);
        ::unlink(p.c_str()); ::unlink((p + ".1").c_str());
    }

    {   // no parent directories are created; failures are reported, never thrown
        RotatingLog log(tmp.path("missing") + "/log", 100);
        CHECK(!log.append("x"));
        CHECK(!exists(tmp.path("missing")));

        ::mkdir(tmp.path("dirlog").c_str(), 0700);            // open fails: the path is a directory
        RotatingLog dl(tmp.path("dirlog"), 100);
        CHECK(!dl.append("x"));
    }

    {   // rename fails (path.1 is a directory): append reports false and leaves the log untouched
        const std::string p = tmp.path("log");
        ::mkdir((p + ".1").c_str(), 0700);
        RotatingLog log(p, 20);
        CHECK(log.append(std::string(16, 'a')));
        CHECK(!log.append(std::string(16, 'b')));
        CHECK(slurp(p) == std::string(16, 'a') + "\n");
        ::unlink(p.c_str());
    }

    if (fails == 0) std::printf("captive_log_filter_test: OK\n");
    return fails ? 1 : 0;
}
