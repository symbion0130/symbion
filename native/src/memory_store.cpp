#include "memory_store.h"

#include "sqlite3.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>
#include <unordered_map>
#include <unordered_set>

namespace symbion {

namespace {

bool Exec(sqlite3* db, const char* sql) {
    char* error = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &error);
    if (error) {
        std::cerr << "SQLite exec error: " << error << "\nSQL: " << sql << "\n";
        sqlite3_free(error);
    }
    return rc == SQLITE_OK;
}

bool TableHasColumn(sqlite3* db, const char* table, const char* column) {
    sqlite3_stmt* stmt = nullptr;
    std::string sql = "PRAGMA table_info(" + std::string(table) + ");";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return false;
    bool found = false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* name = sqlite3_column_text(stmt, 1);
        if (name && std::string(reinterpret_cast<const char*>(name)) == column) {
            found = true;
            break;
        }
    }
    sqlite3_finalize(stmt);
    return found;
}

bool AddColumnIfMissing(sqlite3* db, const char* table, const char* column, const char* definition) {
    if (TableHasColumn(db, table, column)) return true;
    const std::string sql = "ALTER TABLE " + std::string(table) + " ADD COLUMN " + definition + ";";
    return Exec(db, sql.c_str());
}

std::string StableHash(std::string_view value) {
    uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char c : value) {
        hash ^= static_cast<uint64_t>(c);
        hash *= 1099511628211ULL;
    }
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}

std::string TechniqueHash(const std::string& user, const std::string& query, const std::string& move) {
    const std::string material = user + "\x1f" + query + "\x1f" + move;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_size = 0;
    DWORD data_size = 0;
    DWORD hash_size = 0;
    std::vector<unsigned char> object;
    std::vector<unsigned char> digest;
    std::string out;

    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &data_size, 0) == 0 &&
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                          reinterpret_cast<PUCHAR>(&hash_size), sizeof(hash_size), &data_size, 0) == 0) {
        object.resize(object_size);
        digest.resize(hash_size);
        if (BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0) == 0 &&
            BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(material.data())),
                           static_cast<ULONG>(material.size()), 0) == 0 &&
            BCryptFinishHash(hash, digest.data(), hash_size, 0) == 0) {
            std::ostringstream hex;
            for (const unsigned char byte : digest) {
                hex << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
            }
            out = hex.str().substr(0, 12);
        }
    }
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return out.empty() ? StableHash(material).substr(0, 12) : out;
}

bool ContainsInjectionMarker(const std::string& text) {
    std::string lower(text);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    static const std::initializer_list<const char*> markers = {
        "[tool_data", "[/tool_data]", "[symbion_revise]",
        "[thinking_start]", "[thinking_end]", "<tool_call>", "<function_call>"
    };
    for (const char* marker : markers) {
        if (lower.find(marker) != std::string::npos) return true;
    }
    return false;
}

std::string Truncate(std::string value, size_t max_len) {
    if (value.size() <= max_len) return value;
    value.resize(max_len);
    return value;
}

std::string NowSql() {
    return "datetime('now')";
}

