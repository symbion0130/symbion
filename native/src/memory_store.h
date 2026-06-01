#pragma once

#include "config.h"

#include <cstdint>
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

struct TechniqueItem {
    int id = 0;
    std::string query;
    std::string move;
    std::string evidence;
    std::string source;
};

struct KnowledgeGap {
    int id = 0;
    std::string topic;
    std::string description;
};

struct EmotionCheckin {
    int id = 0;
    std::string timestamp;
    std::string session;
    std::string user;
    std::string emotion;
    std::optional<int> intensity;
    std::optional<double> valence;
    std::string body_location;
    std::string trigger;
    std::string note;
    std::optional<std::int64_t> source_message_id;
    std::optional<double> confidence;
    std::string captured_by;
};

struct SessionInfo {
    std::string id;
    std::string title;
    std::string last_activity;
    int turn_count = 0;
};

class SummaryGenerator {
public:
    virtual ~SummaryGenerator() = default;
    virtual std::string SummarizeSessionWindow(const std::vector<ChatMessage>& messages,
                                               const std::string& prior_summary) const = 0;
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
    // Caller retains ownership. The generator must outlive this MemoryStore.
    void SetSummaryGenerator(const SummaryGenerator* generator);

    bool SaveMessage(const std::string& session_id,
                     const std::string& user,
                     const std::string& role,
                     const std::string& content);
    bool SaveEmotion(const std::string& session_id,
                     const std::string& user,
                     const std::string& source_text,
                     const EmotionSignal& signal);
    bool SaveEmotionCheckin(const EmotionCheckin& checkin);
    bool ImportCounselingSource(const std::filesystem::path& text_path);
    bool ImportLegacyContext(const std::filesystem::path& legacy_db_path);
    int ImportSharedTechniques(const std::filesystem::path& path);
    int ExportSharedTechniques(const std::filesystem::path& path) const;
    bool SaveTechnique(const std::string& session_id,
                       const std::string& query,
                       const std::string& move,
                       const std::string& evidence);
    std::vector<TechniqueItem> ListTechniques(int limit) const;
    bool DeleteTechnique(int id);
    int SummarizeSessionIfNeeded(const std::string& session_id, int threshold);
    bool RecordKnowledgeGap(const std::string& session_id,
                            const std::string& topic,
                            const std::string& description);
    bool CaptureKnowledgeGap(const std::string& session_id,
                             const std::string& user_message,
                             const std::string& assistant_reply);
    std::vector<KnowledgeGap> ListKnowledgeGaps(int limit) const;

    std::vector<ChatMessage> RecentMessages(const std::string& session_id, int limit) const;
    std::vector<ChatMessage> RecentSessionSummaries(const std::string& session_id, int limit) const;
    std::vector<ChatMessage> AmbientContext(const std::string& user, int limit) const;
    std::vector<ChatMessage> RetrieveRelevant(const std::string& user, const std::string& query, int limit) const;
    std::vector<SessionInfo> ListSessions(const std::string& user, int limit) const;
    std::optional<std::string> GetProfileFact(const std::string& user, const std::string& key) const;
    std::vector<SourceChunk> SearchCounselingSources(const std::string& query, bool include_high_intensity, int limit) const;
    std::vector<EmotionSignal> RecentEmotionSignals(const std::string& user, int limit) const;
    std::vector<EmotionCheckin> RecentEmotionCheckins(const std::string& user,
                                                      int limit,
                                                      int days,
                                                      const std::string& emotion) const;
    std::vector<EmotionCheckin> RecentEmotionCheckins(int limit) const;
    int DeleteSession(const std::string& session_id);
    int DeleteMatching(const std::string& query);
    int WipeAll();
    int MessageCount() const;

private:
    sqlite3* db_ = nullptr;
    const SummaryGenerator* summary_generator_ = nullptr;
};

EmotionSignal DetectEmotion(std::string_view text);
std::vector<std::string> QueryTerms(std::string_view text);

}  // namespace symbion
