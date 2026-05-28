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

bool ContainsAnyLocal(const std::string& text, const std::initializer_list<const char*> needles) {
    return std::any_of(needles.begin(), needles.end(), [&](const char* needle) {
        return text.find(needle) != std::string::npos;
    });
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

bool IsEmotionalContinuation(const std::string& message,
                             const Intent& intent,
                             const std::vector<ChatMessage>& recent) {
    const std::string lower = Lower(message);
    if (recent.empty() || intent.forget || intent.wipe_all || lower.find('?') != std::string::npos) return false;
    if (intent.crisis || intent.emotional) return false;
    if (WordCount(lower) > 16) return false;
    if (ContainsAnyLocal(lower, {"write ", "make ", "create ", "build ", "fix ", "install ", "run ", "open "})) {
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
            "tell me about"
        });
    }
    return false;
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
    return {};
}

std::string QuickEverydayAnswer(const std::string& message, const Intent& intent) {
    if (intent.mode != IntentMode::DirectAnswer && intent.mode != IntentMode::Task &&
        intent.mode != IntentMode::Creative) return {};
    const std::string lower = Lower(message);
    if (ContainsAnyLocal(lower, {"persona feels off", "what drives how you are speaking", "why are you speaking"})) {
        return "Fair. What drives it is the router: casual chat should sound casual, practical questions should get practical answers, and emotional stuff should slow down and map the feeling. This reply should feel more like me talking with you, not reading a support script.";
    }
    if (ContainsAnyLocal(lower, {"restaurant", "local", "nearby"}) &&
        ContainsAnyLocal(lower, {"protein", "lunch", "food", "hungry", "cook"})) {
        return "Restaurant move: I need your city or neighborhood for real local picks. Without that, I'd look for a taco spot with grilled chicken/steak, Mediterranean shawarma, a burger place, poke, BBQ, or a deli with a solid turkey/chicken sandwich.";
    }
    if (ContainsAnyLocal(lower, {"hungry", "late lunch", "lunch"}) &&
        ContainsAnyLocal(lower, {"protein", "food", "ideas"})) {
        return "Late lunch with protein: tacos with chicken or steak, a burger, shawarma bowl, poke bowl, rotisserie chicken, deli sandwich, or eggs and toast if you want easy. If you want restaurant mode, tell me your city or neighborhood.";
    }
    if (ContainsAnyLocal(lower, {"paper", "folded", "origami"}) &&
        ContainsAnyLocal(lower, {"bat", "airplane", "make", "show"})) {
        return "Yep. Start like a paper airplane: fold lengthwise, open it, fold the top corners to the center, then fold those edges in again. Fold it closed. Make wide wings, then bend the wing tips slightly down so it reads like bat wings. Add little ears by folding two tiny points near the nose.";
    }
    return {};
}

std::string RelationshipStoryInvite(const std::string& message, const Intent& intent) {
    if (intent.mode != IntentMode::Reflective && intent.mode != IntentMode::Counseling) return {};
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
        "mind", "body", "memory", "memories", "feelings", "emotions"
    };
    if (ContainsAnyLocal(relation, non_people)) return {};
    return "Tell me about your " + relation + ".";
}

std::string ChargedDoorMirror(const std::string& message, const Intent& intent) {
    if (intent.mode != IntentMode::Reflective && intent.mode != IntentMode::Counseling) return {};
    const std::string lower = Lower(message);
    if (WordCount(lower) < 6 &&
        !ContainsAnyLocal(lower, {"positive", "burn the ships", "ships"})) return {};

    struct Door {
        const char* needle;
        const char* mirror;
    };
    static const Door doors[] = {
        {"positive", "What's making it positive?"},
        {"burn the ships", "What ship?"},
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
    return {};
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
    memory_.ImportCounselingSource(repo_root_ / "docs" / "source" / "MasterDocument.txt");
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
    Intent intent = ClassifyIntent(user_message);
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
    if (IsEmotionalContinuation(user_message, intent, recent)) {
        intent.mode = IntentMode::Reflective;
        intent.emotional = true;
    }
    const auto relevant = memory_.RetrieveRelevant(user_message, 5);
    const auto sources = intent.crisis ? std::vector<SourceChunk>{}
                                       : memory_.SearchCounselingSources(user_message, false, 4);
    const auto emotions = memory_.RecentEmotionSignals(8);
    memory_.SaveMessage(session_id, "user", user_message);
    memory_.SaveEmotion(session_id, user_message, signal);
    std::string answer = RelationshipStoryInvite(user_message, intent);
    if (answer.empty()) {
        answer = ChargedDoorMirror(user_message, intent);
    }
    if (answer.empty()) {
        answer = QuickEverydayAnswer(user_message, intent);
    }
    if (intent.mode == IntentMode::DirectAnswer) {
        const std::string known = KnownDirectAnswer(user_message);
        if (!known.empty()) answer = known;
    }
    if (answer.empty()) {
        answer = gemma_.Chat(user_message, intent, recent, relevant, sources, emotions);
    }
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
