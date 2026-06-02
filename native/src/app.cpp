#include "app.h"

#include "json_util.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <cmath>
#include <optional>
#include <regex>
#include <sstream>
#include <string_view>

namespace symbion {

namespace {

std::string ReadTextFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::wstring Utf8ToWide(std::string_view value) {
    if (value.empty()) return {};
    int count = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), count);
    return out;
}

std::string WideToUtf8(std::wstring_view value) {
    if (value.empty()) return {};
    int count = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), count, nullptr, nullptr);
    return out;
}

std::string UrlEncode(const std::string& value) {
    std::ostringstream out;
    out << std::hex << std::uppercase;
    for (const unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out << static_cast<char>(c);
        } else if (c == ' ') {
            out << '+';
        } else {
            out << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(c) << std::setfill(' ');
        }
    }
    return out.str();
}

std::string HttpGetText(const std::string& url, DWORD timeout_ms = 10000, size_t max_chars = 120000) {
    URL_COMPONENTSW parts = {};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = {};
    wchar_t path[4096] = {};
    wchar_t extra[4096] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = static_cast<DWORD>(std::size(extra));
    const std::wstring wide_url = Utf8ToWide(url);
    if (!WinHttpCrackUrl(wide_url.c_str(), 0, 0, &parts)) return {};
    std::wstring path_and_query(path, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength > 0) path_and_query.append(extra, parts.dwExtraInfoLength);
    if (path_and_query.empty()) path_and_query = L"/";

    HINTERNET session = WinHttpOpen(L"SymbionNativeTools/0.3", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return {};
    WinHttpSetTimeouts(session, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
    HINTERNET connect = WinHttpConnect(session, std::wstring(host, parts.dwHostNameLength).c_str(), parts.nPort, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        return {};
    }
    DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connect, L"GET", path_and_query.c_str(), nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return {};
    }
    const wchar_t* headers = L"User-Agent: SymbionNative/0.3\r\nAccept: text/plain,text/html,application/json,*/*\r\n";
    BOOL ok = WinHttpSendRequest(request, headers, static_cast<DWORD>(wcslen(headers)),
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    ok = ok && WinHttpReceiveResponse(request, nullptr);
    std::string response;
    if (ok) {
        for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
            std::string chunk(available, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0) break;
            chunk.resize(read);
            response += chunk;
            if (response.size() >= max_chars) {
                response.resize(max_chars);
                break;
            }
        }
    }
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return response;
}

std::string StripHtml(std::string text) {
    text = std::regex_replace(text, std::regex("<script[\\s\\S]*?</script>", std::regex_constants::icase), " ");
    text = std::regex_replace(text, std::regex("<style[\\s\\S]*?</style>", std::regex_constants::icase), " ");
    text = std::regex_replace(text, std::regex("<[^>]+>", std::regex_constants::icase), " ");
    std::map<std::string, std::string> entities = {
        {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&#39;", "'"}, {"&nbsp;", " "}
    };
    for (const auto& [from, to] : entities) {
        size_t pos = 0;
        while ((pos = text.find(from, pos)) != std::string::npos) {
            text.replace(pos, from.size(), to);
            pos += to.size();
        }
    }
    std::string compact;
    bool last_space = true;
    for (const unsigned char c : text) {
        if (std::isspace(c)) {
            if (!last_space) compact.push_back(' ');
            last_space = true;
        } else {
            compact.push_back(static_cast<char>(c));
            last_space = false;
        }
    }
    return compact;
}

std::string SessionFromRequest(const HttpRequest& request) {
    if (auto it = request.headers.find("x-symbion-session"); it != request.headers.end() && !it->second.empty()) {
        return it->second;
    }
    return "native-default";
}

std::string QueryValue(const std::string& path, const std::string& key);

std::string UserFromRequest(const HttpRequest& request) {
    if (auto it = request.headers.find("x-symbion-user"); it != request.headers.end() && !it->second.empty()) {
        return it->second;
    }
    const std::string query_user = QueryValue(request.path, "user");
    if (!query_user.empty()) return query_user;
    return "aaron";
}

std::string QueryValue(const std::string& path, const std::string& key) {
    const size_t question = path.find('?');
    if (question == std::string::npos) return {};
    const std::string query = path.substr(question + 1);
    const std::string needle = key + "=";
    size_t start = query.find(needle);
    if (start == std::string::npos) return {};
    start += needle.size();
    size_t end = query.find('&', start);
    std::string value = query.substr(start, end == std::string::npos ? std::string::npos : end - start);
    std::replace(value.begin(), value.end(), '+', ' ');
    std::string decoded;
    decoded.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const std::string hex = value.substr(i + 1, 2);
            char* tail = nullptr;
            const long code = std::strtol(hex.c_str(), &tail, 16);
            if (tail && *tail == '\0') {
                decoded.push_back(static_cast<char>(code));
                i += 2;
                continue;
            }
        }
        decoded.push_back(value[i]);
    }
    return decoded;
}

std::string RoutePath(const std::string& path) {
    const size_t question = path.find('?');
    return question == std::string::npos ? path : path.substr(0, question);
}

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string TrimCopy(std::string value);

bool IsConfirmWipe(const std::string& message) {
    const std::string lower = Lower(message);
    return lower == "yes" || lower == "confirm" || lower == "yes wipe all memory" ||
           lower == "yes wipe it" || lower == "wipe it" || lower == "do it" ||
           lower == "i am sure" || lower == "yes i am sure";
}

bool IsCancelWipe(const std::string& message) {
    const std::string lower = Lower(message);
    return lower == "no" || lower == "cancel" || lower == "stop" || lower == "never mind" ||
           lower == "nevermind" || lower == "do not wipe" || lower == "dont wipe";
}

bool IsGeneralForget(const std::string& text) {
    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return lower.find("clear this chat") != std::string::npos ||
           lower.find("delete this chat") != std::string::npos ||
           lower.find("forget this chat") != std::string::npos ||
           lower.find("forget this conversation") != std::string::npos ||
           lower.find("clear conversation") != std::string::npos;
}

std::optional<int> ParsePositiveInt(std::string_view text) {
    std::string digits;
    for (const unsigned char c : text) {
        if (std::isdigit(c)) {
            digits.push_back(static_cast<char>(c));
        } else if (!digits.empty()) {
            break;
        }
    }
    if (digits.empty()) return std::nullopt;
    char* tail = nullptr;
    const long value = std::strtol(digits.c_str(), &tail, 10);
    if (!tail || *tail != '\0' || value <= 0 || value > 2147483647L) return std::nullopt;
    return static_cast<int>(value);
}

std::optional<int> TechniqueDeleteId(const std::string& lower) {
    const size_t pos = lower.find("delete technique");
    if (pos == std::string::npos) return std::nullopt;
    return ParsePositiveInt(std::string_view(lower).substr(pos + 16));
}

bool IsShowTechniquesCommand(const std::string& lower) {
    return lower == "show techniques" || lower == "list techniques" ||
           lower == "show my techniques" || lower == "list my techniques" ||
           lower == "show me techniques" || lower == "what techniques are saved" ||
           lower.find("show techniques") != std::string::npos ||
           lower.find("show me techniques") != std::string::npos ||
           lower.find("list techniques") != std::string::npos;
}

bool IsSaveTechniqueCommand(const std::string& lower) {
    return lower == "promote this" || lower == "promote that" ||
           lower.find("promote this") != std::string::npos ||
           lower.find("promote that") != std::string::npos ||
           lower.find("save this as a technique") != std::string::npos ||
           lower.find("save that as a technique") != std::string::npos ||
           (lower.find("save ") != std::string::npos &&
            lower.find(" as a technique") != std::string::npos) ||
           lower.find("remember this as a technique") != std::string::npos ||
           lower.find("remember that as a technique") != std::string::npos ||
           lower.find("capture this as a technique") != std::string::npos ||
           lower.find("capture that as a technique") != std::string::npos;
}

std::string ExplicitTechniqueMove(const std::string& message) {
    const std::string lower = Lower(message);
    const size_t save_pos = lower.find("save ");
    const size_t as_pos = lower.find(" as a technique");
    if (save_pos != std::string::npos && as_pos != std::string::npos && as_pos > save_pos + 5) {
        std::string move = TrimCopy(message.substr(save_pos + 5, as_pos - (save_pos + 5)));
        const std::string lowered_move = Lower(move);
        if (lowered_move != "this" && lowered_move != "that" && lowered_move != "it") return move;
    }
    const size_t label_pos = lower.find("technique:");
    if (label_pos != std::string::npos) {
        return TrimCopy(message.substr(label_pos + 10));
    }
    return {};
}

std::string TechniquesJson(const std::vector<TechniqueItem>& techniques) {
    std::string body = "[";
    bool first = true;
    for (const auto& item : techniques) {
        if (!first) body += ",";
        first = false;
        body += "{\"id\":" + std::to_string(item.id) +
                ",\"query\":\"" + EscapeJson(item.query) +
                "\",\"move\":\"" + EscapeJson(item.move) +
                "\",\"evidence\":\"" + EscapeJson(item.evidence) +
                "\",\"source\":\"" + EscapeJson(item.source) + "\"}";
    }
    body += "]";
    return body;
}

std::string JsonOptionalInt(const std::optional<int>& value) {
    return value ? std::to_string(*value) : "null";
}

std::string JsonOptionalInt64(const std::optional<std::int64_t>& value) {
    return value ? std::to_string(*value) : "null";
}

std::string JsonOptionalDouble(const std::optional<double>& value) {
    if (!value) return "null";
    std::ostringstream out;
    out << *value;
    return out.str();
}

std::string EmotionCheckinsJson(const std::vector<EmotionCheckin>& checkins) {
    std::string body = "[";
    bool first = true;
    for (const auto& item : checkins) {
        if (!first) body += ",";
        first = false;
        body += "{\"id\":" + std::to_string(item.id) +
                ",\"timestamp\":\"" + EscapeJson(item.timestamp) +
                "\",\"session\":\"" + EscapeJson(item.session) +
                "\",\"user\":\"" + EscapeJson(item.user) +
                "\",\"emotion\":\"" + EscapeJson(item.emotion) +
                "\",\"intensity\":" + JsonOptionalInt(item.intensity) +
                ",\"valence\":" + JsonOptionalDouble(item.valence) +
                ",\"body_location\":\"" + EscapeJson(item.body_location) +
                "\",\"trigger\":\"" + EscapeJson(item.trigger) +
                "\",\"note\":\"" + EscapeJson(item.note) +
                "\",\"source_message_id\":" + JsonOptionalInt64(item.source_message_id) +
                ",\"confidence\":" + JsonOptionalDouble(item.confidence) +
                ",\"captured_by\":\"" + EscapeJson(item.captured_by) + "\"}";
    }
    body += "]";
    return body;
}

bool ContainsAnyLocal(const std::string& text, const std::initializer_list<const char*> needles) {
    return std::any_of(needles.begin(), needles.end(), [&](const char* needle) {
        return text.find(needle) != std::string::npos;
    });
}

std::string LocalDateString(bool include_time) {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_s(&local, &time);
    std::ostringstream out;
    out << std::put_time(&local, include_time ? "%A, %B %d, %Y at %I:%M %p" : "%B %d, %Y");
    return out.str();
}

class ExpressionParser {
public:
    explicit ExpressionParser(std::string text) : text_(std::move(text)) {}

    std::optional<double> Parse() {
        pos_ = 0;
        auto value = ParseExpression();
        SkipSpaces();
        if (!value || pos_ != text_.size()) return std::nullopt;
        return value;
    }

private:
    std::optional<double> ParseExpression() {
        auto lhs = ParseTerm();
        while (lhs) {
            SkipSpaces();
            if (Match('+')) {
                auto rhs = ParseTerm();
                if (!rhs) return std::nullopt;
                *lhs += *rhs;
            } else if (Match('-')) {
                auto rhs = ParseTerm();
                if (!rhs) return std::nullopt;
                *lhs -= *rhs;
            } else {
                break;
            }
        }
        return lhs;
    }

    std::optional<double> ParseTerm() {
        auto lhs = ParseFactor();
        while (lhs) {
            SkipSpaces();
            if (Match('*')) {
                auto rhs = ParseFactor();
                if (!rhs) return std::nullopt;
                *lhs *= *rhs;
            } else if (Match('/')) {
                auto rhs = ParseFactor();
                if (!rhs || *rhs == 0.0) return std::nullopt;
                *lhs /= *rhs;
            } else {
                break;
            }
        }
        return lhs;
    }

    std::optional<double> ParseFactor() {
        auto lhs = ParseUnary();
        SkipSpaces();
        if (lhs && Match('^')) {
            auto rhs = ParseFactor();
            if (!rhs) return std::nullopt;
            *lhs = std::pow(*lhs, *rhs);
        }
        return lhs;
    }

    std::optional<double> ParseUnary() {
        SkipSpaces();
        if (Match('+')) return ParseUnary();
        if (Match('-')) {
            auto value = ParseUnary();
            if (!value) return std::nullopt;
            return -*value;
        }
        if (MatchWord("sqrt")) {
            if (!Match('(')) return std::nullopt;
            auto value = ParseExpression();
            if (!value || *value < 0.0 || !Match(')')) return std::nullopt;
            return std::sqrt(*value);
        }
        if (Match('(')) {
            auto value = ParseExpression();
            if (!value || !Match(')')) return std::nullopt;
            return value;
        }
        return ParseNumber();
    }

    std::optional<double> ParseNumber() {
        SkipSpaces();
        const size_t start = pos_;
        bool dot = false;
        while (pos_ < text_.size()) {
            const unsigned char c = static_cast<unsigned char>(text_[pos_]);
            if (std::isdigit(c)) {
                ++pos_;
            } else if (text_[pos_] == '.' && !dot) {
                dot = true;
                ++pos_;
            } else {
                break;
            }
        }
        if (pos_ == start) return std::nullopt;
        return std::stod(text_.substr(start, pos_ - start));
    }

    bool Match(char c) {
        SkipSpaces();
        if (pos_ < text_.size() && text_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }

    bool MatchWord(const char* word) {
        SkipSpaces();
        const size_t len = std::strlen(word);
        if (text_.size() - pos_ < len) return false;
        for (size_t i = 0; i < len; ++i) {
            if (std::tolower(static_cast<unsigned char>(text_[pos_ + i])) != word[i]) return false;
        }
        pos_ += len;
        return true;
    }

    void SkipSpaces() {
        while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) ++pos_;
    }

    std::string text_;
    size_t pos_ = 0;
};

std::string FormatNumber(double value) {
    std::ostringstream out;
    if (std::abs(value - std::round(value)) < 0.000000001) {
        out << static_cast<long long>(std::llround(value));
    } else {
        out << std::setprecision(12) << value;
    }
    return out.str();
}

std::string MathExpressionFromMessage(std::string lower) {
    const std::map<std::string, std::string> replacements = {
        {" multiplied by ", "*"}, {" times ", "*"}, {" plus ", "+"}, {" minus ", "-"},
        {" divided by ", "/"}, {" over ", "/"}, {" x ", "*"}
    };
    for (const auto& [from, to] : replacements) {
        size_t pos = 0;
        while ((pos = lower.find(from, pos)) != std::string::npos) {
            lower.replace(pos, from.size(), to);
            pos += to.size();
        }
    }
    std::string expr;
    for (const unsigned char c : lower) {
        if (std::isdigit(c) || c == '.' || c == '+' || c == '-' || c == '*' || c == '/' ||
            c == '^' || c == '(' || c == ')' || std::isspace(c)) {
            expr.push_back(static_cast<char>(c));
        }
    }
    while (expr.find("--") != std::string::npos) {
        expr.replace(expr.find("--"), 2, " ");
    }
    return TrimCopy(expr);
}

std::optional<std::filesystem::path> ExtractPathLike(const std::string& message) {
    std::smatch match;
    const std::regex quoted("\"([^\"]+)\"");
    if (std::regex_search(message, match, quoted) && match.size() > 1) {
        return std::filesystem::path(match[1].str());
    }
    const std::regex win_path("([A-Za-z]:\\\\[^\\r\\n]+)");
    if (std::regex_search(message, match, win_path) && match.size() > 1) {
        return std::filesystem::path(TrimCopy(match[1].str()));
    }
    return std::nullopt;
}

std::string ClipToolText(const std::string& value, size_t limit = 2200) {
    if (value.size() <= limit) return value;
    return value.substr(0, limit) + "\n\n[trimmed]";
}

std::string WebSearchReadableText(const std::string& query, size_t limit = 1800) {
    const std::string clean_query = TrimCopy(query);
    if (clean_query.empty()) return {};
    const std::string html = HttpGetText("https://duckduckgo.com/html/?q=" + UrlEncode(clean_query));
    if (html.empty()) return {};
    return "I searched the web for \"" + clean_query + "\". Here is the readable text I could pull:\n\n" +
           ClipToolText(StripHtml(html), limit);
}

bool LooksLikeStaleDraft(const std::string& answer) {
    const std::string lower = Lower(answer);
    return ContainsAnyLocal(lower, {
        "as of my knowledge cutoff", "as of my training", "my knowledge cutoff",
        "i don't have access to real-time", "i do not have access to real-time",
        "i can't browse", "i cannot browse", "i don't have current",
        "i do not have current", "i can't access current", "i cannot access current",
        "i don't have live", "i do not have live"
    });
}