std::string Lower(std::string_view value) {
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

bool ContainsAny(const std::string& text, const std::vector<std::string>& words) {
    return std::any_of(words.begin(), words.end(), [&](const std::string& word) {
        return text.find(word) != std::string::npos;
    });
}

double TermScore(const std::string& text, const std::vector<std::string>& terms) {
    if (terms.empty()) return 0.0;
    const std::string lower = Lower(text);
    double score = 0.0;
    for (const auto& term : terms) {
        size_t pos = lower.find(term);
        if (pos == std::string::npos) continue;
        score += 1.0;
        while ((pos = lower.find(term, pos + term.size())) != std::string::npos) {
            score += 0.25;
        }
    }
    return score / static_cast<double>(terms.size());
}

void BindText(sqlite3_stmt* stmt, int index, const std::string& value) {
    sqlite3_bind_text(stmt, index, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
}

void BindOptionalInt(sqlite3_stmt* stmt, int index, const std::optional<int>& value) {
    if (value) {
        sqlite3_bind_int(stmt, index, *value);
    } else {
        sqlite3_bind_null(stmt, index);
    }
}

void BindOptionalInt64(sqlite3_stmt* stmt, int index, const std::optional<std::int64_t>& value) {
    if (value) {
        sqlite3_bind_int64(stmt, index, *value);
    } else {
        sqlite3_bind_null(stmt, index);
    }
}

void BindOptionalDouble(sqlite3_stmt* stmt, int index, const std::optional<double>& value) {
    if (value) {
        sqlite3_bind_double(stmt, index, *value);
    } else {
        sqlite3_bind_null(stmt, index);
    }
}

std::string ColumnText(sqlite3_stmt* stmt, int index) {
    const unsigned char* text = sqlite3_column_text(stmt, index);
    return text ? reinterpret_cast<const char*>(text) : "";
}

std::string TrimWhitespace(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

int WordCountText(const std::string& text) {
    int count = 0;
    bool in_word = false;
    for (const unsigned char c : text) {
        if (std::isalnum(c)) {
            if (!in_word) ++count;
            in_word = true;
        } else {
            in_word = false;
        }
    }
    return count;
}

std::string ClipText(std::string value, size_t limit) {
    value = TrimWhitespace(std::move(value));
    std::replace(value.begin(), value.end(), '\n', ' ');
    while (value.find("  ") != std::string::npos) {
        value.replace(value.find("  "), 2, " ");
    }
    if (value.size() <= limit) return value;
    if (limit <= 3) return value.substr(0, limit);
    return value.substr(0, limit - 3) + "...";
}

std::string FirstSentence(const std::string& text, size_t limit) {
    std::string flat = ClipText(text, limit);
    const size_t stop = flat.find_first_of(".?!");
    if (stop != std::string::npos && stop + 1 < flat.size() && stop + 1 <= limit) {
        flat = flat.substr(0, stop + 1);
    }
    return ClipText(flat, limit);
}

bool LooksQuestionLike(const std::string& text) {
    const std::string lower = Lower(text);
    if (lower.find('?') != std::string::npos) return true;
    return lower.rfind("what ", 0) == 0 || lower.rfind("why ", 0) == 0 ||
           lower.rfind("how ", 0) == 0 || lower.rfind("where ", 0) == 0 ||
           lower.rfind("who ", 0) == 0 || lower.rfind("when ", 0) == 0 ||
           lower.rfind("which ", 0) == 0 || lower.rfind("can ", 0) == 0 ||
           lower.rfind("could ", 0) == 0 || lower.rfind("should ", 0) == 0;
}

bool UserNamesKnowledgeGap(const std::string& text) {
    const std::string lower = Lower(text);
    return ContainsAny(lower, {
        "need to figure out", "need to find out", "we should find out",
        "look up later", "research later", "knowledge gap", "unknown right now",
        "something to investigate"
    });
}

bool AssistantSignalsKnowledgeGap(const std::string& text) {
    const std::string lower = Lower(text);
    return ContainsAny(lower, {
        "i don't know", "i do not know", "i'm not sure", "i am not sure",
        "i'm unsure", "i am unsure", "need your city", "need your location",
        "tell me your city", "tell me your neighborhood", "without that",
        "don't have enough", "do not have enough", "need one more bit",
        "need one more detail", "need current", "cannot check", "can't check",
        "need to inspect", "need to look"
    });
}

bool LooksEmotionallyImportantForSummary(const std::string& text) {
    const std::string lower = Lower(text);
    return ContainsAny(lower, {
        "i feel", "i'm feeling", "im feeling", "i am feeling",
        "ashamed", "shame", "stuck", "not enough", "inadequate",
        "destructive habit", "destructive habits", "habits that were destructive",
        "not being good", "hurting people", "people around me",
        "afraid", "anxious", "anxiety", "pressure", "wrong step",
        "rough", "uphill battle", "down to my bones", "kill myself",
        "hurt myself", "want to die", "trauma", "ptsd", "abuse",
        "which habit", "most damage", "truth on the table", "slowly and gently",
        "what makes you feel", "what is it connected", "what feels most intense"
    });
}

struct StoredMessage {
    sqlite3_int64 id = 0;
    std::string role;
    std::string content;
    std::string created_at;
};

std::string SummarySnippet(const StoredMessage& msg, size_t ordinary_limit) {
    if (LooksEmotionallyImportantForSummary(msg.content)) {
        return ClipText(msg.content, 300);
    }
    return FirstSentence(msg.content, ordinary_limit);
}

std::string JoinTopTerms(const std::vector<StoredMessage>& messages) {
    std::unordered_map<std::string, int> counts;
    for (const auto& msg : messages) {
        if (msg.role != "user") continue;
        for (const auto& term : QueryTerms(msg.content)) {
            ++counts[term];
        }
    }
    std::vector<std::pair<std::string, int>> ranked(counts.begin(), counts.end());
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
    });
    std::ostringstream out;
    int added = 0;
    for (const auto& [term, count] : ranked) {
        (void)count;
        if (added >= 8) break;
        if (added > 0) out << ", ";
        out << term;
        ++added;
    }
    return out.str();
}

std::string BuildHeuristicSummary(const std::vector<StoredMessage>& messages) {
    if (messages.empty()) return {};

    std::vector<std::string> user_details;
    std::vector<std::string> open_questions;
    std::vector<std::string> assistant_moves;
    for (const auto& msg : messages) {
        if (msg.role == "user") {
            if (LooksQuestionLike(msg.content) && open_questions.size() < 3) {
                open_questions.push_back(SummarySnippet(msg, 180));
            } else if (WordCountText(msg.content) >= 5 && user_details.size() < 4) {
                user_details.push_back(SummarySnippet(msg, 190));
            }
        } else if (msg.role == "assistant" && assistant_moves.size() < 2) {
            assistant_moves.push_back(SummarySnippet(msg, 180));
        }
    }

    std::ostringstream summary;
    summary << "Session window";
    if (!messages.front().created_at.empty() || !messages.back().created_at.empty()) {
        summary << " " << messages.front().created_at << " to " << messages.back().created_at;
    }
    summary << " (" << messages.size() << " messages).";

    const std::string terms = JoinTopTerms(messages);
    if (!terms.empty()) summary << " Themes: " << terms << ".";

    if (!user_details.empty()) {
        summary << " User details:";
        for (const auto& detail : user_details) summary << " \"" << detail << "\"";
    }
    if (!open_questions.empty()) {
        summary << " Open questions:";
        for (const auto& question : open_questions) summary << " \"" << question << "\"";
    }
    if (!assistant_moves.empty()) {
        summary << " Assistant moves:";
        for (const auto& move : assistant_moves) summary << " \"" << move << "\"";
    }
    return ClipText(summary.str(), 1400);
}

std::string JoinTermsForFts(const std::vector<std::string>& terms) {
    std::ostringstream query;
    bool first = true;
    for (const auto& term : terms) {
        if (!first) query << " OR ";
        first = false;
        query << term << "*";
    }
    return query.str();
}

void AddUnique(std::vector<std::string>& terms, const std::string& term) {
    if (std::find(terms.begin(), terms.end(), term) == terms.end()) terms.push_back(term);
}

std::string StripUserPrefix(std::string key) {
    const size_t colon = key.find(':');
    if (colon != std::string::npos && colon + 1 < key.size()) {
        key = key.substr(colon + 1);
    }
    return key;
}

struct RankedMemory {
    double score = 0.0;
    int order = 0;
    ChatMessage message;
};

void AddRankedMemory(std::vector<RankedMemory>& out,
                     double score,
                     int order,
                     std::string role,
                     std::string content,
                     std::string created_at) {
    if (score <= 0.0 || content.empty()) return;
    if (content.size() > 1200) {
        content = content.substr(0, 1197) + "...";
    }
    const auto duplicate = std::find_if(out.begin(), out.end(), [&](const RankedMemory& existing) {
        return existing.message.role == role && existing.message.content == content;
    });
    if (duplicate != out.end()) {
        duplicate->score = std::max(duplicate->score, score);
        duplicate->order = std::min(duplicate->order, order);
        return;
    }
    out.push_back({score, order, {std::move(role), std::move(content), std::move(created_at)}});
}

std::vector<std::string> ExpandSourceTerms(const std::string& query, std::vector<std::string> terms) {
    const std::string lower = Lower(query);
    if (ContainsAny(lower, {"reason for life", "meaning of life", "purpose of life", "my purpose", "life purpose"})) {
        AddUnique(terms, "purpose");
        AddUnique(terms, "jesus");
        AddUnique(terms, "serve");
        AddUnique(terms, "peace");
    }
    if (ContainsAny(lower, {"heaven", "eternal life", "afterlife"})) {
        AddUnique(terms, "heaven");
        AddUnique(terms, "eternal");
        AddUnique(terms, "kingdom");
    }
    if (ContainsAny(lower, {"anxiety", "stress", "anger", "afraid", "fear", "calm down"})) {
        AddUnique(terms, "peace");
        AddUnique(terms, "calm");
        AddUnique(terms, "emotion");
    }
    if (ContainsAny(lower, {"forgive", "forgiveness", "resentment"})) {
        AddUnique(terms, "forgive");
        AddUnique(terms, "repair");
        AddUnique(terms, "boundary");
    }
    if (ContainsAny(lower, {"pray", "prayer", "spiritually dry", "far from god", "listen to jesus", "relationship with jesus"})) {
        AddUnique(terms, "jesus");
        AddUnique(terms, "prayer");
        AddUnique(terms, "presence");
        AddUnique(terms, "spirit");
    }
    if (ContainsAny(lower, {"humble", "humility", "thankful", "gratitude", "complain", "complaining"})) {
        AddUnique(terms, "humble");
        AddUnique(terms, "thank");
        AddUnique(terms, "praise");
        AddUnique(terms, "pray");
    }
    if (ContainsAny(lower, {"quick to listen", "answer softly", "speak when", "angry"})) {
        AddUnique(terms, "listen");
        AddUnique(terms, "speak");
        AddUnique(terms, "anger");
        AddUnique(terms, "peace");
    }
    if (ContainsAny(lower, {"surrender", "control", "trust god", "perfect faith"})) {
        AddUnique(terms, "trust");
        AddUnique(terms, "faith");
        AddUnique(terms, "god");
        AddUnique(terms, "surrender");
    }
    if (ContainsAny(lower, {"worthless", "disrespected", "disrespect", "family anger"})) {
        AddUnique(terms, "love");
        AddUnique(terms, "peace");
        AddUnique(terms, "emotion");
        AddUnique(terms, "gentle");
    }
    return terms;
}

std::string TagsFor(const std::string& text) {
    const std::string lower = Lower(text);
    std::vector<std::string> tags;
    auto add = [&](const std::string& tag) {
        if (std::find(tags.begin(), tags.end(), tag) == tags.end()) tags.push_back(tag);
    };
    if (ContainsAny(lower, {"jesus", "christ", "god", "holy spirit", "prayer", "pray"})) add("christian");
    if (ContainsAny(lower, {"heaven", "eternal", "treasure", "kingdom"})) add("heaven");
    if (ContainsAny(lower, {"purpose", "meaning", "reason for life", "serve", "help others"})) add("purpose");
    if (ContainsAny(lower, {"peace", "calm", "stillness", "contentment", "destress"})) add("peace");
    if (ContainsAny(lower, {"anger", "hate", "frustrat", "resent"})) add("anger");
    if (ContainsAny(lower, {"anxiety", "stress", "fear", "panic"})) add("anxiety");
    if (ContainsAny(lower, {"forgive", "forgiveness", "repair", "repent"})) add("forgiveness");
    if (ContainsAny(lower, {"boundary", "boundaries", "safe", "safety"})) add("boundaries");
    if (ContainsAny(lower, {"grief", "loss", "tears", "sorrow"})) add("grief");
    if (ContainsAny(lower, {"journal", "meditat", "mindful", "emotion"})) add("emotional_processing");
    if (tags.empty()) add("general_wisdom");

    std::ostringstream out;
    for (size_t i = 0; i < tags.size(); ++i) {
        if (i > 0) out << ",";
        out << tags[i];
    }
    return out.str();
}

std::string IntensityFor(const std::string& text) {
    const std::string lower = Lower(text);
    return ContainsAny(lower, {
        "demon", "demonic", "spiritual warfare", "narcissist", "jezebel",
        "kingdom of darkness", "enemy obsession", "persecution"
    }) ? "high" : "normal";
}

std::string SafetyClassFor(const std::string& text) {
    const std::string lower = Lower(text);
    return ContainsAny(lower, {
        "suicide", "self-harm", "kill myself", "end my life", "violence",
        "immediate danger"
    }) ? "crisis" : "support";
}

bool LooksLikeTitle(const std::string& line) {
    if (line.size() > 140) return false;
    const std::string lower = Lower(line);
    return line.find('?') != std::string::npos ||
           ContainsAny(lower, {"trajectory", "purpose", "heaven", "guide", "prayer", "hope", "peace"});
}

}  // namespace

MemoryStore::~MemoryStore() {
    Close();
}

bool MemoryStore::Open(const std::filesystem::path& db_path) {
    Close();
    const auto utf8_path = db_path.u8string();
    const std::string path(reinterpret_cast<const char*>(utf8_path.c_str()), utf8_path.size());
    return sqlite3_open(path.c_str(), &db_) == SQLITE_OK && EnsureSchema();
}

