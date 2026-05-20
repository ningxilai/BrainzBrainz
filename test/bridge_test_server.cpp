// Test server for bidirectional JSON-RPC (C++ ↔ Emacs)
// Registers methods that use context.call_emacs() / notify_emacs()
// to test the bidirectional RPC feature.

#include <iostream>
#include <atomic>
#include <chrono>
#include <thread>
#include <unistd.h>
#include "jsonrpc.hpp"

using namespace jsonrpc;

std::atomic<bool> g_quit{ false };

int main() {
    auto waker = []() {};

    // Use STDIN_FILENO so the reader thread uses poll() with timeout
    // and can be interrupted during shutdown.
    Conn conn(waker, std::cin, std::cout, std::cerr, Conn::kDefaultMaxContentLength, STDIN_FILENO);

    // Method that calls back into Emacs synchronously
    conn.register_async_method("ping_emacs", [](Context ctx, const json& params) {
        try {
            json result = ctx.call_emacs("echo", params);
            ctx.reply(result);
        } catch (const std::exception& e) {
            ctx.error(-1, e.what());
        }
    });

    // Method that calls back into Emacs and expects a numeric result
    conn.register_async_method("emacs_add", [](Context ctx, const json& params) {
        try {
            json result = ctx.call_emacs("add", params);
            ctx.reply(result);
        } catch (const std::exception& e) {
            ctx.error(-1, e.what());
        }
    });

    // Method that sends a notification to Emacs (no response expected)
    conn.register_async_method("notify_emacs", [](Context ctx, const json& params) {
        ctx.notify_emacs("logged", params);
        ctx.reply("notification sent");
    });

    // Standard echo method
    conn.register_method("echo", [](const json& params) {
        return params;
    });

    // Standard exit notification
    conn.register_notification("exit", [](const json&) {
        g_quit = true;
    });

    conn.start();

    while (!g_quit && conn.is_running()) {
        conn.process_queue();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    conn.stop();
    return 0;
}
