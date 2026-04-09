#include "jsonrpc.hpp"
#include <unistd.h>
#include <poll.h>
#include <csignal>
#include <chrono>

namespace {
    int pipefd[2] = {-1, -1};
    volatile std::atomic<bool> g_quit{false};
}

static void signal_handler(int) {
    g_quit = true;
    if (pipefd[1] != -1) {
        write(pipefd[1], "quit", 4);
    }
}

int main() {
    if (pipe(pipefd) == -1) {
        std::cerr << "Failed to create pipe" << std::endl;
        return 1;
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    auto waker = []() {
        if (pipefd[1] != -1) {
            write(pipefd[1], "wake", 4);
        }
    };

    jsonrpc::Conn server(waker);

    server.register_method("add", [](const jsonrpc::json& params) {
        return params[0].get<double>() + params[1].get<double>();
        });

    server.register_async_method("heavy_task", [](jsonrpc::Context ctx, const jsonrpc::json& params) {
        std::thread([ctx]() mutable {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            ctx.reply("Task Complete!");
        }).detach();
    });

    server.register_notification("exit", [](const jsonrpc::json&) {
        g_quit = true;
        if (pipefd[1] != -1) {
            write(pipefd[1], "quit", 4);
        }
    });

    server.start();

    pollfd pfd[2] = {
        {pipefd[0], POLLIN, 0},
        {STDIN_FILENO, POLLIN, 0}
    };

    while (!g_quit && server.is_running()) {
        int ret = poll(pfd, 2, 10);

        if (ret > 0) {
            if (pfd[0].revents & POLLIN) {
                char buf[64];
                read(pipefd[0], buf, sizeof(buf));
            }
            if (pfd[1].revents & POLLIN) {
                server.process_queue();
            }
        }

        if (g_quit) break;
        server.process_queue();
    }

    server.stop();

    if (pipefd[0] != -1) close(pipefd[0]);
    if (pipefd[1] != -1) close(pipefd[1]);

    return 0;
}