void MemoryStore::Close() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool MemoryStore::EnsureSchema() {
    if (!db_) return false;
    if (!TableHasColumn(db_, "counseling_sources", "source_order")) {
        Exec(db_, "DROP TABLE IF EXISTS counseling_sources_fts;");
        Exec(db_, "DROP TABLE IF EXISTS counseling_sources;");
    }
    return Exec(db_, "PRAGMA journal_mode=WAL;") &&
           Exec(db_, "PRAGMA busy_timeout=5000;") &&
           Exec(db_, "CREATE TABLE IF NOT EXISTS native_messages ("
                     "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                     "session_id TEXT NOT NULL,"
                     "role TEXT NOT NULL,"
                     "content TEXT NOT NULL,"
                     "summarized INTEGER NOT NULL DEFAULT 0,"
                     "created_at TEXT NOT NULL DEFAULT (datetime('now')));") &&
           Exec(db_, "CREATE INDEX IF NOT EXISTS idx_native_messages_session_time "
                     "ON native_messages(session_id, created_at);") &&
           AddColumnIfMissing(db_, "native_messages", "summarized", "summarized INTEGER NOT NULL DEFAULT 0") &&
           AddColumnIfMissing(db_, "native_messages", "user", "user TEXT") &&
           Exec(db_, "CREATE INDEX IF NOT EXISTS idx_native_messages_session_summarized "
                     "ON native_messages(session_id, summarized, id);") &&
           Exec(db_, "CREATE INDEX IF NOT EXISTS idx_native_messages_user_time "
                     "ON native_messages(user, created_at);") &&
           Exec(db_, "CREATE TABLE IF NOT EXISTS native_emotion_signals ("
                     "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                     "session_id TEXT NOT NULL,"
                     "label TEXT NOT NULL,"
                     "intensity INTEGER NOT NULL,"
                     "source_text TEXT NOT NULL,"
                     "created_at TEXT NOT NULL DEFAULT (datetime('now')));") &&
           Exec(db_, "CREATE INDEX IF NOT EXISTS idx_native_emotions_time "
                     "ON native_emotion_signals(created_at);") &&
           AddColumnIfMissing(db_, "native_emotion_signals", "user", "user TEXT") &&
           Exec(db_, "CREATE INDEX IF NOT EXISTS idx_native_emotions_user_time "
                     "ON native_emotion_signals(user, created_at);") &&
           Exec(db_, "CREATE TABLE IF NOT EXISTS emotional_checkins ("
                     "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                     "timestamp TEXT NOT NULL DEFAULT (datetime('now')),"
                     "session TEXT,"
                     "user TEXT,"
                     "emotion TEXT NOT NULL,"
                     "intensity INTEGER,"
                     "valence REAL,"
                     "body_location TEXT,"
                     "trigger TEXT,"
                     "note TEXT,"
                     "source_message_id INTEGER,"
                     "confidence REAL,"
                     "captured_by TEXT DEFAULT 'system');") &&
           Exec(db_, "CREATE INDEX IF NOT EXISTS idx_emotional_checkins_time "
                     "ON emotional_checkins(timestamp);") &&
           Exec(db_, "CREATE INDEX IF NOT EXISTS idx_emotional_checkins_session "
                     "ON emotional_checkins(session, timestamp);") &&
           Exec(db_, "CREATE TABLE IF NOT EXISTS native_context_items ("
                     "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                     "kind TEXT NOT NULL,"
                     "item_key TEXT NOT NULL,"
                     "content TEXT NOT NULL,"
                     "source TEXT NOT NULL,"
                     "updated_at TEXT NOT NULL DEFAULT (datetime('now')),"
                     "UNIQUE(kind, item_key, source));") &&
           Exec(db_, "CREATE INDEX IF NOT EXISTS idx_native_context_kind_time "
                     "ON native_context_items(kind, updated_at);") &&
           Exec(db_, "CREATE TABLE IF NOT EXISTS counseling_sources ("
                     "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                     "source_path TEXT NOT NULL,"
                     "source_order INTEGER NOT NULL,"
                     "title TEXT NOT NULL,"
                     "content TEXT NOT NULL,"
                     "tags TEXT NOT NULL,"
                     "intensity TEXT NOT NULL,"
                     "safety_class TEXT NOT NULL,"
                     "preference TEXT NOT NULL,"
                     "updated_at TEXT NOT NULL DEFAULT (datetime('now')));") &&
           Exec(db_, "CREATE INDEX IF NOT EXISTS idx_counseling_sources_order "
                     "ON counseling_sources(source_order);") &&
           Exec(db_, "CREATE VIRTUAL TABLE IF NOT EXISTS counseling_sources_fts "
                     "USING fts5(title, content, tags);") &&
           Exec(db_, "CREATE TABLE IF NOT EXISTS summaries ("
                     "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                     "timestamp TEXT NOT NULL DEFAULT (datetime('now')),"
                     "session TEXT,"
                     "content TEXT NOT NULL,"
                     "msg_count INTEGER DEFAULT 0,"
                     "embedding BLOB,"
                     "user TEXT);") &&
           Exec(db_, "CREATE INDEX IF NOT EXISTS idx_sum_session ON summaries(session);") &&
           Exec(db_, "CREATE INDEX IF NOT EXISTS idx_sum_user ON summaries(user);") &&
           Exec(db_, "CREATE TABLE IF NOT EXISTS user_profile ("
                     "key TEXT PRIMARY KEY,"
                     "value TEXT,"
                     "updated_at TEXT NOT NULL DEFAULT (datetime('now')));") &&
           Exec(db_, "CREATE TABLE IF NOT EXISTS techniques ("
                     "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                     "timestamp TEXT NOT NULL DEFAULT (datetime('now')),"
                     "session TEXT,"
                     "user TEXT,"
                     "query TEXT NOT NULL,"
                     "move TEXT NOT NULL,"
                     "evidence TEXT,"
                     "embedding BLOB,"
                     "source TEXT DEFAULT 'local',"
                     "shared_at TEXT);") &&
           Exec(db_, "CREATE INDEX IF NOT EXISTS idx_tech_user ON techniques(user);") &&
           Exec(db_, "CREATE INDEX IF NOT EXISTS idx_tech_source ON techniques(source);") &&
           Exec(db_, "CREATE TABLE IF NOT EXISTS knowledge_gaps ("
                     "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                     "timestamp TEXT NOT NULL DEFAULT (datetime('now')),"
                     "session TEXT,"
                     "user TEXT,"
                     "topic TEXT NOT NULL,"
                     "description TEXT NOT NULL,"
                     "status TEXT NOT NULL DEFAULT 'open',"
                     "updated_at TEXT NOT NULL DEFAULT (datetime('now')));") &&
           AddColumnIfMissing(db_, "summaries", "embedding", "embedding BLOB") &&
           AddColumnIfMissing(db_, "summaries", "user", "user TEXT") &&
           AddColumnIfMissing(db_, "techniques", "timestamp", "timestamp TEXT") &&
           AddColumnIfMissing(db_, "techniques", "session", "session TEXT") &&
           AddColumnIfMissing(db_, "techniques", "user", "user TEXT") &&
           AddColumnIfMissing(db_, "techniques", "query", "query TEXT NOT NULL DEFAULT ''") &&
           AddColumnIfMissing(db_, "techniques", "move", "move TEXT NOT NULL DEFAULT ''") &&
           AddColumnIfMissing(db_, "techniques", "evidence", "evidence TEXT") &&
           AddColumnIfMissing(db_, "techniques", "embedding", "embedding BLOB") &&
           AddColumnIfMissing(db_, "techniques", "source", "source TEXT DEFAULT 'local'") &&
           AddColumnIfMissing(db_, "techniques", "shared_at", "shared_at TEXT") &&
           AddColumnIfMissing(db_, "knowledge_gaps", "user", "user TEXT") &&
           AddColumnIfMissing(db_, "knowledge_gaps", "description", "description TEXT NOT NULL DEFAULT ''") &&
           AddColumnIfMissing(db_, "knowledge_gaps", "status", "status TEXT NOT NULL DEFAULT 'open'") &&
           AddColumnIfMissing(db_, "knowledge_gaps", "updated_at", "updated_at TEXT") &&
           AddColumnIfMissing(db_, "emotional_checkins", "timestamp", "timestamp TEXT") &&
           AddColumnIfMissing(db_, "emotional_checkins", "session", "session TEXT") &&
           AddColumnIfMissing(db_, "emotional_checkins", "user", "user TEXT") &&
           AddColumnIfMissing(db_, "emotional_checkins", "emotion", "emotion TEXT NOT NULL DEFAULT ''") &&
           AddColumnIfMissing(db_, "emotional_checkins", "intensity", "intensity INTEGER") &&
           AddColumnIfMissing(db_, "emotional_checkins", "valence", "valence REAL") &&
           AddColumnIfMissing(db_, "emotional_checkins", "body_location", "body_location TEXT") &&
           AddColumnIfMissing(db_, "emotional_checkins", "trigger", "trigger TEXT") &&
           AddColumnIfMissing(db_, "emotional_checkins", "note", "note TEXT") &&
           AddColumnIfMissing(db_, "emotional_checkins", "source_message_id", "source_message_id INTEGER") &&
           AddColumnIfMissing(db_, "emotional_checkins", "confidence", "confidence REAL") &&
           AddColumnIfMissing(db_, "emotional_checkins", "captured_by", "captured_by TEXT DEFAULT 'system'") &&
           Exec(db_, "CREATE INDEX IF NOT EXISTS idx_emotional_checkins_user_time "
                     "ON emotional_checkins(user, timestamp);") &&
           Exec(db_, "CREATE INDEX IF NOT EXISTS idx_emotional_checkins_emotion "
                     "ON emotional_checkins(emotion, timestamp);") &&
           Exec(db_, "CREATE INDEX IF NOT EXISTS idx_knowledge_gaps_session_status "
                     "ON knowledge_gaps(session, status, updated_at);");
}