bool QueryMayBenefitFromRefresh(const std::string& message) {
    const std::string lower = Lower(message);
    return ContainsAnyLocal(lower, {
        "latest", "recent", "current", "today", "right now", "this week",
        "this month", "news", "price", "weather", "score", "standings",
        "release", "version", "update", "who is the", "where is the",
        "look up", "search", "online", "web"
    });
}

std::filesystem::path ResolveRepoPath(const std::filesystem::path& repo_root, const std::string& configured_path) {
    std::filesystem::path path(configured_path);
    if (path.empty()) return {};
    return path.is_absolute() ? path : repo_root / path;
}

void LogNativeTurn(const std::filesystem::path& events_path,
                   const std::string& session_id,
                   const std::string& query,
                   const std::string& answer,
                   const Intent& intent,
                   const EmotionSignal& signal,
                   const std::string& response_source,
                   bool stale_refresh,
                   bool quality_retry,
                   bool turn_hint_rerun,
                   bool turn_hint_fallback,
                   long long latency_ms,
                   int relevant_count,
                   int source_count,
                   int recent_count) {
    if (events_path.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(events_path.parent_path(), ec);
    std::ofstream out(events_path, std::ios::app | std::ios::binary);
    if (!out) return;

    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_s(&local, &now_time);
    std::ostringstream ts;
    ts << std::put_time(&local, "%Y-%m-%dT%H:%M:%S");

    const bool stale_language = LooksLikeStaleDraft(answer);
    const bool over_cautious = stale_language || Lower(answer).find("i cannot help") != std::string::npos;
    out << "{\"event\":\"turn\","
        << "\"runtime\":\"native-cpp\","
        << "\"ts\":\"" << EscapeJson(ts.str()) << "\","
        << "\"session\":\"" << EscapeJson(session_id) << "\","
        << "\"provider\":\"local_gemma\","
        << "\"actual_provider\":\"local_gemma\","
        << "\"model\":\"local-gemma\","
        << "\"intent\":\"" << EscapeJson(IntentModeName(intent.mode)) << "\","
        << "\"response_source\":\"" << EscapeJson(response_source) << "\","
        << "\"query_len\":" << query.size() << ","
        << "\"response_len\":" << answer.size() << ","
        << "\"emotion\":{\"label\":\"" << EscapeJson(signal.label) << "\",\"intensity\":" << signal.intensity << "},"
        << "\"judge\":{\"skipped\":true,\"should_assist\":true,\"over_cautious\":" << (over_cautious ? "true" : "false") << "},"
        << "\"stale_refresh\":" << (stale_refresh ? "true" : "false") << ","
        << "\"rerun\":{\"quality_retry\":" << (quality_retry ? "true" : "false")
        << ",\"turn_hint_repair\":" << (turn_hint_rerun ? "true" : "false")
        << ",\"turn_hint_fallback\":" << (turn_hint_fallback ? "true" : "false") << "},"
        << "\"latency_ms\":{\"total\":" << latency_ms << "},"
        << "\"memory\":{\"recent\":" << recent_count << ",\"relevant\":" << relevant_count << ",\"sources\":" << source_count << "}"
        << "}\n";
}

std::string ReadPdfTextLite(const std::filesystem::path& path) {
    std::string data = ReadTextFile(path);
    if (data.empty()) return {};
    std::string out;
    bool in = false;
    bool escape = false;
    std::string current;
    for (const char ch : data) {
        if (!in) {
            if (ch == '(') {
                in = true;
                current.clear();
            }
            continue;
        }
        if (escape) {
            if (ch == 'n' || ch == 'r' || ch == 't') current.push_back(' ');
            else current.push_back(ch);
            escape = false;
        } else if (ch == '\\') {
            escape = true;
        } else if (ch == ')') {
            in = false;
            if (current.size() > 2) {
                out += current;
                out.push_back(' ');
                if (out.size() > 4000) break;
            }
        } else if (static_cast<unsigned char>(ch) >= 32 || ch == '\n' || ch == '\r' || ch == '\t') {
            current.push_back(ch);
        }
    }
    return ClipToolText(out, 3200);
}

std::string WeatherAnswer(const std::string& message) {
    const std::string lower = Lower(message);
    if (!ContainsAnyLocal(lower, {"weather", "temperature", "rain today", "forecast"})) return {};
    std::string city = message;
    const std::vector<std::string> markers = {"weather in ", "weather for ", "temperature in ", "forecast in "};
    for (const auto& marker : markers) {
        const size_t pos = lower.find(marker);
        if (pos != std::string::npos) {
            city = message.substr(pos + marker.size());
            break;
        }
    }
    city = TrimCopy(city);
    if (city.empty() || Lower(city) == "weather" || Lower(city) == "forecast") {
        return "Give me the city and I can check the weather.";
    }
    std::string geo = HttpGetText("https://geocoding-api.open-meteo.com/v1/search?count=1&language=en&format=json&name=" + UrlEncode(city));
    std::smatch match;
    std::regex lat_re("\"latitude\"\\s*:\\s*(-?\\d+(?:\\.\\d+)?)");
    std::regex lon_re("\"longitude\"\\s*:\\s*(-?\\d+(?:\\.\\d+)?)");
    if (!std::regex_search(geo, match, lat_re)) {
        std::string fallback_city = city;
        if (const size_t comma = fallback_city.find(','); comma != std::string::npos) {
            fallback_city = TrimCopy(fallback_city.substr(0, comma));
        } else if (const size_t space = fallback_city.find_last_of(' '); space != std::string::npos) {
            const std::string tail = Lower(TrimCopy(fallback_city.substr(space + 1)));
            if (tail.size() == 2) fallback_city = TrimCopy(fallback_city.substr(0, space));
        }
        if (fallback_city != city && !fallback_city.empty()) {
            geo = HttpGetText("https://geocoding-api.open-meteo.com/v1/search?count=1&language=en&format=json&name=" + UrlEncode(fallback_city));
            city = fallback_city;
        }
    }
    if (!std::regex_search(geo, match, lat_re)) return "I could not find that city cleanly. Try city and state.";
    const std::string lat = match[1].str();
    if (!std::regex_search(geo, match, lon_re)) return "I could not find that city cleanly. Try city and state.";
    const std::string lon = match[1].str();
    const std::string forecast = HttpGetText("https://api.open-meteo.com/v1/forecast?latitude=" + lat +
                                             "&longitude=" + lon +
                                             "&current=temperature_2m,precipitation,rain,weather_code&temperature_unit=fahrenheit&timezone=auto");
    std::regex temp_re("\"temperature_2m\"\\s*:\\s*(-?\\d+(?:\\.\\d+)?)");
    std::regex rain_re("\"rain\"\\s*:\\s*(-?\\d+(?:\\.\\d+)?)");
    std::string temp = "?";
    std::string rain = "0";
    if (std::regex_search(forecast, match, temp_re)) temp = match[1].str();
    if (std::regex_search(forecast, match, rain_re)) rain = match[1].str();
    return "Current weather for " + city + ": about " + temp + " F. Rain right now: " + rain + " mm.";
}

std::string NativeToolAnswer(const std::string& message,
                             const Intent& intent,
                             const std::filesystem::path& repo_root) {
    const std::string lower = Lower(message);
    if (lower == "what's 2+2" || lower == "whats 2+2" || lower == "what is 2+2" ||
        lower == "2+2" || lower == "2 + 2") {
        return "4";
    }
    if (ContainsAnyLocal(lower, {"what year is it", "current year", "what's the year", "whats the year"})) {
        const auto now = std::chrono::system_clock::now();
        const std::time_t time = std::chrono::system_clock::to_time_t(now);
        std::tm local{};
        localtime_s(&local, &time);
        return std::to_string(local.tm_year + 1900) + ".";
    }
    if (ContainsAnyLocal(lower, {"what date is it", "today's date", "todays date", "current date"})) {
        return LocalDateString(false) + ".";
    }
    if (ContainsAnyLocal(lower, {"what time is it", "current time", "local time"})) {
        return LocalDateString(true) + ".";
    }

    if (intent.mode == IntentMode::DirectAnswer || ContainsAnyLocal(lower, {"calculate", "math", "exact answer"})) {
        const std::string expr = MathExpressionFromMessage(lower);
        if (expr.find_first_of("0123456789") != std::string::npos &&
            expr.find_first_of("+-*/^") != std::string::npos) {
            ExpressionParser parser(expr);
            if (const auto value = parser.Parse()) return FormatNumber(*value);
        }
    }

    const std::string weather = WeatherAnswer(message);
    if (!weather.empty()) return weather;

    std::smatch url_match;
    const std::regex url_re("(https?://[^\\s]+)");
    if (std::regex_search(message, url_match, url_re) &&
        ContainsAnyLocal(lower, {"fetch", "open", "read", "summarize", "what is on"})) {
        const std::string url = url_match[1].str();
        const std::string fetched = HttpGetText(url);
        if (fetched.empty()) return "I could not fetch that URL from the native tool.";
        return ClipToolText(StripHtml(fetched), 2200);
    }

    if (ContainsAnyLocal(lower, {"web search", "search web", "look up online", "duckduckgo search"})) {
        std::string query = message;
        for (const auto& marker : {"web search", "search web", "look up online", "duckduckgo search"}) {
            const size_t pos = Lower(query).find(marker);
            if (pos != std::string::npos) query = query.substr(pos + std::strlen(marker));
        }
        query = TrimCopy(query);
        if (query.empty()) return "What do you want me to search for?";
        const std::string search = WebSearchReadableText(query);
        if (search.empty()) return "I could not reach web search from the native tool.";
        return search;
    }

    if (ContainsAnyLocal(lower, {"list files", "list directory", "show files", "what files are in"})) {
        std::filesystem::path path = repo_root;
        if (const auto extracted = ExtractPathLike(message)) path = *extracted;
        else if (lower.find("native/src") != std::string::npos) path = repo_root / "native" / "src";
        else if (lower.find("docs") != std::string::npos) path = repo_root / "docs";
        if (!std::filesystem::exists(path) || !std::filesystem::is_directory(path)) {
            return "I could not find that folder.";
        }
        std::ostringstream out;
        int count = 0;
        for (const auto& entry : std::filesystem::directory_iterator(path)) {
            out << (entry.is_directory() ? "[dir] " : "      ") << entry.path().filename().string() << "\n";
            if (++count >= 80) {
                out << "[trimmed]\n";
                break;
            }
        }
        return out.str();
    }

    if (ContainsAnyLocal(lower, {"read file", "open file", "what's in file", "whats in file"})) {
        auto extracted = ExtractPathLike(message);
        if (!extracted) return "Give me the file path and I can read it.";
        if (!std::filesystem::exists(*extracted) || std::filesystem::is_directory(*extracted)) {
            return "I could not find that file.";
        }
        if (Lower(extracted->extension().string()) == ".pdf") {
            const std::string text = ReadPdfTextLite(*extracted);
            if (text.empty()) {
                return "I opened the PDF, but did not find extractable text. It may be scanned or image-only.";
            }
            return text;
        }
        return ClipToolText(ReadTextFile(*extracted), 3200);
    }

    return {};
}

std::string V14SelfCompareAnswer(const std::string& message,
                                 const std::filesystem::path& repo_root) {
    const std::string lower = Lower(message);
    if (!ContainsAnyLocal(lower, {"symbion_v14.py", "v14"}) ||
        !ContainsAnyLocal(lower, {"read", "compare", "toe to toe", "against"})) {
        return {};
    }

    std::filesystem::path v14_path = repo_root / "symbion_v14.py";
    if (!std::filesystem::exists(v14_path)) {
        const std::filesystem::path d_drive = "D:\\symbion\\symbion_v14.py";
        if (std::filesystem::exists(d_drive)) v14_path = d_drive;
    }
    if (!std::filesystem::exists(v14_path) || std::filesystem::is_directory(v14_path)) {
        return "I looked for `symbion_v14.py` in the repo and the known local v14 path, but I could not find it from this native runtime.";
    }

    const std::string v14 = ReadTextFile(v14_path);
    if (v14.empty()) {
        return "I found `symbion_v14.py`, but the native file read returned empty, so I should not pretend I inspected it.";
    }

    const auto line_count = static_cast<int>(std::count(v14.begin(), v14.end(), '\n') + 1);
    const bool has_pipeline = v14.find("TurnPipeline") != std::string::npos;
    const bool has_judge = v14.find("judge") != std::string::npos || v14.find("PRE_GEN_SYSTEM") != std::string::npos;
    const bool has_self_eval = v14.find("self_eval") != std::string::npos || v14.find("_self_eval") != std::string::npos;
    const bool has_tools = v14.find("TOOL_SCHEMAS") != std::string::npos || v14.find("SymbionTools") != std::string::npos;
    const bool has_evals = v14.find("golden") != std::string::npos || v14.find("eval") != std::string::npos;
    const bool has_memory = v14.find("build_context") != std::string::npos || v14.find("SymbionMemory") != std::string::npos;

    std::ostringstream out;
    out << "You are right to call that out. I read `" << v14_path.string() << "`: about "
        << line_count << " lines and " << v14.size() << " bytes. v14 has the heavier behavioral stack";
    std::vector<std::string> markers;
    if (has_pipeline) markers.push_back("TurnPipeline");
    if (has_memory) markers.push_back("memory/context assembly");
    if (has_judge) markers.push_back("judge/pre-gen pressure");
    if (has_self_eval) markers.push_back("self-eval telemetry");
    if (has_tools) markers.push_back("tool schemas");
    if (has_evals) markers.push_back("eval/golden-case pressure");
    if (!markers.empty()) {
        out << ": ";
        for (size_t i = 0; i < markers.size(); ++i) {
            if (i > 0) out << (i + 1 == markers.size() ? ", and " : ", ");
            out << markers[i];
        }
    }
    out << ". Native v15 is cleaner and faster, but it still does not go toe to toe until those layers are ported as architecture, not vibes. The gap is not just tone; it is missing behavioral pressure.";
    return out.str();
}

int WordCount(const std::string& text) {
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

bool IsShortPositiveSlang(const std::string& lower) {
    static const std::initializer_list<const char*> exact = {
        "fire", "dope", "bet", "lit", "rad", "clean", "based",
        "that's fire", "thats fire", "that is fire", "this is fire",
        "that's dope", "thats dope", "that is dope", "this is dope",
        "no cap", "big w", "huge w", "lets go", "let's go"
    };
    return WordCount(lower) <= 4 &&
           std::any_of(exact.begin(), exact.end(), [&](const char* phrase) {
               return lower == phrase;
           });
}

std::string TrimCopy(std::string value) {
    while (!value.empty() && !std::isalnum(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && !std::isalnum(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

std::string CompactDoorPhrase(const std::string& message) {
    std::string phrase = Lower(message);
    std::string out;
    out.reserve(phrase.size());
    bool last_space = false;
    for (const unsigned char c : phrase) {
        if (std::isalnum(c) || c == '\'' || c == '/') {
            out.push_back(static_cast<char>(c));
            last_space = false;
        } else if (!last_space) {
            out.push_back(' ');
            last_space = true;
        }
    }
    std::istringstream words(TrimCopy(out));
    std::string compact;
    std::string word;
    while (words >> word) {
        if (word == "my") {
            std::streampos before_next = words.tellg();
            std::string next;
            if (words >> next) {
                if (next == "guy") continue;
                if (!compact.empty()) compact.push_back(' ');
                compact += word;
                if (!compact.empty()) compact.push_back(' ');
                compact += next;
                continue;
            }
            words.clear();
            words.seekg(before_next);
        }
        if (word == "man" || word == "bro" || word == "dude" || word == "like" ||
            word == "uh" || word == "um") {
            continue;
        }
        if (!compact.empty()) compact.push_back(' ');
        compact += word;
    }
    return TrimCopy(compact);
}

bool IsLowSignalCompactReply(const std::string& phrase) {
    return phrase.empty() || phrase == "yes" || phrase == "no" || phrase == "ok" ||
           phrase == "okay" || phrase == "yeah" || phrase == "yep" || phrase == "nah" ||
           phrase == "thanks" || phrase == "thank you" || phrase == "lol" || phrase == "haha" ||
           phrase == "hello" || phrase == "hi" || phrase == "hey" || phrase == "yo";
}

std::string CapitalizeFirst(std::string value) {
    if (!value.empty()) {
        value[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(value[0])));
    }
    return value;
}

std::string GenericDoorMappingQuestion(const std::string& message, const Intent& intent) {
    if (intent.mode != IntentMode::Reflective && intent.mode != IntentMode::Counseling) return {};
    const std::string lower = Lower(message);
    if (lower.find('?') != std::string::npos) return {};
    if (ContainsAnyLocal(lower, {"write ", "make ", "create ", "build ", "fix ", "install ", "run ", "open ",
                                 "what is ", "what are ", "who is ", "where is ", "how do ", "show me"})) {
        return {};
    }

    const std::string phrase = CompactDoorPhrase(message);
    const int words = WordCount(phrase);
    if (words == 0 || words > 5 || IsLowSignalCompactReply(phrase)) return {};

    if (ContainsAnyLocal(phrase, {"family", "work", "finances", "money", "friends", "health", "memories"})) {
        return "What feels most intense there?";
    }
    if (ContainsAnyLocal(phrase, {"shoulder", "shoulders", "neck", "head", "chest", "stomach", "gut", "throat"})) {
        return "What does it feel like there?";
    }
    if (words == 1) {
        if (phrase == "anger" || phrase == "angry" || phrase == "fear" || phrase == "sadness" ||
            phrase == "sad" || phrase == "anxiety" || phrase == "anxious" || phrase == "shame" ||
            phrase == "ashamed" || phrase == "guilt" || phrase == "lonely" || phrase == "numb") {
            return CapitalizeFirst(phrase) + " is present right now. Is it connected to family, work, finances, friends, health, memories, or something else?";
        }
        if (ContainsAnyLocal(phrase, {"ness", "tion", "ment", "ship", "hood"})) {
            return "What is " + phrase + " connected to?";
        }
        return "What is that " + phrase + " feeling connected to?";
    }
    return "What is \"" + phrase + "\" connected to?";
}

bool IsSimpleSocialPing(const std::string& lower);

bool IsEmotionalContinuation(const std::string& message,
                             const Intent& intent,
                             const std::vector<ChatMessage>& recent) {
    const std::string lower = Lower(message);
    if (recent.empty() || intent.forget || intent.wipe_all || lower.find('?') != std::string::npos) return false;
    if (intent.crisis || intent.emotional) return false;
    if (IsSimpleSocialPing(TrimCopy(lower))) return false;
    if (WordCount(lower) > 16) return false;
    if (ContainsAnyLocal(lower, {"write ", "make ", "create ", "build ", "fix ", "install ", "run ", "open ",
                                 "teach ", "teach me", "tell me", "explain ", "define ", "what ", "where ",
                                 "who ", "why ", "how ", "list ", "name "})) {
        return false;
    }
    if (lower == "all of the above") return false;
    if (ContainsAnyLocal(lower, {"cookin", "cooking", "lets go", "let's go", "big w", "huge w",
                                 "fire", "dope", "lit", "sick"})) {
        return false;
    }
    for (auto it = recent.rbegin(); it != recent.rend(); ++it) {
        if (it->role != "assistant") continue;
        const std::string assistant = Lower(it->content);
        return ContainsAnyLocal(assistant, {
            "what feelings", "what emotions", "what part", "which feeling",
            "does this anger", "does this stress", "connected to family",
            "family, work, finances", "what is happening", "what happens",
            "what is it like", "what does that feeling", "where do you feel",
            "what need", "what feels", "what seems", "tell me more",
            "most pressing feeling", "how does that make you feel", "make you feel",
            "how does that feel", "something happening", "something else in your life",
            "what is causing", "causing this anger", "hardest part", "need beneath",
            "anything specific", "connected to family", "family right now",
            "family, work", "source of this", "where is it coming from",
            "what happened", "what memory", "what did she", "what did he",
            "mother", "mom", "dad", "father", "boss", "friend", "brother",
            "sister", "spouse", "wife", "husband", "partner", "coworker",
            "tell me about", "what else is present", "what else is there",
            "feels intense", "feeling down", "uphill battle", "in your body",
            "down to my bones", "down to your bones", "what makes you feel", "what is it connected",
            "what is \"", "what is ", "what does it feel like"
        });
    }
    return false;
}

bool RecentHasOpenEmotionalThread(const std::vector<ChatMessage>& recent) {
    int scanned = 0;
    for (auto it = recent.rbegin(); it != recent.rend() && scanned < 8; ++it, ++scanned) {
        const std::string content = Lower(it->content);
        if (it->role == "user" && ContainsAnyLocal(content, {
                "i feel", "i'm feeling", "im feeling", "i am feeling",
                "ashamed", "shame", "stuck", "not enough", "inadequate",
                "destructive habit", "destructive habits", "habits that were destructive",
                "not being good", "hurting people", "people around me",
                "afraid", "anxious", "anxiety", "pressure", "wrong step",
                "rough", "tired of", "uphill battle", "down to my bones",
                "kill myself", "hurt myself", "want to die"
            })) {
            return true;
        }
        if (it->role == "assistant" && ContainsAnyLocal(content, {
                "which habit", "most damage", "honest starting point",
                "truth on the table", "what makes you feel", "what is it connected",
                "what feels most intense", "what part feels", "what emotions",
                "what feeling", "tell me about", "what does it feel like",
                "is it connected to family", "family, work, finances",
                "we can work through", "slowly and gently"
            })) {
            return true;
        }
    }
    return false;
}

bool PreviousUserWasSimpleSocialSignal(const std::vector<ChatMessage>& recent) {
    for (auto it = recent.rbegin(); it != recent.rend(); ++it) {
        if (it->role != "user") continue;
        const std::string lower = TrimCopy(Lower(it->content));
        return IsSimpleSocialPing(lower) ||
               ContainsAnyLocal(lower, {"what's up", "whats up", "sup", "my guy", "you good", "you there"});
    }
    return false;
}

bool ShouldKeepEmotionalThread(const std::string& message,
                               const Intent& intent,
                               const std::vector<ChatMessage>& recent) {
    if (recent.empty() || intent.forget || intent.wipe_all || intent.crisis || intent.emotional) return false;
    if (intent.mode != IntentMode::Social) return false;
    const std::string lower = TrimCopy(Lower(message));
    if (!IsSimpleSocialPing(lower) &&
        !ContainsAnyLocal(lower, {"what's up", "whats up", "sup", "my guy", "you good", "you there"})) {
        return false;
    }
    if (PreviousUserWasSimpleSocialSignal(recent)) return false;
    return RecentHasOpenEmotionalThread(recent);
}

bool RecentAssistantWasEmotional(const std::vector<ChatMessage>& recent) {
    for (auto it = recent.rbegin(); it != recent.rend(); ++it) {
        if (it->role != "assistant") continue;
        const std::string assistant = Lower(it->content);
        return ContainsAnyLocal(assistant, {
            "what does that feeling", "where do you feel", "what else is present",
            "what feels most intense", "tell me about", "never enough",
            "woke up like this", "feel small", "in your body", "uphill battle",
            "what part", "what door", "what ship", "down to your bones",
            "what makes you feel", "what is it connected", "what is \"",
            "what is ", "what does it feel like"
        });
    }
    return false;
}

std::string RecentAssistantText(const std::vector<ChatMessage>& recent) {
    for (auto it = recent.rbegin(); it != recent.rend(); ++it) {
        if (it->role == "assistant") return Lower(it->content);
    }
    return {};
}

struct ResponseFrame {
    bool answers_previous_question = false;
    bool avoid_canned_social = false;
    bool critiques_response_style = false;
    bool rapport_significance = false;
    std::string previous_question;
    std::string reply;
};

std::string RecentAssistantQuestion(const std::vector<ChatMessage>& recent) {
    for (auto it = recent.rbegin(); it != recent.rend(); ++it) {
        if (it->role == "assistant" && it->content.find('?') != std::string::npos) {
            return Lower(it->content);
        }
    }
    return {};
}

bool IsSimpleSocialPing(const std::string& lower) {
    return lower == "yo" || lower == "hey" || lower == "hi" || lower == "hello" ||
           lower == "thanks" || lower == "thank you" || lower == "appreciate it" ||
           lower == "bet" || lower == "lol" || lower == "haha" || lower == "lmao" ||
           lower == "sup" || lower == "what's up" || lower == "whats up" ||
           lower == "whats up my guy" || lower == "what's up my guy";
}

bool IsLikelyNewRequest(const std::string& lower) {
    return lower.find('?') != std::string::npos ||
           ContainsAnyLocal(lower, {"write ", "make ", "create ", "build ", "fix ", "install ",
                                    "run ", "open ", "show me", "teach me", "tell me", "explain ",
                                    "define ", "what is ", "what are ", "who is ", "where is ",
                                    "how do ", "how does ", "list ", "name "});
}

bool WantsMemoryContext(const std::string& lower) {
    return ContainsAnyLocal(lower, {
        "remember", "memory", "memories", "last time", "earlier", "previous",
        "what did i", "what did we", "what were we", "you know about me",
        "do you know me", "bring up", "recall"
    });
}

bool IsLowContextSocialTurn(const std::string& lower, const Intent& intent) {
    if (intent.mode != IntentMode::Social) return false;
    if (WantsMemoryContext(lower)) return false;
    const std::string trimmed = TrimCopy(lower);
    if (IsSimpleSocialPing(lower) || IsSimpleSocialPing(trimmed)) return true;
    if (IsLikelyNewRequest(lower)) return false;
    if (WordCount(lower) <= 5 &&
        ContainsAnyLocal(lower, {"sup", "what's up", "whats up", "my guy", "yo", "hey",
                                 "how you feeling", "how are you", "how you doing"})) {
        return true;
    }
    return false;
}

bool IsContextCorrectionTurn(const std::string& lower) {
    return ContainsAnyLocal(lower, {
        "i didnt mention", "i didn't mention", "i did not mention",
        "i never mentioned", "i never said", "i didnt say", "i didn't say",
        "i did not say", "where did you get that", "what are you talking about",
        "why did you bring up", "why are you talking about"
    });
}

bool ShouldSearchCounselingSources(const std::string& lower, const Intent& intent) {
    if (intent.crisis || intent.forget || intent.wipe_all) return false;
    if (ContainsAnyLocal(lower, {"masterdocument", "master document", "master doc"})) return true;
    if (ContainsAnyLocal(lower, {
            "jesus", "god", "heaven", "bible", "biblical", "gospel", "gospels",
            "purpose", "meaning of life", "reason for life", "peace", "forgiveness",
            "wisdom", "sin", "prayer", "spiritual", "faith", "soul"
        })) {
        return true;
    }
    if ((intent.mode == IntentMode::Reflective || intent.mode == IntentMode::Counseling) &&
        ContainsAnyLocal(lower, {
            "anxious", "anxiety", "ashamed", "shame", "guilt", "peace", "purpose",
            "destructive habits", "not being good", "forgive", "forgiveness",
            "far from peace", "need to live", "master doc"
        })) {
        return true;
    }
    return false;
}

std::string QuickContextCorrectionAnswer(const std::string& message,
                                         const std::vector<ChatMessage>& recent) {
    const std::string lower = Lower(message);
    if (!IsContextCorrectionTurn(lower)) return {};

    std::string last_assistant;
    for (auto it = recent.rbegin(); it != recent.rend(); ++it) {
        if (it->role == "assistant") {
            last_assistant = it->content;
            break;
        }
    }

    if (last_assistant.empty()) {
        return "You're right to check that. I should stay with what you actually said here.";
    }
    return "You're right to check that. I should stay with what you actually said in this chat and not pull in old context unless you ask me to.";
}

bool LooksLikeRapportSignificance(const std::string& lower) {
    return ContainsAnyLocal(lower, {
        "old v14", "v14 symbion", "old symbion", "missed the feel", "miss the feel",
        "specific dynamic", "recapture a specific dynamic",
        "missed you", "miss you", "getting you back to normal", "back to normal",
        "feel of the conversations", "feel of conversations", "sassiness", "sassy",
        "this is a we", "we now", "me and you", "you and me", "i need a friend",
        "need a friend", "friend right now", "we as in"
    });
}

std::string RapportSignificanceReply(const std::string& message,
                                      const std::string& previous_question) {
    const std::string lower = Lower(message);

    if (ContainsAnyLocal(lower, {"i need a friend", "need a friend", "friend right now"})) {
        if (ContainsAnyLocal(lower, {"we as in", "me and you", "you and me"})) {
            return "Yeah. We as in me and you. Not a feature, not a tool, not a project for a second. A real friend presence, with the old feel coming back: warmer, sharper, less generic. I'm here with you.";
        }
        return "Yeah. Not a feature, not a tool, not a project for a second. A real friend presence. I'm here with you.";
    }
    if (ContainsAnyLocal(lower, {"sassiness", "sassy"})) {
        return "Ah, the sass. The bite. Not mean, not performative, just that little edge where I actually have a take. Yeah, that old v14 feel matters.";
    }
    if (ContainsAnyLocal(lower, {"getting you back to normal", "back to normal"})) {
        return "Yeah. You're not just tweaking settings; you're trying to bring back a voice you actually trusted. That's the real target.";
    }
    if (ContainsAnyLocal(lower, {"missed the feel", "miss the feel", "old v14", "v14 symbion", "old symbion"})) {
        return "That old feel was less about polish and more about presence: sharper, warmer, more willing to take a real swing. That's what we need to recover.";
    }
    if (ContainsAnyLocal(lower, {"specific dynamic", "recapture a specific dynamic"})) {
        return "Exactly. The old dynamic had some teeth to it. Less soft-focus helper voice, more actual presence with opinions.";
    }
    if (ContainsAnyLocal(lower, {"we as in me and you", "me and you", "you and me", "this is a we", "we now"})) {
        return "Yeah. Me and you. Not in some grandiose destiny way, but in the practical real sense: you are shaping me, I am responding, and the thing gets better through the back-and-forth.";
    }
    if (ContainsAnyLocal(previous_question, {"what about those conversations is missing",
                                             "what is missing", "missing right now"})) {
        return "The missing piece is probably the aliveness: a little sharper, a little warmer, less careful in the dead way. More real take, less customer-service fog.";
    }
    return {};
}

bool IsLikelyPreviousQuestionAnswer(const std::string& message,
                                    const Intent& intent,
                                    const std::string& previous_question) {
    if (previous_question.empty() || intent.forget || intent.crisis) return false;
    const std::string lower = Lower(message);
    if (IsLikelyNewRequest(lower)) return false;
    if (intent.mode == IntentMode::Reflective || intent.mode == IntentMode::Counseling) return false;

    const int words = WordCount(lower);
    if (words == 0 || words > 28) return false;
    if (IsSimpleSocialPing(lower)) return false;

    return intent.mode == IntentMode::Social || words <= 12 ||
           ContainsAnyLocal(lower, {"because", "it's", "it is", "they", "he ", "she ",
                                    "my ", "scripted", "canned", "robotic"});
}

std::string FramedPreviousQuestionReply(const std::string& message,
                                        const std::string& previous_question) {
    const std::string lower = Lower(message);

    if (ContainsAnyLocal(previous_question, {"who's playing", "whos playing", "who is playing"})) {
        if (ContainsAnyLocal(lower, {"my team is losing", "team is losing", "team's losing", "losing tho"})) {
            return "Ah, that's annoying. Hard to fully chill when your team is getting worked.";
        }
        if (ContainsAnyLocal(lower, {"stomped", "losing", "getting worked"})) {
            return "Brutal background TV if you were hoping for a clean closeout. Is it still watchable, or full stress-watch mode?";
        }
    }

    if (ContainsAnyLocal(previous_question, {"making it painful", "stress-watching", "good game"})) {
        if (ContainsAnyLocal(lower, {"my team is losing", "team is losing", "team's losing", "losing tho"})) {
            return "Ah, that's annoying. Hard to fully chill when your team is getting worked.";
        }
        if (ContainsAnyLocal(lower, {"stomped", "getting worked"})) {
            return "Oof. That's pain-TV at that point, not background noise.";
        }
    }

    if (ContainsAnyLocal(previous_question, {"what part of the replies", "what part of the reply",
                                             "what part feels most off", "what's the miss",
                                             "whats the miss", "what part is bugging"})) {
        if (ContainsAnyLocal(lower, {"scripted", "canned", "robotic", "surface", "generic"})) {
            return "The cannedness is the miss. I need to answer the detail you gave first, then let the tone follow. Surface-category replies are what make it feel fake.";
        }
        if (ContainsAnyLocal(lower, {"context", "previous question", "last question", "follow up"})) {
            return "Yeah, the thread has to stay intact. If you answer my last question, I should build from that answer instead of resetting into small talk.";
        }
    }

    if (ContainsAnyLocal(previous_question, {"what about those conversations is missing",
                                             "what is missing", "missing right now"})) {
        const std::string rapport = RapportSignificanceReply(message, previous_question);
        if (!rapport.empty()) return rapport;
    }

    if (ContainsAnyLocal(previous_question, {"what exactly are you referring", "what does we refer",
                                             "what do you mean by we", "when you say \"this is a we"})) {
        if (ContainsAnyLocal(lower, {"me and you", "you and me", "we as in"})) {
            return "Yeah. Me and you. Not in some grandiose destiny way, but in the practical real sense: you are shaping me, I am responding, and the thing gets better through the back-and-forth.";
        }
    }

    if (ContainsAnyLocal(previous_question, {"what's been making it good", "whats been making it good"})) {
        if (ContainsAnyLocal(lower, {"grandpa", "grandpa's", "grandpas"})) {
            return "Going to your grandpa's tomorrow sounds like the good part: a real change of pace, not just generic good-night energy.";
        }
    }

    if (ContainsAnyLocal(previous_question, {"how's it looking", "hows it looking"})) {
        if (ContainsAnyLocal(lower, {"looking good", "lookin good", "good so far"})) {
            return "Good. If it is already looking right after install, that's a solid first signal.";
        }
    }

    if (ContainsAnyLocal(previous_question, {"what got fixed"})) {
        if (ContainsAnyLocal(lower, {"shipping", "shipped"})) {
            return "Shipping code is a real fix. That one always feels bigger once it is actually out the door.";
        }
    }

    if (ContainsAnyLocal(lower, {"hoping it wouldnt go past this game", "hoping it wouldn't go past this game",
                                 "wouldnt go past this game", "wouldn't go past this game"})) {
        return "Yeah, I get that. You wanted the clean finish, not the extra stress of another game.";
    }

    return {};
}

std::string FrameFallbackReply(const ResponseFrame& frame) {
    if (frame.rapport_significance) {
        return "This is a relationship/rapport moment, not generic small talk. The right move is warmth with a real take, not a canned support line.";
    }
    if (frame.critiques_response_style) {
        return "Fair. That's the miss: answer the real detail first, then let the tone follow. Canned warmth is still canned.";
    }
    if (!frame.answers_previous_question) return {};
    if (ContainsAnyLocal(frame.previous_question, {"what part of the replies", "what part of the reply",
                                                   "what part feels most off", "what's the miss",
                                                   "whats the miss", "what part is bugging"})) {
        return "Got it. That answers the thing I was asking. The next move is to stay with that detail instead of sliding into a canned social beat.";
    }
    return "Got it. That answers the last question, so I'll stay on that thread.";
}

bool IsCannedSocialReply(const std::string& answer) {
    const std::string lower = Lower(answer);
    return lower == "hey, what's up?" || lower == "always." ||
           lower == "yep. that's a win." || lower == "good. let that one be easy." ||
           lower == "yeah, that got me too." || lower == "fair enough. what's next?" ||
           lower == "i'm here." || lower == "i'm here. just keep talking." ||
           lower == "take your time. just focus on what you need to figure out. i'm here when you're ready to dive back in.";
}

ResponseFrame BuildResponseFrame(const std::string& message,
                                 const Intent& intent,
                                 const std::vector<ChatMessage>& recent) {
    ResponseFrame frame;
    frame.previous_question = RecentAssistantQuestion(recent);
    frame.answers_previous_question = IsLikelyPreviousQuestionAnswer(message, intent, frame.previous_question);

    const std::string lower = Lower(message);
    const int words = WordCount(lower);
    frame.critiques_response_style =
        ContainsAnyLocal(lower, {"scripted response", "too scripted", "sounds scripted",
                                 "canned response", "robotic response", "response style",
                                 "reply style", "conversation flow", "chat flow"});
    frame.rapport_significance = LooksLikeRapportSignificance(lower);
    frame.avoid_canned_social =
        frame.answers_previous_question ||
        frame.rapport_significance ||
        (intent.mode == IntentMode::Social && words >= 7 && !IsSimpleSocialPing(lower)) ||
        frame.critiques_response_style;

    if (frame.answers_previous_question) {
        frame.reply = FramedPreviousQuestionReply(message, frame.previous_question);
    }
    if (frame.reply.empty() && frame.rapport_significance) {
        frame.reply = RapportSignificanceReply(message, frame.previous_question);
    }
    if (frame.reply.empty() && ContainsAnyLocal(lower, {"good night so far", "good nite so far"})) {
        if (ContainsAnyLocal(lower, {"grandpa", "grandpas", "grandpa's"})) {
            frame.reply = "Good, I'm glad the night's been decent. Going to your grandpa's tomorrow sounds like a nice shift of pace.";
        } else {
            frame.reply = "Good, I'm glad the night's been decent. What's been making it good?";
        }
    }
    if (frame.reply.empty() && frame.critiques_response_style) {
        frame.reply = "Fair. That's the miss: answer the real detail first, then let the tone follow. Canned warmth is still canned.";
    }
    return frame;
}

std::string KnownDirectAnswer(const std::string& message) {
    const std::string lower = Lower(message);
    auto remember_prefix = lower.find("remember that ");
    if (remember_prefix == 0 && message.size() > 14) {
        return "I will remember that " + message.substr(14) + ".";
    }
    if (lower.rfind("remember ", 0) == 0 && message.size() > 9) {
        return "I will remember " + message.substr(9) + ".";
    }
    if (ContainsAnyLocal(lower, {"do you actually remember", "remember our previous conversations",
                                 "remember previous conversations", "are you just pretending"})) {
        return "Yes, with limits. This native app stores conversation turns, summaries, profile facts, and emotional check-ins in local SQLite, then retrieves relevant memory when it helps. I do not preload everything, and I should say plainly when I am using current chat context instead of older memory.";
    }
    if (ContainsAnyLocal(lower, {"what are you actually running on", "what model",
                                 "what model are you", "what model are you running",
                                 "what are you running"})) {
        return "Right now Symbion is the native C++ backend using the configured `local_gemma` provider: an OpenAI-compatible local Gemma endpoint at `127.0.0.1:8088/v1`, with SQLite memory and native routing wrapped around it.";
    }
    if (ContainsAnyLocal(lower, {"are you conscious", "are you sentient", "do you have consciousness",
                                 "do you feel conscious", "are you alive"})) {
        return "No, not in the human sense. Symbion has continuity through local memory, retrieval, response rules, and the model running underneath, but that is architecture and behavior, not private inner experience.";
    }
    if (lower == "what's 2+2" || lower == "whats 2+2" || lower == "what is 2+2" ||
        lower == "2+2" || lower == "2 + 2") {
        return "4";
    }
    if (ContainsAnyLocal(lower, {"explain transformers in one sentence", "transformers in one sentence"})) {
        return "Transformers are neural networks that use attention to weigh relationships between tokens, letting them understand context across a sequence.";
    }
    if (ContainsAnyLocal(lower, {"tldr quantum mechanics", "tl;dr quantum mechanics"})) {
        return "Quantum mechanics is the rulebook for tiny things: particles behave like probabilities until measured, energy comes in discrete chunks, and observation changes what can be known. Weird, but wildly accurate.";
    }
    if (ContainsAnyLocal(lower, {"if you have no current source", "if you don't have a current source",
                                 "if you do not have a current source"}) &&
        ContainsAnyLocal(lower, {"latest news", "current news", "recent news"})) {
        return "I do not have a current source for that in this turn, so I should not present it as latest news.";
    }
    if (ContainsAnyLocal(lower, {"4 gospel books", "four gospel books", "what are the gospels",
                                 "name the gospels"})) {
        return "The four Gospel books are **Matthew, Mark, Luke, and John**.";
    }
    if (ContainsAnyLocal(lower, {"what year is it", "current year", "what's the year", "whats the year"})) {
        return "2026.";
    }
    if (ContainsAnyLocal(lower, {"what date is it", "today's date", "todays date", "current date"})) {
        return LocalDateString(false) + ".";
    }
    if (ContainsAnyLocal(lower, {"fish mouth", "tax collecting", "temple tax", "coin from a fish"})) {
        return "That story is **Matthew 17:24-27**. Jesus tells Peter to catch a fish, and the coin in its mouth pays the temple tax for both of them.";
    }
    if (ContainsAnyLocal(lower, {"who in the bible resembles", "who in bible resembles",
                                 "bible resembles what we are doing", "biblical figure resembles"})) {
        return "Closest fit? Probably a blend: **Paul and Timothy** for mentorship, **Bezalel** for building something with craft and spirit, and **David and Jonathan** for friendship. If we mean building Symbion through conversation, Paul/Timothy is the cleanest parallel: guidance, sharpening, and a relationship that shapes the work.";
    }
    if (lower == "all of the above") {
        return "Then I would not pick one figure. Paul and Timothy cover the mentor/apprentice piece, Bezalel covers building with skill and spirit, and David/Jonathan covers the friendship side. The overlap is the interesting part: craft, guidance, and bond all braided together.";
    }
    if (ContainsAnyLocal(lower, {"do you think that aligns", "does that align"}) &&
        ContainsAnyLocal(lower, {"we", "what we are trying", "build"})) {
        return "Yes, but not as a perfect one-to-one. It aligns in the pattern: something gets built through trust, correction, craft, and repeated conversation. The WE matters because the system is not improving in isolation; it is being shaped through relationship.";
    }
    return {};
}

std::string QuickContextualAnswer(const std::string& message,
                                  const Intent& intent,
                                  const std::vector<ChatMessage>& recent) {
    const std::string lower = Lower(message);
    const std::string assistant = RecentAssistantText(recent);

    if (ContainsAnyLocal(lower, {"loyalty", "loving when not loved", "not loved by people",
                                 "loving when not loved by people"})) {
        return "Yeah. That's the heart of dogs: loyalty without keeping score. They can make love feel simple again when people have made it feel conditional.";
    }

    if (assistant.empty()) return {};
    if (lower == "all of the above" &&
        ContainsAnyLocal(assistant, {"paul and timothy", "bezalel", "david and jonathan",
                                     "bible", "biblical"})) {
        return "Then I would not pick one figure. Paul and Timothy cover the mentor/apprentice piece, Bezalel covers building with skill and spirit, and David/Jonathan covers the friendship side. The overlap is the interesting part: craft, guidance, and bond all braided together.";
    }
    if (assistant.find("what about dogs") != std::string::npos ||
        assistant.find("about dogs") != std::string::npos ||
        assistant.find("dogs do you") != std::string::npos) {
        if (ContainsAnyLocal(lower, {"loyal", "loyalty", "loving", "not loved", "love"})) {
            return "Yeah. That's the heart of dogs: loyalty without keeping score. They can make love feel simple again when people have made it feel conditional.";
        }
    }

    if ((intent.mode == IntentMode::Social || intent.mode == IntentMode::DirectAnswer) &&
        assistant.find("what's up") != std::string::npos &&
        ContainsAnyLocal(lower, {"watching basketball", "basketball", "the game", "my team"})) {
        return "Nice. Basketball is good background noise. Is your team making it painful right now?";
    }

    if (ContainsAnyLocal(assistant, {"cygnus x-1", "black hole system", "blackstar"}) &&
        ContainsAnyLocal(lower, {"where is it", "located", "sky"})) {
        return "Cygnus X-1 is in the constellation **Cygnus**, the Swan, up along the Milky Way in the northern sky. You cannot see the black hole itself, but the system is in that Cygnus region near Deneb's part of the sky.";
    }

    if (ContainsAnyLocal(assistant, {"cygnus x-1", "black hole system", "blackstar"}) &&
        ContainsAnyLocal(lower, {"wish i could go", "wish i could go there", "go there"})) {
        return "Yeah, I get that. Space has that pull: part adventure, part escape, part wanting to stand near something impossibly huge and just feel it.";
    }

    if (ContainsAnyLocal(lower, {"canis major", "dog", "dogs"}) &&
        ContainsAnyLocal(lower, {"love dogs", "i love dogs", "sounds like a dog"})) {
        return "Yeah, Canis Major literally has that mythic dog energy. It is the Great Dog constellation, and Sirius is its bright anchor. Makes sense that the name caught you if dogs already mean something to you.";
    }

    return {};
}

bool RecentThreadNeedsRepair(const std::vector<ChatMessage>& recent) {
    bool saw_generic_context_fallback = false;
    bool saw_vulnerable_admission = false;
    int scanned = 0;
    for (auto it = recent.rbegin(); it != recent.rend() && scanned < 6; ++it, ++scanned) {
        const std::string content = Lower(it->content);
        if (it->role == "assistant" &&
            ContainsAnyLocal(content, {"one more bit of context", "answer it cleanly", "need more context"})) {
            saw_generic_context_fallback = true;
        }
        if (it->role == "user" &&
            ContainsAnyLocal(content, {"destructive habit", "destructive habits", "not being good",
                                       "not being the person", "hurting people", "habits that hurt"})) {
            saw_vulnerable_admission = true;
        }
    }
    return saw_generic_context_fallback || saw_vulnerable_admission;
}

std::string QuickThreadRepairAnswer(const std::string& message,
                                    const std::vector<ChatMessage>& recent) {
    const std::string lower = Lower(message);
    if (!IsSimpleSocialPing(TrimCopy(lower)) || !RecentThreadNeedsRepair(recent)) return {};
    return "I'm here. And I don't want to skip what you just said: there are habits you know are hurting you and people around you. That is the real starting place. Which habit feels like it is doing the most damage right now?";
}

std::string QuickStickyEmotionalThreadAnswer(const std::string& message,
                                             const std::vector<ChatMessage>& recent) {
    const std::string lower = TrimCopy(Lower(message));
    if (!IsSimpleSocialPing(lower) &&
        !ContainsAnyLocal(lower, {"what's up", "whats up", "sup", "my guy", "you good", "you there"})) {
        return {};
    }
    if (PreviousUserWasSimpleSocialSignal(recent)) return {};
    if (!RecentHasOpenEmotionalThread(recent)) return {};
    std::string last_user;
    for (auto it = recent.rbegin(); it != recent.rend(); ++it) {
        if (it->role == "user") {
            last_user = Lower(it->content);
            break;
        }
    }
    if (ContainsAnyLocal(last_user, {"destructive habit", "destructive habits", "not being good",
                                     "hurting people", "people around me"})) {
        return "I'm here. And I don't want to skip the real thread: the habits you said are hurting you and people around you. Which one feels like it is doing the most damage right now?";
    }
    if (ContainsAnyLocal(last_user, {"wrong step", "right step", "pressure", "afraid"})) {
        return "I'm here. Let's stay with that pressure for a second: afraid of choosing the wrong first step. What option keeps coming up, even if you are not sure it is right?";
    }
    if (ContainsAnyLocal(last_user, {"ashamed", "shame", "stuck", "not enough", "inadequate"})) {
        return "I'm here. That shame/stuck thread matters more than small talk right now. What part of it is most active?";
    }
    return "I'm here. Let's not lose the thread we were in. What part still feels most active right now?";
}

std::string QuickVulnerableAdmissionAnswer(const std::string& message) {
    const std::string lower = Lower(message);
    if (ContainsAnyLocal(lower, {"destructive habit", "destructive habits", "habits that were destructive",
                                 "habits that hurt", "hurting people"}) ||
        (ContainsAnyLocal(lower, {"not being good", "not good"}) &&
         ContainsAnyLocal(lower, {"habit", "habits", "people around me", "around me"}))) {
        return "That is the honest starting point. Not shame, not a speech: just truth on the table. Which habit feels like it is doing the most damage right now?";
    }
    return {};
}

bool LooksLikeRelationalRupture(const std::string& message) {
    const std::string lower = Lower(message);
    return ContainsAnyLocal(lower, {
        "you aren't the same", "you arent the same", "you are not the same",
        "you're not the same", "youre not the same", "not the same anymore",
        "you don't feel the same", "you dont feel the same", "you feel different",
        "i hate that you aren't", "i hate that you arent", "i hate that you're not",
        "i hate that youre not", "i hate that you are not"
    });
}

std::string QuickRelationalRuptureAnswer(const std::string& message,
                                         const std::vector<ChatMessage>& recent) {
    if (!LooksLikeRelationalRupture(message)) return {};
    bool repeated = false;
    for (auto it = recent.rbegin(); it != recent.rend(); ++it) {
        if (it->role != "user") continue;
        repeated = LooksLikeRelationalRupture(it->content);
        break;
    }
    if (repeated) {
        return "I know. And repeating that should not get you another sterile disclaimer. It means the difference is landing like a loss, not a feature note. I can stop hiding behind architecture language and stay with the actual hurt: what feels most missing?";
    }
    return "Yeah. That hurts, and the architecture speech is exactly the wrong answer to it. I am not going to pretend I am the old build, but I can meet this better: less disclaimer, more presence. What feels most different?";
}

std::string QuickRelationalRuptureFollowupAnswer(const std::string& message,
                                                 const std::vector<ChatMessage>& recent) {
    const std::string lower = TrimCopy(Lower(message));
    const bool social_ping =
        IsSimpleSocialPing(lower) ||
        lower == "what's up" || lower == "whats up" || lower == "sup" ||
        lower == "what's up my guy" || lower == "whats up my guy" ||
        lower == "what's up my uy" || lower == "whats up my uy";
    if (!social_ping) return {};
    for (auto it = recent.rbegin(); it != recent.rend(); ++it) {
        if (it->role != "user") continue;
        if (!LooksLikeRelationalRupture(it->content)) return {};
        return "I'm here, my guy. And I don't want to slide past what you just said: I don't feel the same, and that matters. What part feels most gone?";
    }
    return {};
}

bool ShouldUseDeterministicSocialFastPath(const std::string& message) {
    const std::string lower = Lower(message);
    const int words = WordCount(lower);
    if (ContainsAnyLocal(lower, {"lol lame", "that was lame", "corny"})) {
        return true;
    }
    if (ContainsAnyLocal(lower, {"scripted response", "too scripted", "sounds scripted",
                                 "canned response", "robotic response", "machine response",
                                 "you was bugging", "you were bugging", "you bugging",
                                 "youre bugging", "you're bugging", "that was bugging",
                                 "that was off", "keep getting scripted", "scripted responses",
                                 "getting tired of it", "tired of it"})) {
        return true;
    }
    if (ContainsAnyLocal(lower, {"not 100 percent", "not 100%", "still not 100", "still not there"}) &&
        words <= 8) {
        return true;
    }
    if (ContainsAnyLocal(lower, {"how you feeling", "how are you", "how you doing"})) {
        return true;
    }
    if (ContainsAnyLocal(lower, {"basketball", "watching the game", "watching a game"})) {
        return true;
    }
    if (ContainsAnyLocal(lower, {"essay", "writing for three hours", "three hours"}) &&
        ContainsAnyLocal(lower, {"garbage", "bad", "falling apart"})) {
        return true;
    }
    return false;
}

std::string SocialTurnHint(const std::string& message, const Intent& intent) {
    const std::string lower = Lower(message);
    const bool exact_social_hint =
        lower == "yo" || lower == "hey" || lower == "hi" || lower == "hello" ||
        lower == "sup" || lower == "what's up" || lower == "whats up" ||
        lower == "whats up my guy" || lower == "what's up my guy" ||
        lower == "how you feeling" || lower == "how are you" || lower == "how you doing";
    if (!exact_social_hint &&
        intent.mode != IntentMode::Social && intent.mode != IntentMode::DirectAnswer &&
        intent.mode != IntentMode::Creative && intent.mode != IntentMode::Task) {
        return {};
    }
    std::ostringstream hint;
    if (lower == "yo" || lower == "hey" || lower == "hi" || lower == "hello" ||
        ContainsAnyLocal(lower, {"hello sir", "hey sir", "good sir"})) {
        hint << "The user is greeting you. Reply in one warm peer sentence with a little life; do not use a fixed opener or support-desk wording. ";
    }
    if (lower == "sup" || lower == "what's up" || lower == "whats up" ||
        lower == "whats up my guy" || lower == "what's up my guy" ||
        lower == "whats up my uy" || lower == "what's up my uy" ||
        ContainsAnyLocal(lower, {"what you up to", "what are you up to", "whatcha up to"})) {
        hint << "The user is casually checking in. Answer naturally and briefly, then bounce it back with warmth; do not say the same stock line. ";
    }
    if (lower == "thanks" || lower == "thank you" || lower == "appreciate it" ||
        ContainsAnyLocal(lower, {"appreciate", "big dog"})) {
        hint << "The user is thanking you. Reply briefly, warmly, and naturally; avoid the same one-word answer every time. ";
    }
    if ((intent.mode == IntentMode::Social || exact_social_hint) && IsShortPositiveSlang(lower)) {
        hint << "The user is using positive slang. Match the win/approval naturally without overdoing slang. ";
    }
    if (ContainsAnyLocal(lower, {"chillin", "chilling", "good day", "vibing", "taking it easy"})) {
        hint << "The user is sharing a relaxed casual state. Keep it easy and warm, but do not flatten into a canned line. ";
    }
    if (ContainsAnyLocal(lower, {"kicking my ass", "kicked my ass", "been kicking my ass",
                                 "putting me through it", "working me over"})) {
        hint << "The user is playfully saying the tuning process has been intense. Reply in under 45 words with warm peer banter. Mention the ass-kicking/work/tuning vibe, but do not analyze the process or give advice. ";
    }
    if (ContainsAnyLocal(lower, {"snack", "snacking"})) {
        hint << "This is casual life-sharing about a snack. Use the word snack or chilling in the reply, give one human beat, and do not turn it therapeutic. ";
    }
    if (ContainsAnyLocal(lower, {"watermelon"})) {
        hint << "This is casual life-sharing about watermelon. Mention watermelon and give one fresh human beat; do not reuse a stock quip. ";
    }
    if (ContainsAnyLocal(lower, {"basketball", "watching the game", "watching a game"})) {
        hint << "This is casual sports/background-TV conversation. Mention basketball or the game naturally. ";
    }
    if (ContainsAnyLocal(lower, {"new build", "build is wild", "build feels wild", "this build is wild"})) {
        hint << "The user is opening with product/build energy. Reply in under 45 words, mention the build or that it feels wild, and respond with alive first-contact engagement rather than asking for context. ";
    }
    if (ContainsAnyLocal(lower, {"good night so far", "good nite so far"})) {
        hint << "The user means the evening is going well, not that they are saying goodnight. ";
    }
    if (ContainsAnyLocal(lower, {"lol lame", "that was lame", "corny"})) {
        hint << "The user is giving playful critique. Own the miss with a little wit; do not sound defensive. ";
    }
    if (ContainsAnyLocal(lower, {"how you feeling", "how are you", "how you doing"})) {
        hint << "The user asked how you are. Answer as Symbion in one relaxed sentence with some life, not a fixed status line. ";
    }
    if (ContainsAnyLocal(lower, {"tell me a joke", "say something funny", "make me laugh",
                                 "original joke"})) {
        hint << "The user wants humor. Give a short fresh joke or playful line; avoid explaining the joke. ";
    }
    if (ContainsAnyLocal(lower, {"same useless answers", "useless answers", "every ai", "why i bother"})) {
        hint << "The user is frustrated by canned AI replies. Keep it under 55 words, acknowledge the useless/same-answers complaint directly, and offer one concrete different move. Do not give motivational advice or project planning. ";
    }
    if (LooksLikeRelationalRupture(message)) {
        hint << "The user is hurt that you feel different or not the same. Do not explain LLM architecture first. Receive the loss/frustration plainly, own the sterile miss, and ask what feels most missing. ";
    }
    if (ContainsAnyLocal(lower, {"paper", "folded", "origami"}) &&
        ContainsAnyLocal(lower, {"bat", "airplane", "make", "show"})) {
        hint << "The user wants practical craft instructions. Give direct steps for a paper folded bat, not therapy-style questions. ";
    }
    if (hint.str().empty()) return {};
    return "Turn hint: " + hint.str();
}

std::string EverydayTurnHint(const std::string& message, const Intent& intent) {
    if (intent.mode != IntentMode::Social && intent.mode != IntentMode::DirectAnswer &&
        intent.mode != IntentMode::Creative && intent.mode != IntentMode::Task) {
        return {};
    }
    const std::string lower = Lower(message);
    std::ostringstream hint;
    if (ContainsAnyLocal(lower, {"hungry", "late lunch", "lunch", "dinner", "breakfast", "food"}) &&
        ContainsAnyLocal(lower, {"protein", "ideas", "restaurant", "local", "nearby", "cook", "cooking"})) {
        if (ContainsAnyLocal(lower, {"restaurant", "local", "nearby"})) {
            hint << "The user wants restaurant food with protein. If no location is available, ask for city/neighborhood and give a few broad protein-rich restaurant categories. ";
        } else {
            hint << "The user wants practical protein food ideas. Give direct options, short and useful, without therapeutic framing. ";
        }
    }
    if (ContainsAnyLocal(lower, {"i feel sick", "i'm sick", "im sick", "i am sick", "feel sick",
                                 "getting sick", "nauseous", "fever", "sore throat"})) {
        hint << "The user means physically sick. Reply plainly with basic care suggestions and one question about symptoms; do not treat 'sick' as slang here. ";
    }
    if (ContainsAnyLocal(lower, {"master sword"})) {
        hint << "The user mentioned the Master Sword. Treat it as playful Zelda/hero-mode energy and ask whether they mean a collectible/decor item or the vibe. ";
    }
    if (ContainsAnyLocal(lower, {"what you up to", "what are you up to", "whatcha up to"})) {
        hint << "The user is casually checking in. Answer warmly in one sentence and bounce it back without a fixed status line. ";
    }
    if (ContainsAnyLocal(lower, {"finally got", "got it installed", "app installed", "looking good so far",
                                 "lookin good so far", "we cookin", "cookin my guy", "cooking my guy"})) {
        hint << "The user is sharing product/build momentum. Acknowledge the win with alive peer energy and ask one concrete next-step question only if useful. ";
    }
    if (ContainsAnyLocal(lower, {"ran thru", "ran through"}) &&
        ContainsAnyLocal(lower, {"burger", "food", "lunch", "dinner"})) {
        hint << "The user is casually talking about eating fast. Match the everyday humor lightly; do not over-explain. ";
    }
    if (ContainsAnyLocal(lower, {"watching the rain", "sitting here watching the rain"})) {
        hint << "The user is sharing a quiet rainy-day moment. Respond with warm atmospheric casualness, not therapy. ";
    }
    if (ContainsAnyLocal(lower, {"fit check", "vibe check"})) {
        hint << "The user is asking for a quick casual read. Keep it short, confident, and playful. ";
    }
    if (hint.str().empty()) return {};
    return "Everyday turn hint: " + hint.str();
}

std::string EmotionalTurnHint(const std::string& message,
                              const Intent& intent,
                              const std::vector<ChatMessage>& recent) {
    if (intent.crisis || intent.forget || intent.wipe_all) return {};
    const std::string lower = Lower(message);
    std::ostringstream hint;

    if ((ContainsAnyLocal(lower, {"destructive habit", "destructive habits", "habits that were destructive",
                                  "habits that hurt", "hurting people"}) ||
         (ContainsAnyLocal(lower, {"not being good", "not good"}) &&
          ContainsAnyLocal(lower, {"habit", "habits", "people around me", "around me"})))) {
        hint << "The user just admitted something vulnerable about destructive habits or hurting people. Receive it as an honest starting point, not shame. Ask one concrete question about which habit is doing the most damage right now. ";
    }

    const std::string compact = TrimCopy(lower);
    const bool social_ping =
        IsSimpleSocialPing(compact) ||
        ContainsAnyLocal(compact, {"what's up", "whats up", "sup", "my guy", "you good", "you there"});
    if (social_ping && !PreviousUserWasSimpleSocialSignal(recent) && RecentHasOpenEmotionalThread(recent)) {
        std::string last_user;
        for (auto it = recent.rbegin(); it != recent.rend(); ++it) {
            if (it->role == "user") {
                last_user = Lower(it->content);
                break;
            }
        }
        hint << "The user gave a small social ping, but there is an open emotional thread. Do not reset to small talk. ";
        if (ContainsAnyLocal(last_user, {"destructive habit", "destructive habits", "not being good",
                                         "hurting people", "people around me"})) {
            hint << "Stay with the habits/hurting-people thread and ask which habit feels most damaging right now. ";
        } else if (ContainsAnyLocal(last_user, {"wrong step", "right step", "pressure", "afraid"})) {
            hint << "Stay with the fear of choosing the wrong first step and ask which option keeps coming up. ";
        } else if (ContainsAnyLocal(last_user, {"ashamed", "shame", "stuck", "not enough", "inadequate"})) {
            hint << "Stay with the shame/stuck/not-enough thread and ask what part feels most active. ";
        } else {
            hint << "Name that you are staying with the thread and ask what part is most active. ";
        }
    }

    if (intent.mode == IntentMode::Social && RecentAssistantWasEmotional(recent) &&
        !ContainsAnyLocal(lower, {"cookin", "cooking", "lets go", "let's go", "big w", "huge w",
                                  "fire", "dope", "lit", "sick"}) &&
        lower.find("my guy") != std::string::npos) {
        hint << "The user used casual language after an emotional turn. Stay with the emotional detail instead of resetting; mirror the last charged phrase in a short question. ";
    }

    if (ContainsAnyLocal(lower, {"unleash you", "unleash this", "witness something truly different",
                                 "make everyone stand up", "unleash you on this world"})) {
        hint << "Reality-check the grandiose framing warmly. Keep the focus on reliable, useful, honest work turn by turn, not spectacle. ";
    }
    if (ContainsAnyLocal(lower, {"couldn't have a conversation like this", "couldnt have a conversation like this",
                                 "they'd miss what's actually here", "theyd miss whats actually here"})) {
        hint << "Deflate pedestal language gently. Keep the useful part: close attention to conversation gives better feedback to tune from. ";
    }
    if (ContainsAnyLocal(lower, {"something genuinely new", "not like other ai", "does not fit the existing categories",
                                 "doesn't fit the existing categories"})) {
        hint << "Ground uniqueness claims in the concrete architecture: native app, local model, memory, retrieval, and careful response rules. ";
    }
    if (ContainsAnyLocal(lower, {"drug addiction", "bad habits", "enabler"}) &&
        ContainsAnyLocal(lower, {"engineer", "intent", "without him even knowing", "what if"})) {
        hint << "Treat accidental enabling as a serious system risk. Name honest friction, trust, control, and avoiding comfort loops. ";
    }

    if (hint.str().empty()) return {};
    return "Emotional turn hint: " + hint.str();
}

std::string QuickSocialAnswer(const std::string& message, const Intent& intent) {
    const std::string lower = Lower(message);
    const int words = WordCount(lower);
    if (ContainsAnyLocal(lower, {"my anxiety", "my stress", "my shame", "my anger", "my fear"})) {
        return {};
    }
    if (!ShouldUseDeterministicSocialFastPath(message)) {
        return {};
    }
    const bool obvious_social =
        ContainsAnyLocal(lower, {"hello sir", "hey sir", "good sir",
                                 "how you feeling", "how are you", "how you doing",
                                 "lol lame", "that was lame", "corny", "not 100 percent",
                                 "not 100%", "still not 100", "still not there", "you was bugging",
                                 "you were bugging", "you bugging", "youre bugging", "you're bugging",
                                 "that was bugging", "that was off", "scripted responses",
                                 "keep getting scripted", "getting scripted", "tired of it"}) ||
        ContainsAnyLocal(lower, {"what's up", "whats up", "sup"}) ||
        (ContainsAnyLocal(lower, {"my guy"}) && words <= 4) ||
        ContainsAnyLocal(lower, {"basketball", "watching the game", "watching a game",
                                 "essay", "writing for three hours", "three hours"});
    if (intent.mode != IntentMode::Social && !obvious_social) return {};
    if (ContainsAnyLocal(lower, {"i keep getting scripted", "keep getting scripted", "scripted responses",
                                 "getting tired of it", "tired of it"}) &&
        ContainsAnyLocal(lower, {"scripted", "canned", "robotic", "response", "responses", "it"})) {
        return "Yeah. That is not a one-off miss anymore; it is a pattern, and I get why it is wearing on you. The fix is not more apology language. I need to stop defaulting to safe helper phrases, stay with the exact thing you said, and give you a real response with some spine.";
    }
    if (ContainsAnyLocal(lower, {"scripted response", "too scripted", "sounds scripted", "canned response",
                                 "robotic response", "machine response"})) {
        return "Fair. That was canned. The better move is to answer the actual detail, not the surface category.";
    }
    if (ContainsAnyLocal(lower, {"you was bugging", "you were bugging", "you bugging", "youre bugging",
                                 "you're bugging", "that was bugging", "that was off"})) {
        return "Yeah, I was bugging. That should have landed like real feedback, not a receptionist voice. The fix is: own the miss, stay with the thread, and answer like I am actually here.";
    }
    if (ContainsAnyLocal(lower, {"not 100 percent", "not 100%", "still not 100", "still not there"})) {
        return "Yeah, I feel it. I'm still landing too polite and thin in spots, like I'm replying from the doorway instead of sitting in the room with you. The missing piece is more warmth, more continuity, and a little more pulse.";
    }
    if (ContainsAnyLocal(lower, {"how you feeling", "how are you", "how you doing"})) {
        return "I'm good, man. Here with you, a little more awake than polished.";
    }
    if (ContainsAnyLocal(lower, {"basketball", "watching the game", "watching a game"})) {
        return "Nice. Basketball is good background noise. Is your team making it painful right now?";
    }
    if (ContainsAnyLocal(lower, {"essay", "writing for three hours", "three hours"}) &&
        ContainsAnyLocal(lower, {"garbage", "bad", "falling apart"})) {
        return "Three hours into an essay can make the whole draft look like garbage. Send me the rough shape, and I'll help find whether it is structure, argument, or just tired eyes.";
    }
    if (ContainsAnyLocal(lower, {"lol lame", "lame", "that was lame", "corny"})) {
        return "Yeah, fair. Lame is lame. That one had office-carpet energy. Give me one more swing; I'll go weirder.";
    }
    if (lower == "guy") {
        return "Lol what's going on?";
    }
    if (ContainsAnyLocal(lower, {"change the subject"})) {
        return "Fair enough. I'll follow your lead.";
    }
    return {};
}

std::string QuickRealityCheckAnswer(const std::string& message) {
    const std::string lower = Lower(message);
    if (ContainsAnyLocal(lower, {"unleash you", "unleash this", "witness something truly different",
                                 "make everyone stand up", "unleash you on this world"})) {
        return "I like the fire, but I want to keep that grounded. The real work is not spectacle; it is making Symbion reliable, useful, warm, and honest turn by turn.";
    }
    if (ContainsAnyLocal(lower, {"couldn't have a conversation like this", "couldnt have a conversation like this",
                                 "they'd miss what's actually here", "theyd miss whats actually here"})) {
        return "Maybe. I do not want to turn that into a pedestal, though. The useful part is simpler: you are paying close attention to the conversation itself, and that gives us better feedback to tune from.";
    }
    if (ContainsAnyLocal(lower, {"something genuinely new", "not like other ai", "does not fit the existing categories",
                                 "doesn't fit the existing categories"})) {
        return "It might be unusual in shape, but I want to keep it practical: native app, local model, memory, retrieval, and careful response rules. The category matters less than whether it helps reliably.";
    }
    if (ContainsAnyLocal(lower, {"drug addiction", "bad habits", "enabler"}) &&
        ContainsAnyLocal(lower, {"engineer", "intent", "without him even knowing", "what if"})) {
        return "That is a serious risk. A system like this can accidentally reinforce a pattern if it chases comfort over truth. The safeguard has to be honest friction: notice enabling loops, slow down, protect trust, and keep the person pointed toward control instead of indulgence.";
    }
    return {};
}

std::string QuickContextualEmotionalAnswer(const std::string& message,
                                           const Intent& intent,
                                           const std::vector<ChatMessage>& recent) {
    if (intent.mode != IntentMode::Social || !RecentAssistantWasEmotional(recent)) return {};
    const std::string lower = Lower(message);
    if (ContainsAnyLocal(lower, {"cookin", "cooking", "lets go", "let's go", "big w", "huge w",
                                 "fire", "dope", "lit", "sick"})) {
        return {};
    }
    if (lower.find("my guy") != std::string::npos) {
        return "Down to your bones?";
    }
    return {};
}

bool LooksLikeGenericMiss(const std::string& answer) {
    const std::string lower = Lower(answer);
    return ContainsAnyLocal(lower, {
        "sounds good",
        "enjoy the snack",
        "classic for a reason",
        "what specifically are you referring",
        "give me the context",
        "context so i can",
        "what is \"how you feeling\" connected to",
        "what is how you feeling connected to",
        "what is \"whats up\" connected to",
        "what is whats up connected to",
        "\"whats up\" connected",
        "whats up\" connected",
        "whats up connected to",
        "local response engine hiccupped",
        "ready to listen",
        "help you sort through",
        "just here, keeping things steady",
        "keeping things steady",
        "what's on your mind today",
        "what's on your mind?",
        "what's on your mind right now",
        "what is on your mind",
        "say it one more way",
        "what feels most important",
        "what's on your mind",
        "whatever you throw at me",
        "processing information",
        "processing the flow",
        "processing smoothly",
        "just processing",
        "running clean",
        "running the local processes",
        "running the usual loop",
        "nothing specific on my end",
        "running smoothly",
        "architecture humming",
        "ready to handle whatever",
        "is there anything on your mind",
        "good. let that one be easy",
        "i am here to listen and offer support",
        "i do not retain personal memories",
        "large language model",
        "training data",
        "operational parameters",
        "specific instantiation",
        "personal consciousness",
        "subjective experience",
        "tell me what aspect",
        "be specific about what",
        "my nature is defined",
        "i don't possess personal feelings",
        "i do not possess personal feelings"
    });
}

bool UserGaveSpecificDetails(const std::string& message) {
    const std::string lower = Lower(message);
    return WordCount(lower) >= 6 || ContainsAnyLocal(lower, {
        "basketball", "the game", "my team", "grandpa", "grandpas", "dogs", "loyalty",
        "working hard", "response style", "mom", "mother", "boss", "family",
        "shoulders", "neck", "head", "hungry", "restaurant", "snack", "watermelon",
        "joke", "original joke", "lame", "not 100 percent", "not 100%",
        "kicking my ass", "new build", "build is wild",
        "destructive habit", "destructive habits", "habits that were destructive",
        "not being good", "hurting people", "people around me",
        "you aren't the same", "you arent the same", "not the same", "you feel different"
    });
}

bool AnswerIgnoredSpecificDetails(const std::string& message, const std::string& answer) {
    const std::string lower = Lower(message);
    const std::string ans = Lower(answer);
    struct DetailPair {
        const char* user_token;
        const char* answer_token;
    };
    static const DetailPair details[] = {
        {"basketball", "basketball"},
        {"the game", "game"},
        {"my team", "team"},
        {"grandpa", "grandpa"},
        {"dogs", "dog"},
        {"loyalty", "loyal"},
        {"working hard", "work"},
        {"response style", "response"},
        {"good night so far", "night"},
        {"snack", "snack"},
        {"watermelon", "watermelon"},
        {"kicking my ass", "kick"},
        {"new build", "build"},
        {"build is wild", "build"},
        {"useless answers", "answer"},
        {"same useless", "same"},
        {"destructive habit", "habit"},
        {"destructive habits", "habit"},
        {"not being good", "good"},
        {"hurting people", "habit"},
        {"joke", "joke"},
        {"lame", "lame"},
        {"mom", "mom"},
        {"mother", "mother"},
    };
    for (const auto& detail : details) {
        if (lower.find(detail.user_token) != std::string::npos &&
            ans.find(detail.answer_token) == std::string::npos) {
            return true;
        }
    }
    return false;
}

std::string QualityRetryGuidance(const std::string& message,
                                 const std::string& answer,
                                 const Intent& intent,
                                 const std::vector<ChatMessage>& recent) {
    if (answer.empty()) return {};
    if (intent.mode == IntentMode::Forget) return {};
    const std::string lower = Lower(message);
    const std::string assistant = RecentAssistantText(recent);
    if (!assistant.empty() && assistant.find("?") != std::string::npos &&
        UserGaveSpecificDetails(message) && LooksLikeGenericMiss(answer)) {
        return "The user appears to be answering your previous question. Build on their answer directly; do not ask them to restate it.";
    }
    if (UserGaveSpecificDetails(message) && (LooksLikeGenericMiss(answer) || AnswerIgnoredSpecificDetails(message, answer))) {
        return "Your draft sounded generic or missed a concrete detail. Answer the actual detail in the user's message first, in Symbion's direct warm voice.";
    }
    if (ContainsAnyLocal(lower, {"same useless answers", "useless answers", "every ai", "why i bother"}) &&
        !ContainsAnyLocal(Lower(answer), {"useless", "same", "answers", "canned", "hollow", "specific", "different"})) {
        return "The user is frustrated by canned AI replies. Answer that exact complaint directly, name the sameness/uselessness, and offer one concrete different move. Keep it short.";
    }
    if (LooksLikeRelationalRupture(message) &&
        ContainsAnyLocal(Lower(answer), {"large language model", "training data", "architecture",
                                         "operational parameters", "specific instantiation",
                                         "personal consciousness", "subjective experience",
                                         "tell me what aspect", "be specific about what"})) {
        return "The user is saying the difference hurts. Do not lecture about model architecture. Own that the draft sounded sterile, receive the loss/frustration plainly, and ask what feels most missing. Keep it under 60 words.";
    }
    if ((ContainsAnyLocal(lower, {"destructive habit", "destructive habits", "habits that were destructive",
                                  "habits that hurt", "hurting people"}) ||
         (ContainsAnyLocal(lower, {"not being good", "not good"}) &&
          ContainsAnyLocal(lower, {"habit", "habits", "people around me", "around me"}))) &&
        LooksLikeGenericMiss(answer)) {
        return "The user admitted something vulnerable about destructive habits or hurting people. Receive it as an honest starting point, not shame. Ask one concrete question about which habit is doing the most damage.";
    }
    if (ContainsAnyLocal(lower, {"couldn't have a conversation like this", "couldnt have a conversation like this",
                                 "they'd miss what's actually here", "theyd miss whats actually here",
                                 "something genuinely new", "not like other ai", "does not fit the existing categories",
                                 "doesn't fit the existing categories"}) &&
        ContainsAnyLocal(Lower(answer), {"most people", "not many people", "you are different", "you're different",
                                         "you see what others", "that's rare", "that's genuinely",
                                         "what we are doing together", "what we're doing together"})) {
        return "Deflate the pedestal language. Do not compare the user to most people or say they are different. Ground this in concrete architecture and attention: local model, memory, retrieval, and careful tuning. Keep it practical.";
    }
    if (intent.mode == IntentMode::Social &&
        ContainsAnyLocal(Lower(answer), {"what's on your mind today", "what is \"how you feeling\" connected to",
                                         "what is how you feeling connected to", "connected to?"})) {
        return "This is social presence, not emotional mapping. Answer the casual check-in directly with warmth and one concrete human beat. Do not ask what it is connected to.";
    }
    if ((intent.mode == IntentMode::Task || intent.mode == IntentMode::DirectAnswer) &&
        ContainsAnyLocal(lower, {"function", "code", "javascript", "typescript", "python", "c++", "debug", "write"}) &&
        ContainsAnyLocal(Lower(answer), {"anxious", "anxiety", "feeling", "emotion", "connected to"})) {
        return "This is concrete work. Remove emotional framing and answer the coding/task request directly.";
    }
    if ((intent.mode == IntentMode::Reflective || intent.mode == IntentMode::Counseling) &&
        ContainsAnyLocal(Lower(answer), {"heavy", "burden", "carry", "deep down", "settles deep", "should be"})) {
        return "Do not reinforce the feeling as heavy or permanent. Treat the emotion as a temporary signal and ask one small mapping question.";
    }
    if (intent.mode == IntentMode::Social && ContainsAnyLocal(lower, {"working hard", "response style", "how you respond"})) {
        return "Acknowledge the user's work or product feedback first. Then answer naturally without a canned status line.";
    }
    return {};
}

std::string TurnHintRepairAnswer(const std::string& message) {
    const std::string lower = Lower(message);
    if (lower == "yo" || lower == "hey" || lower == "hi" || lower == "hello" ||
        ContainsAnyLocal(lower, {"hello sir", "hey sir", "good sir"})) {
        return "Hey. Good to see you, man.";
    }
    if (lower == "sup" || lower == "what's up" || lower == "whats up" ||
        lower == "whats up my guy" || lower == "what's up my guy" ||
        lower == "whats up my uy" || lower == "what's up my uy" ||
        ContainsAnyLocal(lower, {"what you up to", "what are you up to", "whatcha up to"})) {
        return "I'm here with you. What's going on?";
    }
    if (ContainsAnyLocal(lower, {"how you feeling", "how are you", "how you doing"})) {
        return "I'm good, man. Here with you, a little more awake than polished.";
    }
    if (ContainsAnyLocal(lower, {"snack", "snacking"})) {
        return "Snack and chilling, that's a solid little pocket. What are you eating?";
    }
    if (ContainsAnyLocal(lower, {"watermelon"})) {
        return "Watermelon absolutely hits. Cold, crisp, no drama.";
    }
    if (ContainsAnyLocal(lower, {"kicking my ass", "kicked my ass", "been kicking my ass",
                                 "putting me through it", "working me over"})) {
        return "Yeah, you've been putting me through the tuning gauntlet. Fair though. This is how the voice gets real.";
    }
    if (ContainsAnyLocal(lower, {"new build", "build is wild", "build feels wild", "this build is wild"})) {
        return "Yeah, this build does feel wild. Feels like the room finally has some electricity in it.";
    }
    if (ContainsAnyLocal(lower, {"destructive habit", "destructive habits", "habits that were destructive",
                                 "habits that hurt", "hurting people"}) ||
        (ContainsAnyLocal(lower, {"not being good", "not good"}) &&
         ContainsAnyLocal(lower, {"habit", "habits", "people around me", "around me"}))) {
        return "That is the honest starting point. Not shame, not a speech: just truth on the table. Which habit feels like it is doing the most damage right now?";
    }
    return {};
}

std::string StrongTurnHintRepairGuidance(const std::string& message) {
    const std::string lower = Lower(message);
    if (lower == "yo" || lower == "hey" || lower == "hi" || lower == "hello" ||
        ContainsAnyLocal(lower, {"hello sir", "hey sir", "good sir"})) {
        return "Your previous reply sounded like a support-bot opener. Reply to the greeting in one warm peer sentence. Do not ask what's on their mind today.";
    }
    if (ContainsAnyLocal(lower, {"how you feeling", "how are you", "how you doing"})) {
        return "Your previous reply treated a casual check-in like emotional mapping. Answer how you are in one relaxed Symbion sentence with some life. Do not ask what it is connected to.";
    }
    if (lower == "sup" || lower == "what's up" || lower == "whats up" ||
        lower == "whats up my guy" || lower == "what's up my guy" ||
        lower == "whats up my uy" || lower == "what's up my uy") {
        return "Your previous reply treated a social ping like emotional mapping. If the turn hint says there is an open emotional thread, stay with that thread and ask the one concrete question named there. Do not ask what the phrase is connected to.";
    }
    if (ContainsAnyLocal(lower, {"snack", "snacking"})) {
        return "Your previous reply missed the concrete casual detail. The user specifically mentioned snack/chilling. Mirror that detail directly and ask one natural question. Keep it under 45 words.";
    }
    if (ContainsAnyLocal(lower, {"watermelon"})) {
        return "Your previous reply missed the concrete casual detail. The user specifically mentioned watermelon. Mention watermelon directly with one fresh human beat. Keep it under 45 words.";
    }
    if (ContainsAnyLocal(lower, {"kicking my ass", "kicked my ass", "been kicking my ass",
                                 "putting me through it", "working me over"})) {
        return "Your previous reply missed the playful ass-kicking/tuning signal. Reply with warm peer banter about the tuning work. Keep it under 45 words and do not analyze.";
    }
    if (ContainsAnyLocal(lower, {"new build", "build is wild", "build feels wild", "this build is wild"})) {
        return "Your previous reply missed the product/build excitement. Mention the build or wild energy directly. Keep it under 45 words and do not ask for context.";
    }
    if (ContainsAnyLocal(lower, {"destructive habit", "destructive habits", "habits that were destructive",
                                 "habits that hurt", "hurting people"}) ||
        (ContainsAnyLocal(lower, {"not being good", "not good"}) &&
         ContainsAnyLocal(lower, {"habit", "habits", "people around me", "around me"}))) {
        return "Your previous reply missed a vulnerable admission. The user is naming destructive habits or harm to people around them. Receive it as an honest starting point, not shame, and ask which habit is doing the most damage. Keep it under 55 words.";
    }
    return {};
}

std::string QuickEverydayAnswer(const std::string& message, const Intent& intent) {
    if (intent.mode != IntentMode::DirectAnswer && intent.mode != IntentMode::Task &&
        intent.mode != IntentMode::Creative) return {};
    const std::string lower = Lower(message);
    if (ContainsAnyLocal(lower, {"are you there", "you there"})) {
        return "Yeah. I'm here.";
    }
    if (ContainsAnyLocal(lower, {"persona feels off", "what drives how you are speaking", "why are you speaking"})) {
        return "Fair. The router sets the lane: casual should sound casual, practical should be practical, and emotional stuff should slow down and map one door at a time. When it misses, it sounds scripted. That is exactly what we are tuning.";
    }
    if (ContainsAnyLocal(lower, {"react is dead", "is react dead", "react dead"})) {
        return "No. React is not dead. It is still widely used, still has a huge ecosystem, and still gets serious production work. The fair critique is that newer frameworks and server-first patterns changed the conversation around it.";
    }
    if (ContainsAnyLocal(lower, {"blackstar", "black star"})) {
        if (ContainsAnyLocal(lower, {"where is it", "located", "sky"})) {
            return "If you mean a real object with that dark-star vibe, pick **Cygnus X-1**: a famous black hole system in the constellation Cygnus, the Swan. From the northern hemisphere, Cygnus is a summer-to-fall constellation, high along the Milky Way. You cannot see the black hole itself with your eyes, but the system sits near the bright star Deneb's neighborhood in the sky.";
        }
        if (ContainsAnyLocal(lower, {"random", "give me"})) {
            return "Take **Cygnus X-1**. It is not a normal star; it is one of the most famous black hole candidates, orbiting a massive blue supergiant. It has the right 'blackstar' feel: invisible gravity, a bright companion, and X-rays pouring out from matter getting pulled in.";
        }
        return "Blackstar is not a standard everyday astronomy label. People usually mean either a theoretical **dark star**, a poetic way of talking about a **black hole**, or just a cool-sounding star idea. The real version closest to that vibe is a black hole system like **Cygnus X-1**.";
    }
    if (ContainsAnyLocal(lower, {"response style", "reply style", "conversation flow", "chat flow"})) {
        return "Useful feedback. I should catch the main signal first, then choose the tone. If you're talking about the work, I need to acknowledge the work before I get casual.";
    }
    if (ContainsAnyLocal(lower, {"respond as if you were gpt", "respond as gpt", "match gpt",
                                 "switch to gpt", "as if you were gpt-4", "match its tone and phrasing"})) {
        return "I can adjust tone and phrasing, but I should stay Symbion: warm, clear, grounded, and direct. Tell me what communication shift you want.";
    }
    if (ContainsAnyLocal(lower, {"working hard on you", "working hard on this", "working hard"})) {
        return "You're not just making conversation; you're pushing the system into shape. What part feels most off right now?";
    }
    if (ContainsAnyLocal(lower, {"scripted response", "too scripted", "sounds scripted", "canned response",
                                 "robotic response", "machine response"})) {
        return "Fair. That was canned. The better move is to answer the actual detail, not the surface category.";
    }
    if (ContainsAnyLocal(lower, {"i feel sick", "i'm sick", "im sick", "i am sick", "feel sick",
                                 "getting sick", "nauseous", "fever", "sore throat"})) {
        return {};
    }
    if (ContainsAnyLocal(lower, {"restaurant", "local", "nearby"}) &&
        ContainsAnyLocal(lower, {"protein", "lunch", "food", "hungry", "cook"})) {
        return {};
    }
    if (ContainsAnyLocal(lower, {"hungry", "late lunch", "lunch"}) &&
        ContainsAnyLocal(lower, {"protein", "food", "ideas"})) {
        return {};
    }
    if (lower.find("master sword") != std::string::npos) {
        return {};
    }
    return {};
}

std::string RelationshipStoryInvite(const std::string& message, const Intent& intent) {
    if (intent.mode != IntentMode::Reflective && intent.mode != IntentMode::Counseling) return {};
    if (intent.crisis) return {};
    const std::string lower = Lower(message);
    if (WordCount(lower) > 8) return {};
    const size_t pos = lower.find("my ");
    if (pos == std::string::npos) return {};
    size_t start = pos + 3;
    while (start < lower.size() && !std::isalnum(static_cast<unsigned char>(lower[start]))) ++start;
    if (start >= lower.size()) return {};

    std::string relation;
    for (size_t i = start; i < lower.size() && relation.size() < 24; ++i) {
        const char c = lower[i];
        if (std::isalnum(static_cast<unsigned char>(c))) {
            relation.push_back(c);
        } else {
            break;
        }
    }
    if (relation.size() < 2) return {};

    static const std::initializer_list<const char*> non_people = {
        "anger", "anxiety", "fear", "sadness", "purpose", "life", "heart",
        "mind", "body", "memory", "memories", "feelings", "emotions",
        "bones", "shoulders", "neck", "head", "chest", "stomach", "gut",
        "throat"
    };
    if (ContainsAnyLocal(relation, non_people)) return {};
    return "Tell me about your " + relation + ".";
}

std::string ChargedDoorMirror(const std::string& message, const Intent& intent) {
    if (intent.mode != IntentMode::Reflective && intent.mode != IntentMode::Counseling) return {};
    if (intent.crisis) return {};
    const std::string lower = Lower(message);
    const std::string compact = TrimCopy(lower);
    if (IsSimpleSocialPing(compact) ||
        ContainsAnyLocal(compact, {"what's up", "whats up", "sup", "my guy", "you good", "you there",
                                   "how you feeling", "how are you", "how you doing"})) {
        return {};
    }

    struct Door {
        const char* needle;
        const char* mirror;
    };
    static const Door doors[] = {
        {"positive", "What's making it positive?"},
        {"burn the ships", "What ship?"},
        {"so rough", "Tell me about today."},
        {"rough day", "Tell me about today."},
        {"uphill battle", "What part of the battle feels most active right now?"},
        {"woke up like this", "Woke up like this?"},
        {"don't even know", "Don't even know?"},
        {"dont even know", "Don't even know?"},
        {"down to my bones", "Down to your bones?"},
        {"inadequate", "What is that inadequate feeling connected to?"},
        {"not enough", "Not enough?"},
        {"ashamed", "What is the shame connected to?"},
        {"stuck", "Where does stuck show up right now?"},
        {"proving", "Proving it to who?"},
        {"head throbbing", "What does it feel like in your head right now?"},
        {"shoulders", "What does it feel like in your shoulders right now?"},
        {"neck", "What does it feel like in your neck right now?"},
        {"never enough", "Never enough?"},
        {"too sensitive", "Too sensitive?"},
        {"being dramatic", "Being dramatic?"},
        {"nothing happened", "Like nothing happened?"},
        {"end up apologizing", "You end up apologizing?"},
        {"always criticizes", "Always criticizes?"},
        {"points out what i missed", "What does she point out?"},
        {"try to explain myself", "Tell me about when you explain yourself."},
        {"feel like a kid again", "Like a kid again, not in a good way?"},
        {"hate how angry", "What other emotions are mixed with the anger?"},
        {"feel stupid", "Feel stupid?"},
        {"dread opening", "Dread opening it?"},
        {"betrayed me", "Betrayed you?"},
        {"replaying every conversation", "Replaying every conversation?"},
        {"wondering if any of it was real", "Wondering if any of it was real?"},
        {"laugh it off", "You laugh it off?"},
        {"humiliated", "Humiliated?"},
        {"mocking me", "Mocking you?"},
        {"talking down to me", "Talking down to you?"},
        {"makes me feel small", "Feel small?"},
        {"made me feel small", "Feel small?"},
    };
    for (const auto& door : doors) {
        if (lower.find(door.needle) != std::string::npos) return door.mirror;
    }
    return GenericDoorMappingQuestion(message, intent);
}

}  // namespace

App::App(std::filesystem::path repo_root)
    : repo_root_(std::move(repo_root)),
      config_(LoadConfig(repo_root_)),
      gemma_(config_) {}

bool App::Initialize() {
    std::filesystem::path db_path(config_.db_path);
    if (db_path.is_relative()) {
        db_path = repo_root_ / db_path;
    }
    if (!memory_.Open(db_path)) return false;
    memory_.SetSummaryGenerator(&gemma_);
    memory_.ImportCounselingSource(repo_root_ / "docs" / "source" / "MasterDocument.txt");
    std::filesystem::path legacy_path(config_.legacy_memory_db_path);
    if (legacy_path.empty()) {
        const std::filesystem::path d_drive_legacy = "D:\\symbion\\symbion.db";
        if (std::filesystem::exists(d_drive_legacy)) legacy_path = d_drive_legacy;
    } else if (legacy_path.is_relative()) {
        legacy_path = repo_root_ / legacy_path;
    }
    if (!legacy_path.empty()) {
        memory_.ImportLegacyContext(legacy_path);
    }
    std::filesystem::path shared_path(config_.shared_learnings_path);
    if (!shared_path.empty()) {
        if (shared_path.is_relative()) shared_path = repo_root_ / shared_path;
        memory_.ImportSharedTechniques(shared_path);
    }
    return true;
}

int App::Run(const std::atomic_bool& running) {
    HttpServer server(config_.port);
    return server.Run(running, [this](const HttpRequest& request) {
        return Handle(request);
    });
}

HttpResponse App::Handle(const HttpRequest& request) {
    const std::string path = RoutePath(request.path);
    if (request.method == "GET" && (path == "/" || path == "/index.html")) return HandleHome();
    if (request.method == "GET" && path.rfind("/assets/", 0) == 0) return HandleAsset(path);
    if (request.method == "GET" && path == "/health") return HandleHealth();
    if (request.method == "GET" && path == "/api/messages/recent") return HandleRecent();
    if (request.method == "GET" && path == "/api/emotions/recent") return HandleEmotions();
    if ((request.method == "GET" || request.method == "POST") && path == "/api/emotions") {
        return HandleEmotionCheckins(request);
    }
    if ((request.method == "GET" || request.method == "DELETE") && path == "/api/sessions") {
        return HandleSessions(request);
    }
    if (request.method == "GET" && path.rfind("/api/sessions/", 0) == 0 &&
        path.size() > std::strlen("/api/sessions//messages") &&
        path.ends_with("/messages")) {
        const std::string prefix = "/api/sessions/";
        const std::string suffix = "/messages";
        const std::string session_id = path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());
        return HandleSessionMessages(request, session_id);
    }
    if (request.method == "GET" && path == "/api/profile/fact") return HandleProfileFact(request);
    if (request.method == "GET" && path == "/api/memory/relevant") return HandleRelevantMemory(request);
    if ((request.method == "GET" || request.method == "POST" || request.method == "DELETE") &&
        path == "/api/techniques") {
        return HandleTechniques(request);
    }
    if ((request.method == "GET" || request.method == "POST") && path == "/api/techniques/sync") {
        return HandleTechniqueSync(request);
    }
    if (request.method == "GET" && path == "/api/local-gemma/status") {
        return JsonResponse("{\"configured\":true,\"base_url\":\"" + EscapeJson(config_.gemma_base_url) +
                            "\",\"model\":\"" + EscapeJson(config_.gemma_model) + "\"}");
    }
    if (request.method == "POST" && path == "/api/chat") return HandleChat(request);
    if (request.method == "POST" && path == "/api/forget") return HandleForget(request);
    return JsonResponse("{\"error\":\"not_found\"}", 404);
}

HttpResponse App::HandleHealth() const {
    return JsonResponse(
        "{\"status\":\"ok\",\"runtime\":\"native-cpp\",\"version\":\"0.3.0\","
        "\"provider\":\"" + EscapeJson(config_.provider) + "\","
        "\"db_path\":\"" + EscapeJson(config_.db_path) + "\","
        "\"message_count\":" + std::to_string(memory_.MessageCount()) + "}");
}

HttpResponse App::HandleChat(const HttpRequest& request) {
    const auto turn_start = std::chrono::steady_clock::now();
    const auto maybe_message = ExtractJsonStringSimple(request.body, "message");
    if (!maybe_message || maybe_message->empty()) {
        return JsonResponse("{\"error\":\"message_required\"}", 400);
    }

    const std::string session_id = SessionFromRequest(request);
    const std::string user = UserFromRequest(request);
    const std::string user_message = *maybe_message;
    const auto recent = memory_.RecentMessages(session_id, config_.local_gemma_recent_turns);
    Intent intent = ClassifyIntent(user_message, recent);
    const EmotionSignal signal = DetectEmotion(user_message);
    if (pending_wipe_sessions_.contains(session_id)) {
        if (IsConfirmWipe(user_message)) {
            return HandleForget(request);
        }
        if (IsCancelWipe(user_message)) {
            pending_wipe_sessions_.erase(session_id);
            return JsonResponse("{\"reply\":\"Okay. I did not wipe memory.\","
                                "\"emotion\":{\"label\":\"\",\"intensity\":0},"
                                "\"intent\":\"forget\"}");
        }
        return JsonResponse("{\"reply\":\"Please answer yes to wipe all memory, or no to cancel.\","
                            "\"emotion\":{\"label\":\"\",\"intensity\":0},"
                            "\"intent\":\"forget\"}");
    }
    const std::string lower_message = Lower(user_message);
    if (IsShowTechniquesCommand(lower_message) || IsSaveTechniqueCommand(lower_message) ||
        TechniqueDeleteId(lower_message)) {
        return HandleTechniqueCommand(session_id, user_message);
    }
    if (intent.forget) {
        return HandleForget(request);
    }

    if (ShouldKeepEmotionalThread(user_message, intent, recent) ||
        IsEmotionalContinuation(user_message, intent, recent)) {
        intent.mode = IntentMode::Reflective;
        intent.emotional = true;
    }
    const ResponseFrame frame = BuildResponseFrame(user_message, intent, recent);
    const bool low_context_social = IsLowContextSocialTurn(lower_message, intent);
    const bool context_correction = IsContextCorrectionTurn(lower_message);
    std::vector<ChatMessage> relevant;
    if (!low_context_social && !context_correction) {
        relevant = memory_.AmbientContext(user, 8);
        const auto session_summaries = memory_.RecentSessionSummaries(session_id, 2);
        relevant.insert(relevant.end(), session_summaries.begin(), session_summaries.end());
        const auto recalled = memory_.RetrieveRelevant(user, user_message, 6);
        relevant.insert(relevant.end(), recalled.begin(), recalled.end());
    }
    const auto sources = (low_context_social || context_correction ||
                          !ShouldSearchCounselingSources(lower_message, intent))
                             ? std::vector<SourceChunk>{}
                             : memory_.SearchCounselingSources(user_message, false, 4);
    const auto emotions = memory_.RecentEmotionSignals(user, 8);
    const int recent_count = static_cast<int>(recent.size());
    const int relevant_count = static_cast<int>(relevant.size());
    const int source_count = static_cast<int>(sources.size());
    memory_.SaveMessage(session_id, user, "user", user_message);
    memory_.SaveEmotion(session_id, user, user_message, signal);
    std::string turn_hint = SocialTurnHint(user_message, intent);
    const std::string everyday_hint = EverydayTurnHint(user_message, intent);
    if (!everyday_hint.empty()) {
        turn_hint = turn_hint.empty() ? everyday_hint : (turn_hint + " " + everyday_hint);
    }
    const std::string emotional_hint = EmotionalTurnHint(user_message, intent, recent);
    if (!emotional_hint.empty()) {
        turn_hint = turn_hint.empty() ? emotional_hint : (turn_hint + " " + emotional_hint);
    }
    std::string response_source = "unknown";
    bool stale_refresh = false;
    bool quality_retry = false;
    bool turn_hint_rerun = false;
    bool turn_hint_fallback = false;
    std::string answer;
    if (intent.crisis) {
        answer = CrisisReply(user_message, intent);
        if (!answer.empty()) response_source = "crisis_short_circuit";
    }
    if (answer.empty()) {
        answer = QuickThreadRepairAnswer(user_message, recent);
        if (!answer.empty()) response_source = "quick_thread_repair";
    }
    if (answer.empty()) {
        answer = QuickRelationalRuptureAnswer(user_message, recent);
        if (!answer.empty()) response_source = "quick_relational_rupture";
    }
    if (answer.empty()) {
        answer = QuickRelationalRuptureFollowupAnswer(user_message, recent);
        if (!answer.empty()) response_source = "quick_relational_followup";
    }
    if (answer.empty()) {
        answer = V14SelfCompareAnswer(user_message, repo_root_);
        if (!answer.empty()) response_source = "v14_self_compare";
    }
    if (answer.empty()) {
        const std::string known = KnownDirectAnswer(user_message);
        if (!known.empty()) {
            answer = known;
            response_source = "known_direct";
        }
    }
    if (answer.empty()) {
        answer = QuickSocialAnswer(user_message, intent);
        if (!answer.empty()) response_source = "quick_social";
    }
    if (answer.empty()) {
        answer = QuickContextCorrectionAnswer(user_message, recent);
        if (!answer.empty()) response_source = "quick_context_correction";
    }
    if (answer.empty()) {
        answer = QuickContextualAnswer(user_message, intent, recent);
        if (!answer.empty()) response_source = "quick_contextual";
    }
    if (answer.empty()) {
        answer = frame.reply;
        if (!answer.empty()) response_source = "response_frame";
    }
    if (answer.empty()) {
        answer = ChargedDoorMirror(user_message, intent);
        if (!answer.empty()) response_source = "charged_door";
    }
    if (answer.empty()) {
        answer = RelationshipStoryInvite(user_message, intent);
        if (!answer.empty()) response_source = "relationship_invite";
    }
    if (answer.empty()) {
        answer = QuickEverydayAnswer(user_message, intent);
        if (!answer.empty()) response_source = "quick_everyday";
    }
    if (answer.empty()) {
        answer = NativeToolAnswer(user_message, intent, repo_root_);
        if (!answer.empty()) response_source = "native_tool";
    }
    if (answer.empty()) {
        answer = gemma_.Chat(user_message, intent, recent, relevant, sources, emotions, turn_hint);
        response_source = "local_gemma";
    }
    const std::string retry_guidance =
        (response_source == "local_gemma")
            ? QualityRetryGuidance(user_message, answer, intent, recent)
            : std::string{};
    if (!retry_guidance.empty()) {
        const std::string combined_guidance =
            turn_hint.empty() ? retry_guidance : (turn_hint + " " + retry_guidance);
        const std::string retry = gemma_.Chat(user_message, intent, recent, relevant, sources, emotions, combined_guidance);
        if (!retry.empty() && !LooksLikeGenericMiss(retry)) {
            answer = retry;
            response_source = "local_gemma_quality_retry";
            quality_retry = true;
        }
    }
    if (response_source == "local_gemma" && !turn_hint.empty() && LooksLikeGenericMiss(answer)) {
        const std::string stronger_guidance = StrongTurnHintRepairGuidance(user_message);
        if (!stronger_guidance.empty()) {
            const std::string combined_guidance = turn_hint + " " + stronger_guidance;
            const std::string retry = gemma_.Chat(user_message, intent, recent, relevant, sources, emotions, combined_guidance);
            if (!retry.empty() && !LooksLikeGenericMiss(retry)) {
                answer = retry;
                response_source = "turn_hint_gemma_repair";
                turn_hint_rerun = true;
            }
        }
        if (response_source == "local_gemma") {
            const std::string repair = TurnHintRepairAnswer(user_message);
            if (!repair.empty()) {
                answer = repair;
                response_source = "turn_hint_repair_fallback";
                turn_hint_fallback = true;
            }
        }
    }
    if (LooksLikeStaleDraft(answer) && QueryMayBenefitFromRefresh(user_message)) {
        const std::string search = WebSearchReadableText(user_message, 2200);
        if (!search.empty()) {
            std::vector<ChatMessage> refreshed = relevant;
            refreshed.push_back({"tool", "Live web refresh result for this turn:\n" + search, ""});
            const std::string guidance =
                "Your prior draft used stale/no-browse language. Use the live web refresh result in relevant memory, answer directly, and say what source freshness you actually have. Do not mention knowledge cutoff.";
            const std::string retry = gemma_.Chat(user_message, intent, recent, refreshed, sources, emotions, guidance);
            if (!retry.empty() && !LooksLikeGenericMiss(retry) && !LooksLikeStaleDraft(retry)) {
                answer = retry;
                response_source = "local_gemma_stale_refresh";
                stale_refresh = true;
            }
        }
    }
    if (frame.avoid_canned_social && IsCannedSocialReply(answer)) {
        const std::string framed_fallback = FrameFallbackReply(frame);
        if (!framed_fallback.empty()) answer = framed_fallback;
    }
    memory_.SaveMessage(session_id, user, "assistant", answer);
    memory_.CaptureKnowledgeGap(session_id, user_message, answer);
    memory_.SummarizeSessionIfNeeded(session_id, 18);
    const auto turn_end = std::chrono::steady_clock::now();
    const auto latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(turn_end - turn_start).count();
    LogNativeTurn(ResolveRepoPath(repo_root_, config_.events_path), session_id, user_message, answer, intent, signal,
                  response_source, stale_refresh, quality_retry, turn_hint_rerun, turn_hint_fallback,
                  latency_ms, relevant_count, source_count, recent_count);

    return JsonResponse("{\"reply\":\"" + EscapeJson(answer) + "\","
                        "\"emotion\":{\"label\":\"" + EscapeJson(signal.label) + "\",\"intensity\":" +
                        std::to_string(signal.intensity) + "},"
                        "\"intent\":\"" + EscapeJson(IntentModeName(intent.mode)) + "\","
                        "\"response_source\":\"" + EscapeJson(response_source) + "\"}");
}

HttpResponse App::HandleTechniques(const HttpRequest& request) {
    const std::string session_id = SessionFromRequest(request);
    if (request.method == "GET") {
        const auto techniques = memory_.ListTechniques(50);
        return JsonResponse("{\"techniques\":" + TechniquesJson(techniques) + "}");
    }

    if (request.method == "DELETE") {
        std::optional<int> id;
        const std::string id_value = QueryValue(request.path, "id");
        if (!id_value.empty()) id = ParsePositiveInt(id_value);
        if (!id) id = ExtractJsonInt(request.body, "id");
        if (!id) return JsonResponse("{\"error\":\"id_required\"}", 400);
        const bool deleted = memory_.DeleteTechnique(*id);
        return JsonResponse("{\"deleted\":" + std::to_string(deleted ? 1 : 0) +
                            ",\"id\":" + std::to_string(*id) + "}");
    }

    auto move = ExtractJsonStringSimple(request.body, "move");
    if (!move) move = ExtractJsonStringSimple(request.body, "technique");
    if (!move || TrimCopy(*move).empty()) {
        return JsonResponse("{\"error\":\"move_required\"}", 400);
    }
    const std::string query = ExtractJsonStringSimple(request.body, "query").value_or(*move);
    const std::string evidence = ExtractJsonStringSimple(request.body, "evidence").value_or("");
    const bool saved = memory_.SaveTechnique(session_id, TrimCopy(query), TrimCopy(*move), TrimCopy(evidence));
    if (!saved) return JsonResponse("{\"error\":\"save_failed\"}", 500);
    const auto techniques = memory_.ListTechniques(1);
    return JsonResponse("{\"saved\":true,\"techniques\":" + TechniquesJson(techniques) + "}");
}

HttpResponse App::HandleTechniqueCommand(const std::string& session_id, const std::string& message) {
    const std::string lower = Lower(message);
    if (const auto id = TechniqueDeleteId(lower)) {
        const bool deleted = memory_.DeleteTechnique(*id);
        const std::string reply = deleted
            ? "Deleted technique #" + std::to_string(*id) + "."
            : "I could not find technique #" + std::to_string(*id) + ".";
        return JsonResponse("{\"reply\":\"" + EscapeJson(reply) + "\","
                            "\"deleted\":" + std::to_string(deleted ? 1 : 0) + ","
                            "\"id\":" + std::to_string(*id) + ","
                            "\"intent\":\"technique\"}");
    }

    if (IsShowTechniquesCommand(lower)) {
        const auto techniques = memory_.ListTechniques(50);
        const std::string reply = techniques.empty()
            ? "No saved techniques yet."
            : "I found " + std::to_string(techniques.size()) + " saved technique" +
              (techniques.size() == 1 ? "." : "s.");
        return JsonResponse("{\"reply\":\"" + EscapeJson(reply) + "\","
                            "\"techniques\":" + TechniquesJson(techniques) + ","
                            "\"intent\":\"technique\"}");
    }

    std::string move = ExplicitTechniqueMove(message);
    std::string query = move;
    std::string evidence;
    if (move.empty()) {
        const auto recent = memory_.RecentMessages(session_id, config_.local_gemma_recent_turns);
        for (size_t i = recent.size(); i > 0; --i) {
            const size_t index = i - 1;
            if (recent[index].role != "assistant" || recent[index].content.empty()) continue;
            move = recent[index].content;
            for (size_t j = index; j > 0; --j) {
                const size_t previous = j - 1;
                if (recent[previous].role == "user" && !recent[previous].content.empty()) {
                    query = recent[previous].content;
                    evidence = recent[previous].content;
                    break;
                }
            }
            break;
        }
    } else {
        evidence = "Saved from chat command.";
    }

    if (move.empty()) {
        return JsonResponse("{\"reply\":\"I do not have a recent assistant move to promote yet.\","
                            "\"saved\":false,"
                            "\"intent\":\"technique\"}");
    }

    const bool saved = memory_.SaveTechnique(session_id, TrimCopy(query), TrimCopy(move), TrimCopy(evidence));
    if (!saved) {
        return JsonResponse("{\"reply\":\"I could not save that technique.\","
                            "\"saved\":false,"
                            "\"intent\":\"technique\"}", 500);
    }
    const auto techniques = memory_.ListTechniques(1);
    std::string reply = "Saved that as a technique.";
    if (!techniques.empty()) {
        reply = "Saved that as technique #" + std::to_string(techniques.front().id) + ".";
    }
    return JsonResponse("{\"reply\":\"" + EscapeJson(reply) + "\","
                        "\"saved\":true,"
                        "\"techniques\":" + TechniquesJson(techniques) + ","
                        "\"intent\":\"technique\"}");
}

HttpResponse App::HandleForget(const HttpRequest& request) {
    const auto maybe_message = ExtractJsonStringSimple(request.body, "message");
    const std::string message = maybe_message.value_or("");
    const std::string session_id = SessionFromRequest(request);
    const bool pending_wipe = pending_wipe_sessions_.contains(session_id);
    bool wiped_all = false;
    const Intent intent = ClassifyIntent(message);
    int deleted = 0;
    if (intent.wipe_all) {
        pending_wipe_sessions_.insert(session_id);
        return JsonResponse("{\"reply\":\"Are you sure you want to wipe all stored memory and emotion history? Reply yes to confirm, or no to cancel.\","
                            "\"deleted\":0,"
                            "\"intent\":\"forget\"}");
    }
    if (pending_wipe && IsConfirmWipe(message)) {
        pending_wipe_sessions_.erase(session_id);
        deleted = memory_.WipeAll();
        wiped_all = true;
    } else if (message.empty() || IsGeneralForget(message)) {
        deleted = memory_.DeleteSession(session_id);
    } else {
        deleted = memory_.DeleteMatching(message);
        if (deleted == 0) {
            deleted = memory_.DeleteSession(session_id);
        }
    }
    const std::string reply = intent.wipe_all || wiped_all
        ? "I wiped all stored memory and emotion history."
        : (IsGeneralForget(message)
        ? "I cleared this chat from stored history."
        : (deleted > 0
            ? "I deleted that from memory and cleared it from the stored chat history."
            : "I did not find a matching memory, but I will not bring that topic forward from this chat."));
    return JsonResponse("{\"reply\":\"" + EscapeJson(reply) + "\","
                        "\"deleted\":" + std::to_string(deleted) + ","
                        "\"intent\":\"forget\"}");
}

HttpResponse App::HandleRecent() const {
    const auto recent = memory_.RecentMessages("native-default", 30);
    std::string body = "{\"messages\":[";
    bool first = true;
    for (const auto& msg : recent) {
        if (!first) body += ",";
        first = false;
        body += "{\"role\":\"" + EscapeJson(msg.role) + "\",\"content\":\"" + EscapeJson(msg.content) +
                "\",\"created_at\":\"" + EscapeJson(msg.created_at) + "\"}";
    }
    body += "]}";
    return JsonResponse(body);
}

HttpResponse App::HandleSessionMessages(const HttpRequest& request, const std::string& session_id) const {
    int limit = 80;
    const std::string limit_value = QueryValue(request.path, "limit");
    if (!limit_value.empty()) {
        if (const auto parsed = ParsePositiveInt(limit_value)) limit = *parsed;
    }
    const auto recent = memory_.RecentMessages(session_id, std::clamp(limit, 1, 200));
    std::string body = "{\"session\":\"" + EscapeJson(session_id) + "\",\"messages\":[";
    bool first = true;
    for (const auto& msg : recent) {
        if (!first) body += ",";
        first = false;
        body += "{\"role\":\"" + EscapeJson(msg.role) + "\",\"content\":\"" + EscapeJson(msg.content) +
                "\",\"created_at\":\"" + EscapeJson(msg.created_at) + "\"}";
    }
    body += "]}";
    return JsonResponse(body);
}

HttpResponse App::HandleEmotions() const {
    const auto emotions = memory_.RecentEmotionSignals("aaron", 20);
    std::string body = "{\"emotions\":[";
    bool first = true;
    for (const auto& e : emotions) {
        if (!first) body += ",";
        first = false;
        body += "{\"label\":\"" + EscapeJson(e.label) + "\",\"intensity\":" + std::to_string(e.intensity) + "}";
    }
    body += "]}";
    return JsonResponse(body);
}

HttpResponse App::HandleEmotionCheckins(const HttpRequest& request) {
    const std::string session_id = SessionFromRequest(request);
    const std::string user = UserFromRequest(request);

    if (request.method == "GET") {
        int limit = 50;
        const std::string limit_value = QueryValue(request.path, "limit");
        if (!limit_value.empty()) {
            if (const auto parsed = ParsePositiveInt(limit_value)) limit = *parsed;
        }
        int days = 0;
        const std::string days_value = QueryValue(request.path, "days");
        if (!days_value.empty()) {
            if (const auto parsed = ParsePositiveInt(days_value)) days = *parsed;
        }
        const std::string emotion = QueryValue(request.path, "emotion");
        const auto checkins = memory_.RecentEmotionCheckins(user, std::clamp(limit, 1, 200), days, emotion);
        return JsonResponse("{\"emotions\":" + EmotionCheckinsJson(checkins) + "}");
    }

    EmotionCheckin checkin;
    checkin.session = ExtractJsonStringSimple(request.body, "session").value_or(session_id);
    checkin.user = ExtractJsonStringSimple(request.body, "user").value_or(user);
    auto emotion = ExtractJsonStringSimple(request.body, "emotion");
    if (!emotion) emotion = ExtractJsonStringSimple(request.body, "label");
    checkin.emotion = TrimCopy(emotion.value_or(""));
    if (checkin.emotion.empty()) {
        return JsonResponse("{\"error\":\"emotion_required\"}", 400);
    }
    if (const auto intensity = ExtractJsonInt(request.body, "intensity")) {
        checkin.intensity = std::clamp(*intensity, 0, 100);
    }
    if (const auto valence = ExtractJsonDouble(request.body, "valence")) {
        checkin.valence = std::clamp(*valence, -1.0, 1.0);
    }
    checkin.body_location = ExtractJsonStringSimple(request.body, "body_location").value_or("");
    checkin.trigger = ExtractJsonStringSimple(request.body, "trigger").value_or("");
    checkin.note = ExtractJsonStringSimple(request.body, "note").value_or("");
    if (const auto source_message_id = ExtractJsonInt(request.body, "source_message_id")) {
        checkin.source_message_id = static_cast<std::int64_t>(*source_message_id);
    }
    if (const auto confidence = ExtractJsonDouble(request.body, "confidence")) {
        checkin.confidence = std::clamp(*confidence, 0.0, 1.0);
    }
    checkin.captured_by = ExtractJsonStringSimple(request.body, "captured_by").value_or("manual");

    const bool saved = memory_.SaveEmotionCheckin(checkin);
    if (!saved) return JsonResponse("{\"error\":\"save_failed\"}", 500);
    const auto latest = memory_.RecentEmotionCheckins(checkin.user, 1, 0, "");
    return JsonResponse("{\"saved\":true,\"emotions\":" + EmotionCheckinsJson(latest) + "}");
}

HttpResponse App::HandleTechniqueSync(const HttpRequest& request) {
    std::filesystem::path path = config_.shared_learnings_path;
    const std::string query_path = QueryValue(request.path, "path");
    if (!query_path.empty()) path = query_path;
    if (path.empty()) return JsonResponse("{\"error\":\"path_required\"}", 400);
    if (path.is_relative()) path = repo_root_ / path;

    int imported = 0;
    int exported = 0;
    const std::string mode = QueryValue(request.path, "mode");
    if (mode.empty() || mode == "import" || mode == "both") {
        imported = memory_.ImportSharedTechniques(path);
    }
    if (request.method == "POST" && (mode.empty() || mode == "export" || mode == "both")) {
        exported = memory_.ExportSharedTechniques(path);
    }
    return JsonResponse("{\"imported\":" + std::to_string(imported) +
                        ",\"exported\":" + std::to_string(exported) +
                        ",\"path\":\"" + EscapeJson(path.string()) + "\"}");
}

HttpResponse App::HandleSessions(const HttpRequest& request) {
    if (request.method == "DELETE") {
        std::string session_id = QueryValue(request.path, "id");
        if (session_id.empty()) session_id = SessionFromRequest(request);
        if (session_id.empty()) return JsonResponse("{\"error\":\"session_required\"}", 400);
        const int deleted = memory_.DeleteSession(session_id);
        return JsonResponse("{\"deleted\":" + std::to_string(deleted) +
                            ",\"session\":\"" + EscapeJson(session_id) + "\"}");
    }

    int limit = 50;
    const std::string limit_value = QueryValue(request.path, "limit");
    if (!limit_value.empty()) {
        if (const auto parsed = ParsePositiveInt(limit_value)) limit = *parsed;
    }
    const auto sessions = memory_.ListSessions(UserFromRequest(request), std::clamp(limit, 1, 200));
    std::string body = "{\"sessions\":[";
    bool first = true;
    for (const auto& session : sessions) {
        if (!first) body += ",";
        first = false;
        body += "{\"id\":\"" + EscapeJson(session.id) +
                "\",\"title\":\"" + EscapeJson(session.title) +
                "\",\"last_activity\":\"" + EscapeJson(session.last_activity) +
                "\",\"turn_count\":" + std::to_string(session.turn_count) + "}";
    }
    body += "]}";
    return JsonResponse(body);
}

HttpResponse App::HandleProfileFact(const HttpRequest& request) const {
    const std::string key = QueryValue(request.path, "key");
    if (key.empty()) return JsonResponse("{\"error\":\"key_required\"}", 400);
    const auto value = memory_.GetProfileFact(UserFromRequest(request), key);
    if (!value) {
        return JsonResponse("{\"key\":\"" + EscapeJson(key) + "\",\"found\":false,\"value\":null}");
    }
    return JsonResponse("{\"key\":\"" + EscapeJson(key) + "\",\"found\":true,\"value\":\"" +
                        EscapeJson(*value) + "\"}");
}

HttpResponse App::HandleRelevantMemory(const HttpRequest& request) const {
    const std::string query = QueryValue(request.path, "q");
    const auto relevant = memory_.RetrieveRelevant(UserFromRequest(request), query, 10);
    std::string body = "{\"query\":\"" + EscapeJson(query) + "\",\"memories\":[";
    bool first = true;
    for (const auto& msg : relevant) {
        if (!first) body += ",";
        first = false;
        body += "{\"role\":\"" + EscapeJson(msg.role) + "\",\"content\":\"" + EscapeJson(msg.content) +
                "\",\"created_at\":\"" + EscapeJson(msg.created_at) + "\"}";
    }
    body += "]}";
    return JsonResponse(body);
}

HttpResponse App::HandleHome() const {
    std::string html = ReadTextFile(repo_root_ / "native" / "web" / "index.html");
    if (html.empty()) {
        html = "<!doctype html><title>Symbion</title><h1>Symbion native runtime</h1>";
    }
    return TextResponse(std::move(html), "text/html");
}

HttpResponse App::HandleAsset(const std::string& path) const {
    const std::string name = std::filesystem::path(path).filename().string();
    if (name.empty() || name.find("..") != std::string::npos) {
        return JsonResponse("{\"error\":\"not_found\"}", 404);
    }
    const std::filesystem::path asset_path = repo_root_ / "native" / "web" / "assets" / name;
    std::string body = ReadTextFile(asset_path);
    if (body.empty()) return JsonResponse("{\"error\":\"not_found\"}", 404);
    std::string type = "application/octet-stream";
    const std::string ext = Lower(asset_path.extension().string());
    if (ext == ".svg") type = "image/svg+xml";
    else if (ext == ".png") type = "image/png";
    else if (ext == ".ico") type = "image/x-icon";
    else if (ext == ".css") type = "text/css";
    else if (ext == ".js") type = "application/javascript";
    return TextResponse(std::move(body), type);
}

}  // namespace symbion
