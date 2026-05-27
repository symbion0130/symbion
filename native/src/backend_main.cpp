#include "app.h"

#include <atomic>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <string_view>

namespace {

std::atomic_bool g_running = true;

void HandleSignal(int) {
    g_running = false;
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    std::filesystem::path repo_root = std::filesystem::current_path();
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string_view(argv[i]) == "--repo") {
            repo_root = argv[i + 1];
        }
    }

    symbion::App app(repo_root);
    if (!app.Initialize()) {
        std::cerr << "Symbion backend failed to initialize\n";
        return 1;
    }

    return app.Run(g_running);
}
