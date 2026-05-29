#pragma once

#include "config.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace symbion {

struct ChatMessage {
    std::string role;
    std::string content;
    std::string created_at;
};

struct EmotionSignal {
    std::string label;
    int intensity = 0;
};

struct SourceChunk {
    std::string title;
    std::string content;
    std::string tags;
    std::string intensity;
    std::string safety_class;
};

class MemoryStore {
public:
    MemoryStore() = default;
    ~MemoryStore();

    MemoryStore(const MemoryStore&) = delete;
    MemoryStore& operator=(const MemoryStore&) = delete;

    bool Open(const std::filesystem::path& db_path);
    void Close();
    bool EnsureSchema();

    bool SaveMessage(const std::string& session_id, const std::string& role, const std::string& content);
    bool SaveEmotion(const std::string& session_id, const std::string& source_text, const EmotionSignal& signal);
    bool ImportCounselingSource(const std::filesystem::path& text_path);
    bool ImportLegacyContext(const std::filesystem::path& legacy_db_path);

    std::vector<ChatMessage> RecentMessages(const std::string& session_id, int limit) const;
    std::vector<ChatMessage> AmbientContext(int limit) const;
    std::vector<ChatMessage> RetrieveRelevant(const std::string& query, int limit) const;
    std::vector<SourceChunk> SearchCounselingSources(const std::string& query, bool include_high_intensity, int limit) const;
    std::vector<EmotionSignal> RecentEmotionSignals(int limit) const;
    int DeleteSession(const std::string& session_id);
    int DeleteMatching(const std::string& query);
    int WipeAll();
    int MessageCount() const;

private:
    sqlite3* db_ = nullptr;
};

EmotionSignal DetectEmotion(std::string_view text);
std::vector<std::string> QueryTerms(std::string_view text);

}  // namespace symbion