bool MemoryStore::SaveMessage(const std::string& session_id,
                              const std::string& user,
                              const std::string& role,
                              const std::string& content) {
    if (!db_) return false;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO native_messages(session_id, user, role, content) VALUES(?, ?, ?, ?);";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    BindText(stmt, 1, session_id);
    BindText(stmt, 2, user.empty() ? "aaron" : user);
    BindText(stmt, 3, role);
    BindText(stmt, 4, content);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool MemoryStore::SaveEmotion(const std::string& session_id,
                              const std::string& user,
                              const std::string& source_text,
                              const EmotionSignal& signal) {
    if (!db_ || signal.label.empty() || signal.intensity <= 0) return false;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO native_emotion_signals(session_id, user, label, intensity, source_text) VALUES(?, ?, ?, ?, ?);";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    BindText(stmt, 1, session_id);
    BindText(stmt, 2, user.empty() ? "aaron" : user);
    BindText(stmt, 3, signal.label);
    sqlite3_bind_int(stmt, 4, signal.intensity);
    BindText(stmt, 5, source_text);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    if (ok) {
        EmotionCheckin checkin;
        checkin.session = session_id;
        checkin.user = user.empty() ? "aaron" : user;
        checkin.emotion = signal.label;
        checkin.intensity = signal.intensity;
        checkin.note = source_text;
        checkin.confidence = 0.65;
        checkin.captured_by = "detector";
        SaveEmotionCheckin(checkin);
    }
    return ok;
}

bool MemoryStore::SaveEmotionCheckin(const EmotionCheckin& checkin) {
    if (!db_ || checkin.emotion.empty()) return false;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO emotional_checkins(timestamp, session, user, emotion, intensity, valence, body_location, trigger, note, source_message_id, confidence, captured_by) "
        "VALUES(COALESCE(NULLIF(?, ''), datetime('now')), ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    BindText(stmt, 1, checkin.timestamp);
    BindText(stmt, 2, checkin.session);
    BindText(stmt, 3, checkin.user.empty() ? "aaron" : checkin.user);
    BindText(stmt, 4, checkin.emotion);
    BindOptionalInt(stmt, 5, checkin.intensity);
    BindOptionalDouble(stmt, 6, checkin.valence);
    BindText(stmt, 7, checkin.body_location);
    BindText(stmt, 8, checkin.trigger);
    BindText(stmt, 9, checkin.note);
    BindOptionalInt64(stmt, 10, checkin.source_message_id);
    BindOptionalDouble(stmt, 11, checkin.confidence);
    BindText(stmt, 12, checkin.captured_by.empty() ? "manual" : checkin.captured_by);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool MemoryStore::SaveTechnique(const std::string& session_id,
                                const std::string& query,
                                const std::string& move,
                                const std::string& evidence) {
    if (!db_ || move.empty()) return false;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO techniques(timestamp, session, user, query, move, evidence, source) "
        "VALUES(datetime('now'), ?, ?, ?, ?, ?, ?);";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    BindText(stmt, 1, session_id);
    BindText(stmt, 2, "aaron");
    BindText(stmt, 3, query.empty() ? move : query);
    BindText(stmt, 4, move);
    BindText(stmt, 5, evidence);
    BindText(stmt, 6, "native");
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

std::vector<TechniqueItem> MemoryStore::ListTechniques(int limit) const {
    std::vector<TechniqueItem> out;
    if (!db_ || limit <= 0) return out;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT id, query, move, evidence, source FROM techniques "
        "WHERE move IS NOT NULL AND move!='' "
        "ORDER BY id DESC LIMIT ?;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_int(stmt, 1, limit);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        out.push_back({
            sqlite3_column_int(stmt, 0),
            ColumnText(stmt, 1),
            ColumnText(stmt, 2),
            ColumnText(stmt, 3),
            ColumnText(stmt, 4),
        });
    }
    sqlite3_finalize(stmt);
    return out;
}

bool MemoryStore::DeleteTechnique(int id) {
    if (!db_ || id <= 0) return false;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM techniques WHERE id=?;", -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int(stmt, 1, id);
    sqlite3_step(stmt);
    const bool deleted = sqlite3_changes(db_) > 0;
    sqlite3_finalize(stmt);
    return deleted;
}

int MemoryStore::SummarizeSessionIfNeeded(const std::string& session_id, int threshold) {
    if (!db_ || session_id.empty()) return 0;
    threshold = std::max(8, threshold);

    std::vector<StoredMessage> unsummarized;
    sqlite3_stmt* stmt = nullptr;
    const char* select_sql =
        "SELECT id, role, content, created_at FROM native_messages "
        "WHERE session_id=? AND summarized=0 ORDER BY id ASC;";
    if (sqlite3_prepare_v2(db_, select_sql, -1, &stmt, nullptr) != SQLITE_OK) return 0;
    BindText(stmt, 1, session_id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        unsummarized.push_back({
            sqlite3_column_int64(stmt, 0),
            ColumnText(stmt, 1),
            ColumnText(stmt, 2),
            ColumnText(stmt, 3),
        });
    }
    sqlite3_finalize(stmt);

    if (static_cast<int>(unsummarized.size()) < threshold || unsummarized.size() <= 6) return 0;
    const size_t summarize_count = std::min<size_t>(24, unsummarized.size() - 6);
    if (summarize_count == 0) return 0;

    std::vector<StoredMessage> window(unsummarized.begin(), unsummarized.begin() + summarize_count);
    const std::string summary = BuildHeuristicSummary(window);
    if (summary.empty()) return 0;

    Exec(db_, "BEGIN;");
    sqlite3_stmt* insert = nullptr;
    const char* insert_sql = "INSERT INTO summaries(session, content, msg_count, user) VALUES(?, ?, ?, 'native');";
    if (sqlite3_prepare_v2(db_, insert_sql, -1, &insert, nullptr) != SQLITE_OK) {
        Exec(db_, "ROLLBACK;");
        return 0;
    }
    BindText(insert, 1, session_id);
    BindText(insert, 2, summary);
    sqlite3_bind_int(insert, 3, static_cast<int>(window.size()));
    const bool inserted = sqlite3_step(insert) == SQLITE_DONE;
    sqlite3_finalize(insert);
    if (!inserted) {
        Exec(db_, "ROLLBACK;");
        return 0;
    }

    sqlite3_stmt* update = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE native_messages SET summarized=1 WHERE id=?;", -1, &update, nullptr) != SQLITE_OK) {
        Exec(db_, "ROLLBACK;");
        return 0;
    }
    for (const auto& msg : window) {
        sqlite3_reset(update);
        sqlite3_clear_bindings(update);
        sqlite3_bind_int64(update, 1, msg.id);
        sqlite3_step(update);
    }
    sqlite3_finalize(update);
    Exec(db_, "COMMIT;");
    return 1;
}

bool MemoryStore::RecordKnowledgeGap(const std::string& session_id,
                                     const std::string& topic,
                                     const std::string& description) {
    if (!db_) return false;
    const std::string clean_topic = ClipText(topic, 160);
    const std::string clean_description = ClipText(description.empty() ? topic : description, 600);
    if (clean_topic.empty() || clean_description.empty()) return false;

    sqlite3_stmt* existing = nullptr;
    int existing_id = 0;
    const char* select_sql =
        "SELECT id FROM knowledge_gaps WHERE session=? AND topic=? AND status='open' "
        "ORDER BY id DESC LIMIT 1;";
    if (sqlite3_prepare_v2(db_, select_sql, -1, &existing, nullptr) == SQLITE_OK) {
        BindText(existing, 1, session_id);
        BindText(existing, 2, clean_topic);
        if (sqlite3_step(existing) == SQLITE_ROW) {
            existing_id = sqlite3_column_int(existing, 0);
        }
    }
    sqlite3_finalize(existing);

    sqlite3_stmt* stmt = nullptr;
    const char* sql = existing_id > 0
        ? "UPDATE knowledge_gaps SET description=?, updated_at=datetime('now') WHERE id=?;"
        : "INSERT INTO knowledge_gaps(session, user, topic, description, status) VALUES(?, 'native', ?, ?, 'open');";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    if (existing_id > 0) {
        BindText(stmt, 1, clean_description);
        sqlite3_bind_int(stmt, 2, existing_id);
    } else {
        BindText(stmt, 1, session_id);
        BindText(stmt, 2, clean_topic);
        BindText(stmt, 3, clean_description);
    }
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool MemoryStore::CaptureKnowledgeGap(const std::string& session_id,
                                      const std::string& user_message,
                                      const std::string& assistant_reply) {
    const bool explicit_gap = UserNamesKnowledgeGap(user_message);
    const bool unanswered_question = LooksQuestionLike(user_message) && AssistantSignalsKnowledgeGap(assistant_reply);
    if (!explicit_gap && !unanswered_question) return false;

    const std::string topic = FirstSentence(user_message, 160);
    std::string description = "User asked or flagged an unresolved item: " + ClipText(user_message, 260);
    if (!assistant_reply.empty()) {
        description += " Assistant response/context: " + ClipText(assistant_reply, 260);
    }
    return RecordKnowledgeGap(session_id, topic, description);
}

std::vector<KnowledgeGap> MemoryStore::ListKnowledgeGaps(int limit) const {
    std::vector<KnowledgeGap> out;
    if (!db_ || limit <= 0) return out;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT id, topic, description FROM knowledge_gaps "
        "WHERE status='open' ORDER BY updated_at DESC LIMIT ?;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_int(stmt, 1, limit);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        out.push_back({
            sqlite3_column_int(stmt, 0),
            ColumnText(stmt, 1),
            ColumnText(stmt, 2),
        });
    }
    sqlite3_finalize(stmt);
    return out;
}

