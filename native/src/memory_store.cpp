#include "memory_store.h"

#include "sqlite3.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_set>

namespace symbion {

namespace {

bool Exec(sqlite3* db, const char* sql) {
    char* error = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &error);
    if (error) {
        sqlite3_free(error);
    }
    return rc == SQLITE_OK;
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

void BindText(sqlite3_stmt* stmt, int index, const std::string& value) {
    sqlite3_bind_text(stmt, index, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
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
    return Exec(db_, "PRAGMA journal_mode=WAL;") &&
           Exec(db_, "PRAGMA busy_timeout=5000;") &&
           Exec(db_, "CREATE TABLE IF NOT EXISTS native_messages ("
                     "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                     "session_id TEXT NOT NULL,"
                     "role TEXT NOT NULL,"
                     "content TEXT NOT NULL,"
                     "created_at TEXT NOT NULL DEFAULT (datetime('now')));") &&
           Exec(db_, "CREATE INDEX IF NOT EXISTS idx_native_messages_session_time "
                     "ON native_messages(session_id, created_at);") &&
           Exec(db_, "CREATE TABLE IF NOT EXISTS native_emotion_signals ("
                     "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                     "session_id TEXT NOT NULL,"
                     "label TEXT NOT NULL,"
                     "intensity INTEGER NOT NULL,"
                     "source_text TEXT NOT NULL,"
                     "created_at TEXT NOT NULL DEFAULT (datetime('now')));") &&
           Exec(db_, "CREATE INDEX IF NOT EXISTS idx_native_emotions_time "
                     "ON native_emotion_signals(created_at);");
}

bool MemoryStore::SaveMessage(const std::string& session_id, const std::string& role, const std::string& content) {
    if (!db_) return false;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO native_messages(session_id, role, content) VALUES(?, ?, ?);";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    BindText(stmt, 1, session_id);
    BindText(stmt, 2, role);
    BindText(stmt, 3, content);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool MemoryStore::SaveEmotion(const std::string& session_id, const std::string& source_text, const EmotionSignal& signal) {
    if (!db_ || signal.label.empty() || signal.intensity <= 0) return false;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO native_emotion_signals(session_id, label, intensity, source_text) VALUES(?, ?, ?, ?);";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    BindText(stmt, 1, session_id);
    BindText(stmt, 2, signal.label);
    sqlite3_bind_int(stmt, 3, signal.intensity);
    BindText(stmt, 4, source_text);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
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
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)),
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)),
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)),
        });
    }
    sqlite3_finalize(stmt);
    std::reverse(out.begin(), out.end());
    return out;
}

std::vector<ChatMessage> MemoryStore::RetrieveRelevant(const std::string& query, int limit) const {
    std::vector<ChatMessage> out;
    if (!db_) return out;
    const auto terms = QueryTerms(query);
    if (terms.empty()) return out;

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT role, content, created_at FROM native_messages ORDER BY id DESC LIMIT 300;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
    while (sqlite3_step(stmt) == SQLITE_ROW && static_cast<int>(out.size()) < limit) {
        ChatMessage msg{
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)),
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)),
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)),
        };
        if (ContainsAny(Lower(msg.content), terms)) {
            out.push_back(std::move(msg));
        }
    }
    sqlite3_finalize(stmt);
    return out;
}

std::vector<EmotionSignal> MemoryStore::RecentEmotionSignals(int limit) const {
    std::vector<EmotionSignal> out;
    if (!db_) return out;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT label, intensity FROM native_emotion_signals ORDER BY id DESC LIMIT ?;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_int(stmt, 1, limit);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        out.push_back({
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)),
            sqlite3_column_int(stmt, 1),
        });
    }
    sqlite3_finalize(stmt);
    return out;
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
    if (ContainsAny(lower, {"panic", "terrified", "scared", "afraid", "anxious"})) {
        signal = {"anxiety", 7};
    } else if (ContainsAny(lower, {"sad", "grief", "lonely", "depressed", "hopeless"})) {
        signal = {"sadness", 7};
    } else if (ContainsAny(lower, {"angry", "furious", "resent", "rage"})) {
        signal = {"anger", 7};
    } else if (ContainsAny(lower, {"overwhelmed", "stressed", "too much", "burned out"})) {
        signal = {"overwhelm", 6};
    } else if (ContainsAny(lower, {"happy", "grateful", "better", "proud", "hopeful"})) {
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
        "where", "why", "how", "you", "your", "for", "are", "was", "were", "but"
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
