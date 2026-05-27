#include "app.h"

#include "json_util.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace symbion {

namespace {

std::string ReadTextFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::string SessionFromRequest(const HttpRequest& request) {
    if (auto it = request.headers.find("x-symbion-session"); it != request.headers.end() && !it->second.empty()) {
        return it->second;
    }
    return "native-default";
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
    return memory_.Open(db_path);
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
    if (request.method == "GET" && path == "/health") return HandleHealth();
    if (request.method == "GET" && path == "/api/messages/recent") return HandleRecent();
    if (request.method == "GET" && path == "/api/emotions/recent") return HandleEmotions();
    if (request.method == "GET" && path == "/api/memory/relevant") return HandleRelevantMemory(request);
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
    const auto maybe_message = ExtractJsonStringSimple(request.body, "message");
    if (!maybe_message || maybe_message->empty()) {
        return JsonResponse("{\"error\":\"message_required\"}", 400);
    }

    const std::string session_id = SessionFromRequest(request);
    const std::string user_message = *maybe_message;
    const Intent intent = ClassifyIntent(user_message);
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
    if (intent.forget) {
        return HandleForget(request);
    }

    const auto recent = memory_.RecentMessages(session_id, config_.local_gemma_recent_turns);
    const auto relevant = memory_.RetrieveRelevant(user_message, 5);
    const auto emotions = memory_.RecentEmotionSignals(8);
    memory_.SaveMessage(session_id, "user", user_message);
    memory_.SaveEmotion(session_id, user_message, signal);
    const std::string answer = gemma_.Chat(user_message, intent, recent, relevant, emotions);
    memory_.SaveMessage(session_id, "assistant", answer);

    return JsonResponse("{\"reply\":\"" + EscapeJson(answer) + "\","
                        "\"emotion\":{\"label\":\"" + EscapeJson(signal.label) + "\",\"intensity\":" +
                        std::to_string(signal.intensity) + "},"
                        "\"intent\":\"" + EscapeJson(IntentModeName(intent.mode)) + "\"}");
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

HttpResponse App::HandleEmotions() const {
    const auto emotions = memory_.RecentEmotionSignals(20);
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

HttpResponse App::HandleRelevantMemory(const HttpRequest& request) const {
    const std::string query = QueryValue(request.path, "q");
    const auto relevant = memory_.RetrieveRelevant(query, 10);
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

}  // namespace symbion