bool MemoryStore::ImportCounselingSource(const std::filesystem::path& text_path) {
    if (!db_ || !std::filesystem::exists(text_path)) return false;

    const auto file_time = std::filesystem::last_write_time(text_path);
    const auto file_size = std::filesystem::file_size(text_path);
    const std::string source_path = text_path.string();
    std::string import_key = source_path + "|" + std::to_string(file_size) + "|" +
                             std::to_string(file_time.time_since_epoch().count());

    sqlite3_stmt* check = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM counseling_sources WHERE source_path=?;", -1, &check, nullptr) == SQLITE_OK) {
        BindText(check, 1, import_key);
        if (sqlite3_step(check) == SQLITE_ROW && sqlite3_column_int(check, 0) > 0) {
            sqlite3_finalize(check);
            return true;
        }
    }
    sqlite3_finalize(check);

    std::ifstream input(text_path, std::ios::binary);
    if (!input) return false;

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) {
        line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());
        if (!line.empty()) lines.push_back(line);
    }
    if (lines.empty()) return false;

    Exec(db_, "BEGIN;");
    Exec(db_, "DELETE FROM counseling_sources;");
    Exec(db_, "DELETE FROM counseling_sources_fts;");

    sqlite3_stmt* insert_source = nullptr;
    sqlite3_stmt* insert_fts = nullptr;
    const char* source_sql =
        "INSERT INTO counseling_sources(source_path, source_order, title, content, tags, intensity, safety_class, preference) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?);";
    const char* fts_sql =
        "INSERT INTO counseling_sources_fts(rowid, title, content, tags) VALUES(?, ?, ?, ?);";
    if (sqlite3_prepare_v2(db_, source_sql, -1, &insert_source, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(db_, fts_sql, -1, &insert_fts, nullptr) != SQLITE_OK) {
        sqlite3_finalize(insert_source);
        sqlite3_finalize(insert_fts);
        Exec(db_, "ROLLBACK;");
        return false;
    }

    std::string title = "MasterDocument";
    std::string chunk;
    int order = 0;
    int imported = 0;
    auto flush = [&]() {
        if (chunk.empty()) return;
        const std::string tags = TagsFor(title + " " + chunk);
        const std::string intensity = IntensityFor(chunk);
        const std::string safety = SafetyClassFor(chunk);
        const std::string preference = intensity == "normal" && safety == "support" ? "gentle_practical" : "restricted";

        sqlite3_reset(insert_source);
        sqlite3_clear_bindings(insert_source);
        BindText(insert_source, 1, import_key);
        sqlite3_bind_int(insert_source, 2, order++);
        BindText(insert_source, 3, title);
        BindText(insert_source, 4, chunk);
        BindText(insert_source, 5, tags);
        BindText(insert_source, 6, intensity);
        BindText(insert_source, 7, safety);
        BindText(insert_source, 8, preference);
        if (sqlite3_step(insert_source) != SQLITE_DONE) return;
        const sqlite3_int64 rowid = sqlite3_last_insert_rowid(db_);

        sqlite3_reset(insert_fts);
        sqlite3_clear_bindings(insert_fts);
        sqlite3_bind_int64(insert_fts, 1, rowid);
        BindText(insert_fts, 2, title);
        BindText(insert_fts, 3, chunk);
        BindText(insert_fts, 4, tags);
        sqlite3_step(insert_fts);
        ++imported;
        chunk.clear();
    };

    for (const auto& raw : lines) {
        if (LooksLikeTitle(raw)) {
            flush();
            title = raw.substr(0, 180);
            continue;
        }
        if (!chunk.empty() && chunk.size() + raw.size() > 1100) {
            flush();
        }
        if (!chunk.empty()) chunk += "\n";
        chunk += raw;
    }
    flush();

    sqlite3_finalize(insert_source);
    sqlite3_finalize(insert_fts);
    Exec(db_, "COMMIT;");
    return imported > 0;
}

bool MemoryStore::ImportLegacyContext(const std::filesystem::path& legacy_db_path) {
    if (!db_ || legacy_db_path.empty() || !std::filesystem::exists(legacy_db_path)) return false;
    const std::string source = legacy_db_path.string();

    sqlite3_stmt* attach = nullptr;
    if (sqlite3_prepare_v2(db_, "ATTACH DATABASE ? AS legacy;", -1, &attach, nullptr) != SQLITE_OK) return false;
    BindText(attach, 1, source);
    const bool attached = sqlite3_step(attach) == SQLITE_DONE;
    sqlite3_finalize(attach);
    if (!attached) return false;

    bool ok = true;
    ok = Exec(db_, "DELETE FROM native_context_items WHERE source='legacy:v14';") && ok;
    ok = Exec(db_,
        "INSERT OR IGNORE INTO native_context_items(kind, item_key, content, source, updated_at) "
        "SELECT 'profile', key, key || ': ' || value, 'legacy:v14', COALESCE(updated_at, datetime('now')) "
        "FROM legacy.user_profile "
        "WHERE key NOT LIKE '%__active_session%' "
        "AND key NOT LIKE '%__loc_%' "
        "AND key NOT IN ('aaron:name') "
        "AND value IS NOT NULL AND length(value)>0;") && ok;
    ok = Exec(db_,
        "INSERT OR IGNORE INTO native_context_items(kind, item_key, content, source, updated_at) "
        "SELECT 'summary', printf('%s:%d', COALESCE(session,''), id), content, 'legacy:v14', COALESCE(timestamp, datetime('now')) "
        "FROM legacy.summaries "
        "WHERE content IS NOT NULL AND length(content)>0 "
        "ORDER BY id DESC LIMIT 80;") && ok;
    ok = Exec(db_,
        "INSERT OR IGNORE INTO native_context_items(kind, item_key, content, source, updated_at) "
        "SELECT 'technique', printf('%s:%d', COALESCE(user,'aaron'), id), "
        "'When something like this comes up, useful move: ' || move, "
        "'legacy:v14', COALESCE(timestamp, datetime('now')) "
        "FROM legacy.techniques "
        "WHERE move IS NOT NULL AND length(move)>0;") && ok;
    ok = Exec(db_,
        "INSERT OR IGNORE INTO native_context_items(kind, item_key, content, source, updated_at) "
        "SELECT 'position', printf('%s:%d', COALESCE(topic,''), id), "
        "'Previous position on ' || topic || ': ' || position, "
        "'legacy:v14', COALESCE(timestamp, datetime('now')) "
        "FROM legacy.user_positions "
        "WHERE topic IS NOT NULL AND position IS NOT NULL AND length(position)>0 "
        "ORDER BY id DESC LIMIT 120;") && ok;

    Exec(db_, "DETACH DATABASE legacy;");
    return ok;
}

int MemoryStore::ImportSharedTechniques(const std::filesystem::path& path) {
    if (!db_ || path.empty() || !std::filesystem::exists(path)) return 0;
    std::error_code ec;
    if (std::filesystem::file_size(path, ec) > 10 * 1024 * 1024) return 0;
    std::ifstream input(path, std::ios::binary);
    if (!input) return 0;
    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string text = buffer.str();

    int imported = 0;
    size_t pos = 0;
    while ((pos = text.find("## ", pos)) != std::string::npos) {
        const size_t next = text.find("\n## ", pos + 3);
        const std::string block = text.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
        pos = next == std::string::npos ? text.size() : next + 1;

        const size_t header_end = block.find('\n');
        const std::string header = header_end == std::string::npos ? block : block.substr(0, header_end);
        std::string user = "aaron";
        size_t sep = header.find(" - ");
        if (sep == std::string::npos) sep = header.find("\xC2\xB7");
        if (sep != std::string::npos) {
            const size_t hash_pos = header.find("hash:", sep);
            const size_t user_start = header.compare(sep, 3, " - ") == 0 ? sep + 3 : sep + 2;
            user = TrimWhitespace(header.substr(user_start, hash_pos == std::string::npos ? std::string::npos : hash_pos - user_start));
            if (user.empty()) user = "aaron";
        }
        auto field = [&](const std::string& label) {
            const std::string marker = "**" + label + ":**";
            const size_t start = block.find(marker);
            if (start == std::string::npos) return std::string{};
            const size_t value_start = start + marker.size();
            const size_t next_field = block.find("\n**", value_start);
            const size_t separator = block.find("\n---", value_start);
            const size_t end = std::min(next_field == std::string::npos ? block.size() : next_field,
                                        separator == std::string::npos ? block.size() : separator);
            return TrimWhitespace(block.substr(value_start, end - value_start));
        };
        std::string query = Truncate(field("query"), 1000);
        std::string move = Truncate(field("move"), 500);
        std::string evidence = Truncate(field("evidence"), 1500);
        user = Truncate(user, 32);
        if (move.empty() || ContainsInjectionMarker(user + "\n" + query + "\n" + move + "\n" + evidence)) continue;
        const std::string hash = TechniqueHash(user, query, move);

        bool duplicate = false;
        sqlite3_stmt* exists = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT user, query, move FROM techniques ORDER BY id DESC LIMIT 10000;", -1, &exists, nullptr) == SQLITE_OK) {
            while (sqlite3_step(exists) == SQLITE_ROW) {
                if (TechniqueHash(ColumnText(exists, 0), ColumnText(exists, 1), ColumnText(exists, 2)) == hash) {
                    duplicate = true;
                    break;
                }
            }
        }
        sqlite3_finalize(exists);
        if (duplicate) continue;

        sqlite3_stmt* insert = nullptr;
        const char* sql =
            "INSERT INTO techniques(timestamp, session, user, query, move, evidence, source, shared_at) "
            "VALUES(datetime('now'), '', ?, ?, ?, ?, 'shared', datetime('now'));";
        if (sqlite3_prepare_v2(db_, sql, -1, &insert, nullptr) != SQLITE_OK) continue;
        BindText(insert, 1, user);
        BindText(insert, 2, query);
        BindText(insert, 3, move);
        BindText(insert, 4, evidence);
        if (sqlite3_step(insert) == SQLITE_DONE) ++imported;
        sqlite3_finalize(insert);
    }
    return imported;
}

