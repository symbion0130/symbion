#pragma once

#include <filesystem>
#include <string>

namespace symbion {

struct Config {
    int port = 8000;
    int local_gemma_max_tokens = 768;
    int local_gemma_context_char_budget = 12000;
    int local_gemma_recent_turns = 8;
    double temperature = 0.82;
    std::string provider = "local_gemma";
    std::string gemma_base_url = "http://127.0.0.1:8088/v1";
    std::string gemma_model = "local-gemma";
    std::string db_path = "data/symbion.db";
    std::string legacy_memory_db_path = "";
    std::string shared_learnings_path = "";
};

Config LoadConfig(const std::filesystem::path& repo_root);

}  // namespace symbion
