#pragma once

#include "config.h"
#include "gemma_client.h"
#include "http_server.h"
#include "memory_store.h"

#include <atomic>
#include <filesystem>

namespace symbion {

class App {
public:
    explicit App(std::filesystem::path repo_root);

    bool Initialize();
    int Run(const std::atomic_bool& running);
    HttpResponse Handle(const HttpRequest& request);

private:
    HttpResponse HandleHealth() const;
    HttpResponse HandleChat(const HttpRequest& request);
    HttpResponse HandleRecent() const;
    HttpResponse HandleEmotions() const;
    HttpResponse HandleRelevantMemory(const HttpRequest& request) const;
    HttpResponse HandleHome() const;

    std::filesystem::path repo_root_;
    Config config_;
    MemoryStore memory_;
    GemmaClient gemma_;
};

}  // namespace symbion