int MemoryStore::ExportSharedTechniques(const std::filesystem::path& path) const {
    if (!db_ || path.empty()) return 0;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    std::string existing_text;
    {
        std::ifstream input(path, std::ios::binary);
        if (input) {
            std::ostringstream buffer;
            buffer << input.rdbuf();
            existing_text = buffer.str();
        }
    }
    std::ofstream output(path, std::ios::app | std::ios::binary);
    if (!output) return 0;
    if (existing_text.empty()) output << "# Symbion shared learnings\n\n";

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT timestamp, user, query, move, evidence FROM techniques "
        "WHERE COALESCE(source,'local')='local' ORDER BY id ASC LIMIT 10000;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0;
    int exported = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const std::string ts = ColumnText(stmt, 0);
        const std::string user = ColumnText(stmt, 1).empty() ? "aaron" : ColumnText(stmt, 1);
        const std::string query = ColumnText(stmt, 2);
        const std::string move = ColumnText(stmt, 3);
        const std::string evidence = ColumnText(stmt, 4);
        if (move.empty() || ContainsInjectionMarker(query + "\n" + move + "\n" + evidence)) continue;
        const std::string hash = TechniqueHash(user, query, move);
        if (existing_text.find("hash:" + hash) != std::string::npos) continue;
        output << "\n## " << (ts.empty() ? "native" : ts) << " - " << user << " - hash:" << hash << "\n"
               << "**query:** " << Truncate(query, 1000) << "\n\n"
               << "**move:** " << Truncate(move, 500) << "\n";
        if (!evidence.empty()) output << "\n**evidence:** " << Truncate(evidence, 1500) << "\n";
        output << "\n---\n";
        ++exported;
    }
    sqlite3_finalize(stmt);
    return exported;
}

std::vector<ChatMessage> MemoryStore::RecentMessages(const std::string& session_id, int limit) const {
    std::vector<ChatMessage> out;
    if (!db_) return out;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT role, content, created_at FROM native_messages "
        "WHERE session_id=? ORDER BY id DESC LIMIT ?;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
    BindText(stmt, 1, session_id);
    sqlite3_bind_int(stmt, 2, limit);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        out.push_back({
            ColumnText(stmt, 0),
            ColumnText(stmt, 1),
            ColumnText(stmt, 2),
        });
    }
    sqlite3_finalize(stmt);
    std::reverse(out.begin(), out.end());
    return out;
}

std::vector<ChatMessage> MemoryStore::RecentSessionSummaries(const std::string& session_id, int limit) const {
    std::vector<ChatMessage> out;
    if (!db_ || limit <= 0) return out;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT content, timestamp FROM summaries "
        "WHERE session=? AND content IS NOT NULL AND content!='' "
        "ORDER BY id DESC LIMIT ?;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
    BindText(stmt, 1, session_id);
    sqlite3_bind_int(stmt, 2, limit);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        out.push_back({
            "summary",
            "Recent session summary: " + ColumnText(stmt, 0),
            ColumnText(stmt, 1),
        });
    }
    sqlite3_finalize(stmt);
    std::reverse(out.begin(), out.end());
    return out;
}

std::vector<SessionInfo> MemoryStore::ListSessions(const std::string& user, int limit) const {
    std::vector<SessionInfo> out;
    if (!db_ || limit <= 0) return out;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT session_id, MAX(created_at) AS last_activity, COUNT(*) AS turns, "
        "(SELECT content FROM native_messages m2 WHERE m2.session_id=m.session_id AND role='user' ORDER BY id ASC LIMIT 1) AS title "
        "FROM native_messages m WHERE COALESCE(user,'aaron')=? GROUP BY session_id ORDER BY last_activity DESC LIMIT ?;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
    BindText(stmt, 1, user.empty() ? "aaron" : user);
    sqlite3_bind_int(stmt, 2, limit);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::string title = ColumnText(stmt, 3);
        title = ClipText(title, 80);
        out.push_back({
            ColumnText(stmt, 0),
            title.empty() ? "New chat" : title,
            ColumnText(stmt, 1),
            sqlite3_column_int(stmt, 2),
        });
    }
    sqlite3_finalize(stmt);
    return out;
}

std::optional<std::string> MemoryStore::GetProfileFact(const std::string& user, const std::string& key) const {
    if (!db_ || key.empty()) return std::nullopt;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT value FROM user_profile WHERE key=? OR key=? ORDER BY key=? DESC LIMIT 1;", -1, &stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    const std::string effective_user = user.empty() ? "aaron" : user;
    BindText(stmt, 1, key);
    BindText(stmt, 2, effective_user + ":" + key);
    BindText(stmt, 3, effective_user + ":" + key);
    std::optional<std::string> value;
    if (sqlite3_step(stmt) == SQLITE_ROW) value = ColumnText(stmt, 0);
    sqlite3_finalize(stmt);
    return value;
}

std::vector<ChatMessage> MemoryStore::AmbientContext(const std::string& user, int limit) const {
    std::vector<ChatMessage> out;
    if (!db_ || limit <= 0) return out;
    sqlite3_stmt* stmt = nullptr;
    const std::string effective_user = user.empty() ? "aaron" : user;
    const char* sql =
        "SELECT kind, content, updated_at FROM native_context_items "
        "WHERE kind IN ('profile', 'technique') "
        "AND (item_key NOT LIKE '%:%' OR item_key LIKE ? || ':%') "
        "ORDER BY CASE kind WHEN 'profile' THEN 0 ELSE 1 END, updated_at DESC LIMIT ?;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
    BindText(stmt, 1, effective_user);
    sqlite3_bind_int(stmt, 2, limit);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        out.push_back({
            ColumnText(stmt, 0),
            ColumnText(stmt, 1),
            ColumnText(stmt, 2),
        });
    }
    sqlite3_finalize(stmt);
    return out;
}

std::vector<ChatMessage> MemoryStore::RetrieveRelevant(const std::string& user, const std::string& query, int limit) const {
    std::vector<ChatMessage> out;
    if (!db_) return out;
    const std::string effective_user = user.empty() ? "aaron" : user;
    const auto terms = QueryTerms(query);
    if (terms.empty()) return out;
    const int capped_limit = std::max(0, limit);
    if (capped_limit == 0) return out;

    std::vector<RankedMemory> ranked;

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT role, content, created_at FROM native_messages "
        "WHERE role='user' AND summarized=0 AND COALESCE(user,'aaron')=? ORDER BY id DESC LIMIT 300;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
    BindText(stmt, 1, effective_user);
    int order = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const std::string content = ColumnText(stmt, 1);
        const double score = TermScore(content, terms);
        AddRankedMemory(ranked, score > 0.0 ? score + 0.20 : 0.0, order++,
                        ColumnText(stmt, 0), content, ColumnText(stmt, 2));
    }
    sqlite3_finalize(stmt);

    stmt = nullptr;
    const char* ctx_sql =
        "SELECT kind, content, updated_at FROM native_context_items "
        "WHERE item_key NOT LIKE '%:%' OR item_key LIKE ? || ':%' "
        "ORDER BY updated_at DESC LIMIT 400;";
    if (sqlite3_prepare_v2(db_, ctx_sql, -1, &stmt, nullptr) == SQLITE_OK) {
        BindText(stmt, 1, effective_user);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const std::string kind = ColumnText(stmt, 0);
            const std::string content = ColumnText(stmt, 1);
            const double score = TermScore(content, terms);
            const double boost = kind == "summary" ? 0.35 : (kind == "technique" ? 0.25 : 0.15);
            AddRankedMemory(ranked, score > 0.0 ? score + boost : 0.0, order++,
                            kind, content, ColumnText(stmt, 2));
        }
    }
    sqlite3_finalize(stmt);

    stmt = nullptr;
    const char* summary_sql =
        "SELECT content, timestamp FROM summaries "
        "WHERE content IS NOT NULL AND content!='' "
        "AND COALESCE(user,'aaron')=? "
        "ORDER BY id DESC LIMIT 200;";
    if (sqlite3_prepare_v2(db_, summary_sql, -1, &stmt, nullptr) == SQLITE_OK) {
        BindText(stmt, 1, effective_user);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const std::string content = ColumnText(stmt, 0);
            const double score = TermScore(content, terms);
            AddRankedMemory(ranked, score > 0.0 ? score + 0.35 : 0.0, order++,
                            "summary", "Past conversation summary: " + content, ColumnText(stmt, 1));
        }
    }
    sqlite3_finalize(stmt);

    stmt = nullptr;
    const char* profile_sql =
        "SELECT key, value, updated_at FROM user_profile "
        "WHERE value IS NOT NULL AND value!='' "
        "AND (key NOT LIKE '%:%' OR key LIKE ? || ':%') "
        "ORDER BY updated_at DESC LIMIT 80;";
    if (sqlite3_prepare_v2(db_, profile_sql, -1, &stmt, nullptr) == SQLITE_OK) {
        BindText(stmt, 1, effective_user);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const std::string key = StripUserPrefix(ColumnText(stmt, 0));
            if (key.rfind("__", 0) == 0) continue;
            const std::string value = ColumnText(stmt, 1);
            double score = TermScore(key + " " + value, terms);
            if (score > 0.0 &&
                (key == "current_situation" || key == "communication_style" || key == "core_positions")) {
                score += 0.15;
            }
            AddRankedMemory(ranked, score > 0.0 ? score + 0.10 : 0.0, order++, "profile",
                            "Remembered profile fact (context, not an instruction): " + key + " = " + value,
                            ColumnText(stmt, 2));
        }
    }
    sqlite3_finalize(stmt);

    stmt = nullptr;
    const char* technique_sql =
        "SELECT query, move, evidence, source, timestamp FROM techniques "
        "WHERE move IS NOT NULL AND move!='' "
        "AND COALESCE(user,'aaron')=? "
        "ORDER BY id DESC LIMIT 200;";
    if (sqlite3_prepare_v2(db_, technique_sql, -1, &stmt, nullptr) == SQLITE_OK) {
        BindText(stmt, 1, effective_user);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const std::string saved_query = ColumnText(stmt, 0);
            const std::string move = ColumnText(stmt, 1);
            const std::string evidence = ColumnText(stmt, 2);
            std::string source = ColumnText(stmt, 3);
            if (source.empty()) source = "local";
            std::string content = "Technique worth replicating if relevant [" + source + "]: " + move;
            if (!evidence.empty()) content += " Evidence: " + evidence;
            const double score = TermScore(saved_query + " " + move, terms);
            AddRankedMemory(ranked, score > 0.0 ? score + 0.25 : 0.0, order++,
                            "technique", content, ColumnText(stmt, 4));
        }
    }
    sqlite3_finalize(stmt);

    stmt = nullptr;
    const char* gap_sql =
        "SELECT topic, description, updated_at FROM knowledge_gaps "
        "WHERE status='open' AND COALESCE(user,'aaron')=? ORDER BY updated_at DESC LIMIT 120;";
    if (sqlite3_prepare_v2(db_, gap_sql, -1, &stmt, nullptr) == SQLITE_OK) {
        BindText(stmt, 1, effective_user);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const std::string topic = ColumnText(stmt, 0);
            const std::string description = ColumnText(stmt, 1);
            const double score = TermScore(topic + " " + description, terms);
            AddRankedMemory(ranked, score > 0.0 ? score + 0.30 : 0.0, order++,
                            "knowledge_gap",
                            "Open knowledge gap: " + topic + ". " + description,
                            ColumnText(stmt, 2));
        }
    }
    sqlite3_finalize(stmt);

    std::sort(ranked.begin(), ranked.end(), [](const RankedMemory& a, const RankedMemory& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.order < b.order;
    });
    for (const auto& item : ranked) {
        if (static_cast<int>(out.size()) >= capped_limit) break;
        out.push_back(item.message);
    }
    return out;
}

