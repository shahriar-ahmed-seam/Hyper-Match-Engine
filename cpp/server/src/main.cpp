// hme_engine_server: the real, runnable TCP front-end for the Matching_Engine.
//
// Composes the existing in-process pipeline (ServerPipeline) with the real
// select()-based TcpServer + SelectReadinessPoller and runs the event loop
// until interrupted (Ctrl-C / SIGINT). It parses a few flags, brings the engine
// to operational state, prints a startup line, serves, and prints a clean
// shutdown line. No protocol parsing happens here -- the pipeline frames inbound
// bytes and encodes outbound events; this binary only moves bytes over TCP.

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>

#include "hme/server_pipeline.hpp"
#include "hme/tcp_server.hpp"

namespace {

using Pipeline = hme::server::ServerPipeline<>;
using Server = hme::network::TcpServer<Pipeline>;

// Set on construction of the server so the signal handler can request a stop.
// std::atomic store is sufficiently async-signal-safe for this use.
std::atomic<Server*> g_server{nullptr};

extern "C" void handle_sigint(int /*signum*/) {
    Server* server = g_server.load(std::memory_order_relaxed);
    if (server != nullptr) {
        server->stop();
    }
}

struct Options {
    std::string host = "0.0.0.0";
    std::uint16_t port = 9001;
    std::uint32_t max_connections = hme::network::kMaxConnections;
};

bool parse_args(int argc, char** argv, Options& out) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << name << "\n";
                return nullptr;
            }
            return argv[++i];
        };
        if (arg == "--host") {
            const char* v = next("--host");
            if (!v) return false;
            out.host = v;
        } else if (arg == "--port") {
            const char* v = next("--port");
            if (!v) return false;
            out.port = static_cast<std::uint16_t>(std::strtoul(v, nullptr, 10));
        } else if (arg == "--max-connections") {
            const char* v = next("--max-connections");
            if (!v) return false;
            out.max_connections =
                static_cast<std::uint32_t>(std::strtoul(v, nullptr, 10));
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "usage: hme_engine_server [--host H] [--port P] "
                         "[--max-connections N]\n";
            return false;
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Options opts;
    if (!parse_args(argc, argv, opts)) {
        return 1;
    }

    try {
        // Winsock must be up before any socket call (no-op on POSIX).
        hme::network::WinsockGuard winsock_guard;

        // Real readiness poller + assembled in-process pipeline over it.
        // The pipeline reserves all order/price/ring storage inline, so it is
        // many megabytes -- allocate it on the heap, not the stack.
        hme::network::SelectReadinessPoller poller;
        auto pipeline = std::make_unique<Pipeline>(poller, opts.max_connections);
        if (!pipeline->initialize()) {
            std::cerr << "engine initialization failed; aborting\n";
            return 1;
        }

        // The real TCP server (binds + listens here).
        Server server(opts.host, opts.port, *pipeline, poller);
        g_server.store(&server, std::memory_order_relaxed);

        // Clean shutdown on Ctrl-C.
        std::signal(SIGINT, handle_sigint);
#ifdef SIGTERM
        std::signal(SIGTERM, handle_sigint);
#endif

        std::cout << "hme-engine listening on " << opts.host << ":"
                  << opts.port << std::endl;

        server.run();

        g_server.store(nullptr, std::memory_order_relaxed);
        std::cout << "hme-engine shutting down cleanly (accepted="
                  << server.accepted_count()
                  << ", closed=" << server.closed_count() << ")" << std::endl;
    } catch (const std::exception& ex) {
        std::cerr << "fatal: " << ex.what() << std::endl;
        return 1;
    }
    return 0;
}
