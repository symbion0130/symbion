#include "app.h"

#include "json_util.h"

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
    if (request.method == "GET" && (request.path == "/" || request.path == "/index.html")) return HandleHome();
    if (request.method == "GET" && request.path == "/health") return HandleHealth();
    if (request.method == "GET" && request.path == "/api/messages/recent") return HandleRecent();
    if (request.method == "GET" && request.path == "/api/emotions/recent") return HandleEmotions();
    if (request.method == "GET" && request.path == "/api/local-gemma/status") {
        return JsonResponse("{\"configured\":true,\"base_url\":\"" + EscapeJson(config_.gemma_base_url) +
                            "\",\"model\":\"" + EscapeJson(config_.gemma_model) + "\"}");
    }
    if (request.method == "POST" && request.path == "/api/chat") return HandleChat(request);
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
    memory_.SaveMessage(session_id, "user", user_message);
    memory_.SaveEmotion(session_id, user_message, signal);

    if (intent.crisis) {
        const std::string answer = CrisisReply(user_message, intent);
        memory_.SaveMessage(session_id, "assistant", answer);
        return JsonResponse("{\"reply\":\"" + EscapeJson(answer) + "\","
                            "\"emotion\":{\"label\":\"" + EscapeJson(signal.label) + "\",\"intensity\":" +
                            std::to_string(signal.intensity) + "},"
                            "\"intent\":\"" + EscapeJson(IntentModeName(intent.mode)) + "\"}");
    }

    const auto recent = memory_.RecentMessages(session_id, config_.local_gemma_recent_turns);
    const auto relevant = memory_.RetrieveRelevant(user_message, 5);
    const auto emotions = memory_.RecentEmotionSignals(8);
    const std::string answer = gemma_.Chat(user_message, intent, recent, relevant, emotions);
    memory_.SaveMessage(session_id, "assistant", answer);

    return JsonResponse("{\"reply\":\"" + EscapeJson(answer) + "\","
                        "\"emotion\":{\"label\":\"" + EscapeJson(signal.label) + "\",\"intensity\":" +
                        std::to_string(signal.intensity) + "},"
                        "\"intent\":\"" + EscapeJson(IntentModeName(intent.mode)) + "\"}");
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

HttpResponse App::HandleHome() const {
    std::string html = ReadTextFile(repo_root_ / "native" / "web" / "index.html");
    if (html.empty()) {
        html = "<!doctype html><title>Symbion</title><h1>Symbion native runtime</h1>";
    }
    return TextResponse(std::move(html), "text/html");
}

}  // namespace symbion