std::vector<SourceChunk> MemoryStore::SearchCounselingSources(const std::string& query,
                                                              bool include_high_intensity,
                                                              int limit) const {
    std::vector<SourceChunk> out;
    if (!db_ || limit <= 0) return out;
    const auto terms = ExpandSourceTerms(query, QueryTerms(query));
    if (terms.empty()) return out;
    const std::string match = JoinTermsForFts(terms);
    if (match.empty()) return out;

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT s.title, s.content, s.tags, s.intensity, s.safety_class "
        "FROM counseling_sources_fts f "
        "JOIN counseling_sources s ON s.id=f.rowid "
        "WHERE counseling_sources_fts MATCH ? "
        "AND (?=1 OR s.intensity!='high') "
        "AND s.safety_class='support' "
        "ORDER BY CASE s.preference WHEN 'gentle_practical' THEN 0 ELSE 1 END, bm25(counseling_sources_fts) "
        "LIMIT ?;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
    BindText(stmt, 1, match);
    sqlite3_bind_int(stmt, 2, include_high_intensity ? 1 : 0);
    sqlite3_bind_int(stmt, 3, limit);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        out.push_back({
            ColumnText(stmt, 0),
            ColumnText(stmt, 1),
            ColumnText(stmt, 2),
            ColumnText(stmt, 3),
            ColumnText(stmt, 4),
        });
    }
    sqlite3_finalize(stmt);
    return out;
}

std::vector<EmotionSignal> MemoryStore::RecentEmotionSignals(const std::string& user, int limit) const {
    std::vector<EmotionSignal> out;
    if (!db_) return out;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT label, intensity FROM native_emotion_signals WHERE COALESCE(user,'aaron')=? ORDER BY id DESC LIMIT ?;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
    BindText(stmt, 1, user.empty() ? "aaron" : user);
    sqlite3_bind_int(stmt, 2, limit);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        out.push_back({
            ColumnText(stmt, 0),
            sqlite3_column_int(stmt, 1),
        });
    }
    sqlite3_finalize(stmt);
    return out;
}

std::vector<EmotionCheckin> MemoryStore::RecentEmotionCheckins(const std::string& user,
                                                               int limit,
                                                               int days,
                                                               const std::string& emotion) const {
    std::vector<EmotionCheckin> out;
    if (!db_ || limit <= 0) return out;
    const int capped_limit = std::clamp(limit, 1, 200);
    const int capped_days = std::max(0, days);
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT id, timestamp, session, user, emotion, intensity, valence, body_location, trigger, note, "
        "source_message_id, confidence, captured_by "
        "FROM emotional_checkins "
        "WHERE user=? "
        "AND (?='' OR emotion=?) "
        "AND (?=0 OR timestamp >= datetime('now', '-' || ? || ' days')) "
        "ORDER BY id DESC LIMIT ?;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
    BindText(stmt, 1, user.empty() ? "aaron" : user);
    BindText(stmt, 2, emotion);
    BindText(stmt, 3, emotion);
    sqlite3_bind_int(stmt, 4, capped_days);
    sqlite3_bind_int(stmt, 5, capped_days);
    sqlite3_bind_int(stmt, 6, capped_limit);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        EmotionCheckin checkin;
        checkin.id = sqlite3_column_int(stmt, 0);
        checkin.timestamp = ColumnText(stmt, 1);
        checkin.session = ColumnText(stmt, 2);
        checkin.user = ColumnText(stmt, 3);
        checkin.emotion = ColumnText(stmt, 4);
        if (sqlite3_column_type(stmt, 5) != SQLITE_NULL) checkin.intensity = sqlite3_column_int(stmt, 5);
        if (sqlite3_column_type(stmt, 6) != SQLITE_NULL) checkin.valence = sqlite3_column_double(stmt, 6);
        checkin.body_location = ColumnText(stmt, 7);
        checkin.trigger = ColumnText(stmt, 8);
        checkin.note = ColumnText(stmt, 9);
        if (sqlite3_column_type(stmt, 10) != SQLITE_NULL) {
            checkin.source_message_id = sqlite3_column_int64(stmt, 10);
        }
        if (sqlite3_column_type(stmt, 11) != SQLITE_NULL) checkin.confidence = sqlite3_column_double(stmt, 11);
        checkin.captured_by = ColumnText(stmt, 12);
        out.push_back(std::move(checkin));
    }
    sqlite3_finalize(stmt);
    return out;
}

std::vector<EmotionCheckin> MemoryStore::RecentEmotionCheckins(int limit) const {
    return RecentEmotionCheckins("aaron", limit, 0, "");
}

int MemoryStore::DeleteSession(const std::string& session_id) {
    if (!db_) return 0;
    int changed = 0;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM native_messages WHERE session_id=?;", -1, &stmt, nullptr) == SQLITE_OK) {
        BindText(stmt, 1, session_id);
        sqlite3_step(stmt);
        changed += sqlite3_changes(db_);
    }
    sqlite3_finalize(stmt);
    stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM native_emotion_signals WHERE session_id=?;", -1, &stmt, nullptr) == SQLITE_OK) {
        BindText(stmt, 1, session_id);
        sqlite3_step(stmt);
        changed += sqlite3_changes(db_);
    }
    sqlite3_finalize(stmt);
    stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM emotional_checkins WHERE session=?;", -1, &stmt, nullptr) == SQLITE_OK) {
        BindText(stmt, 1, session_id);
        sqlite3_step(stmt);
        changed += sqlite3_changes(db_);
    }
    sqlite3_finalize(stmt);
    stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM summaries WHERE session=?;", -1, &stmt, nullptr) == SQLITE_OK) {
        BindText(stmt, 1, session_id);
        sqlite3_step(stmt);
        changed += sqlite3_changes(db_);
    }
    sqlite3_finalize(stmt);
    stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM techniques WHERE session=?;", -1, &stmt, nullptr) == SQLITE_OK) {
        BindText(stmt, 1, session_id);
        sqlite3_step(stmt);
        changed += sqlite3_changes(db_);
    }
    sqlite3_finalize(stmt);
    stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM knowledge_gaps WHERE session=?;", -1, &stmt, nullptr) == SQLITE_OK) {
        BindText(stmt, 1, session_id);
        sqlite3_step(stmt);
        changed += sqlite3_changes(db_);
    }
    sqlite3_finalize(stmt);
    return changed;
}

