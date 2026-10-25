// probe-cli: runs ONE captive-portal probe cycle (engine/wpeqt/captive/probe_runner.h) and prints its outcome as a
// single line, so the libsoup runner can be tested on the host against tools/fake-portal (tests/probe_runner_test.sh).
//
//   probe-cli --google URL --apple URL --confirm URL [--ca FILE] [--route-file FILE] [--iface NAME]
//   -> result=<cycleResultName> view=<url> location=<url> g_addr=<addr> skew=<seconds|unknown>
//
// Exit 0 once a cycle ran (whatever its result), 2 on a usage error. A GIO without a TLS backend (no
// glib-networking) is reported on stderr as "no TLS backend": the HTTPS confirmation cannot succeed without one.
//
// Build: clang++ -std=c++17 -Wall -Wextra -o build/probe-cli tools/probe-cli/probe-cli.cpp \
//            $(pkg-config --cflags --libs libsoup-3.0 gio-2.0)
#include <cstdio>
#include <cstring>
#include <string>

#include "../../engine/wpeqt/captive/probe_runner.h"

using namespace rmweb::captive;

namespace {

int usage(const char* argv0, bool error) {
    std::fprintf(error ? stderr : stdout,
                 "usage: %s --google URL --apple URL --confirm URL [--ca FILE] [--route-file FILE] [--iface NAME]\n"
                 "Runs one captive-portal probe cycle and prints:\n"
                 "  result=<result> view=<url> location=<url> g_addr=<addr> skew=<seconds|unknown>\n"
                 "Defaults: --route-file /proc/net/route, --iface wlan0, no --ca (system trust).\n",
                 argv0);
    return error ? 2 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    CaptiveConfig cfg;
    cfg.gUrl.clear();
    cfg.aUrl.clear();
    cfg.confirmUrl.clear();
    std::string routeFile, iface;
    for (int i = 1; i < argc; ++i) {
        const char* opt = argv[i];
        if (std::strcmp(opt, "-h") == 0 || std::strcmp(opt, "--help") == 0) return usage(argv[0], false);
        std::string* target = nullptr;
        if (std::strcmp(opt, "--google") == 0) target = &cfg.gUrl;
        else if (std::strcmp(opt, "--apple") == 0) target = &cfg.aUrl;
        else if (std::strcmp(opt, "--confirm") == 0) target = &cfg.confirmUrl;
        else if (std::strcmp(opt, "--ca") == 0) target = &cfg.caFile;
        else if (std::strcmp(opt, "--route-file") == 0) target = &routeFile;
        else if (std::strcmp(opt, "--iface") == 0) target = &iface;
        if (target == nullptr) {
            std::fprintf(stderr, "probe-cli: unknown option %s\n", opt);
            return usage(argv[0], true);
        }
        if (i + 1 >= argc) {
            std::fprintf(stderr, "probe-cli: %s needs a value\n", opt);
            return usage(argv[0], true);
        }
        *target = argv[++i];
    }
    if (cfg.gUrl.empty() || cfg.aUrl.empty() || cfg.confirmUrl.empty()) {
        std::fprintf(stderr, "probe-cli: --google, --apple and --confirm are required\n");
        return usage(argv[0], true);
    }

    if (!g_tls_backend_supports_tls(g_tls_backend_get_default()))
        std::fprintf(stderr, "probe-cli: warning: GIO has no TLS backend (install glib-networking); "
                             "the HTTPS confirmation cannot succeed\n");

    ProbeEndpoints ep = endpointsFrom(cfg);
    if (!routeFile.empty()) ep.routeFile = routeFile;
    if (!iface.empty()) ep.iface = iface;

    GMainContext* ctx = g_main_context_default();
    GMainLoop* loop = g_main_loop_new(ctx, FALSE);
    CycleReport report;
    {
        ProbeRunner runner(ctx, ep);
        runner.runCycle([&report, loop](const CycleReport& r) {
            report = r;
            g_main_loop_quit(loop);
        });
        g_main_loop_run(loop);
    }
    g_main_loop_unref(loop);

    const std::string skew = report.dateKnown ? std::to_string(report.skewSec) : std::string("unknown");
    std::printf("result=%s view=%s location=%s g_addr=%s skew=%s\n", cycleResultName(report.outcome.result),
                report.outcome.viewUrl.c_str(), report.outcome.locationUrl.c_str(), report.gAddress.c_str(),
                skew.c_str());
    return 0;
}
