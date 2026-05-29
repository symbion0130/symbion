#include "config.h"

#include "json_util.h"

#include <fstream>
#include <sstream>

namespace symbion {

namespace {

std::string ReadTextFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

}  // namespace

Config LoadConfig(const std::filesystem::path& repo_root) {
    Config config;
    std::string json = ReadTextFile(repo_root / "config" / "symbion.json");
    if (json.empty()) {
        json = ReadTextFile(repo_root / "symbion.json");
    }
    if (auto value = ExtractJsonInt(json, "web_port")) config.port = *value;
    if (auto value = ExtractJsonInt(json, "local_gemma_max_tokens")) config.local_gemma_max_tokens = *value;
    if (auto value = ExtractJsonInt(json, "local_gemma_context_char_budget")) config.local_gemma_context_char_budget = *value;
    if (auto value = ExtractJsonInt(json, "local_gemma_recent_turns")) config.local_gemma_recent_turns = *value;
    if (auto value = ExtractJsonDouble(json, "temperature")) config.temperature = *value;
    if (auto value = ExtractJsonString(json, "llm_provider")) config.provider = *value;
    if (auto value = ExtractJsonString(json, "local_gemma_base_url")) config.gemma_base_url = *value;
    if (auto value = ExtractJsonString(json, "local_gemma_model")) config.gemma_model = *value;
    if (auto value = ExtractJsonString(json, "db_path")) config.db_path = *value;
    if (auto value = ExtractJsonString(json, "legacy_memory_db_path")) config.legacy_memory_db_path = *value;
    return config;
}

}  // namespace symbion