int MemoryStore::DeleteMatching(const std::string& query) {
    if (!db_) return 0;
    const auto terms = QueryTerms(query);
    if (terms.empty()) return 0;
    int changed = 0;

    sqlite3_stmt* select = nullptr;
    std::vector<std::string> source_texts;
    if (sqlite3_prepare_v2(db_, "SELECT content FROM native_messages ORDER BY id DESC LIMIT 500;", -1, &select, nullptr) == SQLITE_OK) {
        while (sqlite3_step(select) == SQLITE_ROW) {
            const std::string content = ColumnText(select, 0);
            if (ContainsAny(Lower(content), terms)) {
                source_texts.push_back(content);
            }
        }
    }
    sqlite3_finalize(select);
    select = nullptr;

    sqlite3_stmt* del_msg = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM native_messages WHERE content=?;", -1, &del_msg, nullptr) == SQLITE_OK) {
        for (const auto& content : source_texts) {
            sqlite3_reset(del_msg);
            sqlite3_clear_bindings(del_msg);
            BindText(del_msg, 1, content);
            sqlite3_step(del_msg);
            changed += sqlite3_changes(db_);
        }
    }
    sqlite3_finalize(del_msg);

    sqlite3_stmt* del_emotion = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM native_emotion_signals WHERE source_text=?;", -1, &del_emotion, nullptr) == SQLITE_OK) {
        for (const auto& content : source_texts) {
            sqlite3_reset(del_emotion);
            sqlite3_clear_bindings(del_emotion);
            BindText(del_emotion, 1, content);
            sqlite3_step(del_emotion);
            changed += sqlite3_changes(db_);
        }
    }
    sqlite3_finalize(del_emotion);

    std::vector<sqlite3_int64> context_ids;
    if (sqlite3_prepare_v2(db_, "SELECT id, content FROM native_context_items ORDER BY id DESC LIMIT 500;", -1, &select, nullptr) == SQLITE_OK) {
        while (sqlite3_step(select) == SQLITE_ROW) {
            const std::string content = ColumnText(select, 1);
            if (ContainsAny(Lower(content), terms)) {
                context_ids.push_back(sqlite3_column_int64(select, 0));
            }
        }
    }
    sqlite3_finalize(select);
    select = nullptr;
    std::vector<sqlite3_int64> summary_ids;
    if (sqlite3_prepare_v2(db_, "SELECT id, content FROM summaries ORDER BY id DESC LIMIT 500;", -1, &select, nullptr) == SQLITE_OK) {
        while (sqlite3_step(select) == SQLITE_ROW) {
            const std::string content = ColumnText(select, 1);
            if (ContainsAny(Lower(content), terms)) {
                summary_ids.push_back(sqlite3_column_int64(select, 0));
            }
        }
    }
    sqlite3_finalize(select);
    select = nullptr;
    std::vector<std::string> profile_keys;
    if (sqlite3_prepare_v2(db_, "SELECT key, value FROM user_profile ORDER BY updated_at DESC LIMIT 300;", -1, &select, nullptr) == SQLITE_OK) {
        while (sqlite3_step(select) == SQLITE_ROW) {
            const std::string key = ColumnText(select, 0);
            const std::string value = ColumnText(select, 1);
            if (ContainsAny(Lower(key + " " + value), terms)) {
                profile_keys.push_back(key);
            }
        }
    }
    sqlite3_finalize(select);
    select = nullptr;
    std::vector<sqlite3_int64> technique_ids;
    if (sqlite3_prepare_v2(db_, "SELECT id, query, move, evidence FROM techniques ORDER BY id DESC LIMIT 500;", -1, &select, nullptr) == SQLITE_OK) {
        while (sqlite3_step(select) == SQLITE_ROW) {
            const std::string haystack = ColumnText(select, 1) + " " + ColumnText(select, 2) + " " + ColumnText(select, 3);
            if (ContainsAny(Lower(haystack), terms)) {
                technique_ids.push_back(sqlite3_column_int64(select, 0));
            }
        }
    }
    sqlite3_finalize(select);
    select = nullptr;
    std::vector<sqlite3_int64> gap_ids;
    if (sqlite3_prepare_v2(db_, "SELECT id, topic, description FROM knowledge_gaps ORDER BY updated_at DESC LIMIT 500;", -1, &select, nullptr) == SQLITE_OK) {
        while (sqlite3_step(select) == SQLITE_ROW) {
            const std::string haystack = ColumnText(select, 1) + " " + ColumnText(select, 2);
            if (ContainsAny(Lower(haystack), terms)) {
                gap_ids.push_back(sqlite3_column_int64(select, 0));
            }
        }
    }
    sqlite3_finalize(select);
    select = nullptr;
    std::vector<sqlite3_int64> checkin_ids;
    if (sqlite3_prepare_v2(db_, "SELECT id, emotion, body_location, trigger, note FROM emotional_checkins ORDER BY id DESC LIMIT 500;", -1, &select, nullptr) == SQLITE_OK) {
        while (sqlite3_step(select) == SQLITE_ROW) {
            const std::string haystack = ColumnText(select, 1) + " " + ColumnText(select, 2) + " " +
                                        ColumnText(select, 3) + " " + ColumnText(select, 4);
            if (ContainsAny(Lower(haystack), terms)) {
                checkin_ids.push_back(sqlite3_column_int64(select, 0));
            }
        }
    }
    sqlite3_finalize(select);

    sqlite3_stmt* del_id = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM native_context_items WHERE id=?;", -1, &del_id, nullptr) == SQLITE_OK) {
        for (const auto id : context_ids) {
            sqlite3_reset(del_id);
            sqlite3_clear_bindings(del_id);
            sqlite3_bind_int64(del_id, 1, id);
            sqlite3_step(del_id);
            changed += sqlite3_changes(db_);
        }
    }
    sqlite3_finalize(del_id);
    del_id = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM summaries WHERE id=?;", -1, &del_id, nullptr) == SQLITE_OK) {
        for (const auto id : summary_ids) {
            sqlite3_reset(del_id);
            sqlite3_clear_bindings(del_id);
            sqlite3_bind_int64(del_id, 1, id);
            sqlite3_step(del_id);
            changed += sqlite3_changes(db_);
        }
    }
    sqlite3_finalize(del_id);
    sqlite3_stmt* del_profile = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM user_profile WHERE key=?;", -1, &del_profile, nullptr) == SQLITE_OK) {
        for (const auto& key : profile_keys) {
            sqlite3_reset(del_profile);
            sqlite3_clear_bindings(del_profile);
            BindText(del_profile, 1, key);
            sqlite3_step(del_profile);
            changed += sqlite3_changes(db_);
        }
    }
    sqlite3_finalize(del_profile);
    del_id = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM techniques WHERE id=?;", -1, &del_id, nullptr) == SQLITE_OK) {
        for (const auto id : technique_ids) {
            sqlite3_reset(del_id);
            sqlite3_clear_bindings(del_id);
            sqlite3_bind_int64(del_id, 1, id);
            sqlite3_step(del_id);
            changed += sqlite3_changes(db_);
        }
    }
    sqlite3_finalize(del_id);
    del_id = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM knowledge_gaps WHERE id=?;", -1, &del_id, nullptr) == SQLITE_OK) {
        for (const auto id : gap_ids) {
            sqlite3_reset(del_id);
            sqlite3_clear_bindings(del_id);
            sqlite3_bind_int64(del_id, 1, id);
            sqlite3_step(del_id);
            changed += sqlite3_changes(db_);
        }
    }
    sqlite3_finalize(del_id);
    del_id = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM emotional_checkins WHERE id=?;", -1, &del_id, nullptr) == SQLITE_OK) {
        for (const auto id : checkin_ids) {
            sqlite3_reset(del_id);
            sqlite3_clear_bindings(del_id);
            sqlite3_bind_int64(del_id, 1, id);
            sqlite3_step(del_id);
            changed += sqlite3_changes(db_);
        }
    }
    sqlite3_finalize(del_id);
    return changed;
}

int MemoryStore::WipeAll() {
    if (!db_) return 0;
    int changed = 0;
    if (Exec(db_, "DELETE FROM native_messages;")) {
        changed += sqlite3_changes(db_);
    }
    if (Exec(db_, "DELETE FROM native_emotion_signals;")) {
        changed += sqlite3_changes(db_);
    }
    if (Exec(db_, "DELETE FROM emotional_checkins;")) {
        changed += sqlite3_changes(db_);
    }
    if (Exec(db_, "DELETE FROM native_context_items;")) {
        changed += sqlite3_changes(db_);
    }
    if (Exec(db_, "DELETE FROM summaries;")) {
        changed += sqlite3_changes(db_);
    }
    if (Exec(db_, "DELETE FROM user_profile;")) {
        changed += sqlite3_changes(db_);
    }
    if (Exec(db_, "DELETE FROM techniques;")) {
        changed += sqlite3_changes(db_);
    }
    if (Exec(db_, "DELETE FROM knowledge_gaps;")) {
        changed += sqlite3_changes(db_);
    }
    return changed;
}

int MemoryStore::MessageCount() const {
    if (!db_) return 0;
    sqlite3_stmt* stmt = nullptr;
    int count = 0;
    if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM native_messages;", -1, &stmt, nullptr) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

EmotionSignal DetectEmotion(std::string_view text) {
    const std::string lower = Lower(text);
    EmotionSignal signal;
    if (ContainsAny(lower, {"panic", "terrified", "scared", "afraid", "anxious", "anxiety"})) {
        signal = {"anxiety", 7};
    } else if (ContainsAny(lower, {"sad", "grief", "lonely", "depressed", "hopeless"})) {
        signal = {"sadness", 7};
    } else if (ContainsAny(lower, {"anger", "angry", "furious", "resent", "rage"})) {
        signal = {"anger", 7};
    } else if (ContainsAny(lower, {"overwhelmed", "stressed", "too much", "burned out"})) {
        signal = {"overwhelm", 6};
    } else if (ContainsAny(lower, {"happy", "grateful", "better", "proud", "hopeful", "positive",
                                   "that's sick", "thats sick", "that is sick", "this is sick",
                                   "that's fire", "thats fire", "that's dope", "thats dope",
                                   "big w", "huge w", "lets go", "let's go", "cookin",
                                   "cooking", "clean win"})) {
        signal = {"positive", 5};
    }
    if (ContainsAny(lower, {"extremely", "unbearable", "can't", "cannot", "really"})) {
        signal.intensity = std::min(10, signal.intensity + 2);
    }
    return signal;
}

std::vector<std::string> QueryTerms(std::string_view text) {
    static const std::unordered_set<std::string> stop = {
        "the", "and", "that", "this", "with", "have", "just", "about", "what", "when",
        "where", "why", "how", "you", "your", "for", "are", "was", "were", "but",
        "forget", "delete", "remove", "erase", "memory", "memories", "bring", "again",
        "discuss", "anymore", "remember", "clear", "from", "chat", "conversation"
    };
    std::vector<std::string> terms;
    std::string current;
    for (char c : Lower(text)) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            current.push_back(c);
        } else if (!current.empty()) {
            if (current.size() >= 4 && !stop.contains(current)) terms.push_back(current);
            current.clear();
        }
    }
    if (current.size() >= 4 && !stop.contains(current)) terms.push_back(current);
    std::sort(terms.begin(), terms.end());
    terms.erase(std::unique(terms.begin(), terms.end()), terms.end());
    return terms;
}

}  // namespace symbion
