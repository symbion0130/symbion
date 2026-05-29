#include "memory_store.h"

#include "sqlite3.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>
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

std::string ColumnText(sqlite3_stmt* stmt, int index) {
    const unsigned char* text = sqlite3_column_text(stmt, index);
    return text ? reinterpret_cast<const char*>(text) : "";
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
                     "ON native_emotion_signals(created_at);") &&
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
           AddColumnIfMissing(db_, "summaries", "embedding", "embedding BLOB") &&
           AddColumnIfMissing(db_, "summaries", "user", "user TEXT") &&
           AddColumnIfMissing(db_, "techniques", "evidence", "evidence TEXT") &&
           AddColumnIfMissing(db_, "techniques", "embedding", "embedding BLOB") &&
           AddColumnIfMissing(db_, "techniques", "source", "source TEXT DEFAULT 'local'") &&
           AddColumnIfMissing(db_, "techniques", "shared_at", "shared_at TEXT");
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

std::vector<ChatMessage> MemoryStore::AmbientContext(int limit) const {
    std::vector<ChatMessage> out;
    if (!db_ || limit <= 0) return out;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT kind, content, updated_at FROM native_context_items "
        "WHERE kind IN ('profile', 'technique') "
        "ORDER BY CASE kind WHEN 'profile' THEN 0 ELSE 1 END, updated_at DESC LIMIT ?;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_int(stmt, 1, limit);
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

std::vector<ChatMessage> MemoryStore::RetrieveRelevant(const std::string& query, int limit) const {
    std::vector<ChatMessage> out;
    if (!db_) return out;
    const auto terms = QueryTerms(query);
    if (terms.empty()) return out;
    const int capped_limit = std::max(0, limit);
    if (capped_limit == 0) return out;

    std::vector<RankedMemory> ranked;

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT role, content, created_at FROM native_messages WHERE role='user' ORDER BY id DESC LIMIT 300;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
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
        "ORDER BY updated_at DESC LIMIT 400;";
    if (sqlite3_prepare_v2(db_, ctx_sql, -1, &stmt, nullptr) == SQLITE_OK) {
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
        "ORDER BY id DESC LIMIT 200;";
    if (sqlite3_prepare_v2(db_, summary_sql, -1, &stmt, nullptr) == SQLITE_OK) {
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
        "ORDER BY updated_at DESC LIMIT 80;";
    if (sqlite3_prepare_v2(db_, profile_sql, -1, &stmt, nullptr) == SQLITE_OK) {
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
        "ORDER BY id DESC LIMIT 200;";
    if (sqlite3_prepare_v2(db_, technique_sql, -1, &stmt, nullptr) == SQLITE_OK) {
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

std::vector<EmotionSignal> MemoryStore::RecentEmotionSignals(int limit) const {
    std::vector<EmotionSignal> out;
    if (!db_) return out;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT label, intensity FROM native_emotion_signals ORDER BY id DESC LIMIT ?;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_int(stmt, 1, limit);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        out.push_back({
            ColumnText(stmt, 0),
            sqlite3_column_int(stmt, 1),
        });
    }
    sqlite3_finalize(stmt);
    return out;
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
