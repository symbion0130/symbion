#pragma once

#include "config.h"
#include "gemma_client.h"
#include "http_server.h"
#include "memory_store.h"

#include <atomic>
#include <filesystem>
#include <unordered_set>

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
    HttpResponse HandleForget(const HttpRequest& request);
    HttpResponse HandleTechniques(const HttpRequest& request);
    HttpResponse HandleTechniqueCommand(const std::string& session_id, const std::string& message);
    HttpResponse HandleTechniqueSync(const HttpRequest& request);
    HttpResponse HandleSessions(const HttpRequest& request);
    HttpResponse HandleSessionMessages(const HttpRequest& request, const std::string& session_id) const;
    HttpResponse HandleProfileFact(const HttpRequest& request) const;
    HttpResponse HandleRecent() const;
    HttpResponse HandleEmotions() const;
    HttpResponse HandleEmotionCheckins(const HttpRequest& request);
    HttpResponse HandleRelevantMemory(const HttpRequest& request) const;
    HttpResponse HandleHome() const;
    HttpResponse HandleAsset(const std::string& path) const;

    std::filesystem::path repo_root_;
    Config config_;
    MemoryStore memory_;
    GemmaClient gemma_;
    std::unordered_set<std::string> pending_wipe_sessions_;
};

}  // namespace symbion
