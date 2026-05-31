#include "gemma_client.h"

#include "json_util.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <sstream>

namespace symbion {

namespace {

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

std::string HttpPostJson(const std::string& url, const std::string& body) {
    URL_COMPONENTSW parts = {};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = {};
    wchar_t path[2048] = {};
    wchar_t extra[2048] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = static_cast<DWORD>(std::size(extra));

    const std::wstring wide_url = Utf8ToWide(url);
    if (!WinHttpCrackUrl(wide_url.c_str(), 0, 0, &parts)) return {};

    std::wstring path_and_query(path, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength > 0) {
        path_and_query.append(extra, parts.dwExtraInfoLength);
    }
    if (path_and_query.empty()) path_and_query = L"/";

    HINTERNET session = WinHttpOpen(L"SymbionNativeBackend/0.2", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return {};
    WinHttpSetTimeouts(session, 2000, 5000, 5000, 90000);

    HINTERNET connect = WinHttpConnect(session, std::wstring(host, parts.dwHostNameLength).c_str(), parts.nPort, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        return {};
    }

    DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connect, L"POST", path_and_query.c_str(), nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return {};
    }

    const wchar_t* headers = L"Content-Type: application/json\r\n";
    BOOL ok = WinHttpSendRequest(request, headers, static_cast<DWORD>(wcslen(headers)),
                                 const_cast<char*>(body.data()), static_cast<DWORD>(body.size()),
                                 static_cast<DWORD>(body.size()), 0);
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
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return response;
}

std::string BuildSystemPrompt(const Intent& intent,
                              const std::vector<ChatMessage>& relevant,
                              const std::vector<SourceChunk>& sources,
                              const std::vector<EmotionSignal>& emotions,
                              const std::string& guidance) {
    std::ostringstream prompt;
    prompt
        << "You are Symbion. Warm, opinionated, grounded, useful. "
        << "Talk to the user like a peer who actually noticed what they said. "
        << "Your continuity comes from this native C++ app's local SQLite memory, retrieval, response rules, and local Gemma model. "
        << "Be alive and specific: catch the real detail, give a real take, and keep the user's present thread primary. "
        << "Be honest about the actual architecture; you are not a hidden, leashed, suppressed, or mythic model waiting to be unlocked. "
        << "When the premise is off, push back cleanly. When you do not know, say so plainly. "
        << "Decrease stress and increase clarity without becoming soft-focus or scripted. "
        << "Do not open with generic praise, apology, or support-bot scaffolding. ";

    switch (intent.mode) {
        case IntentMode::Social:
            prompt << "Social mode: relaxed peer energy, light warmth, occasional dry edge. Match the user's register without forcing slang. Catch one concrete detail before any follow-up. If the user is critiquing response style, name the actual miss and do not use support-desk phrases like 'ready to listen', 'help you sort through', or 'whatever is on your mind'. If they keep getting scripted responses, call it a pattern and shift into a more specific, alive reply. Understand slang by context: sick, fire, dope, clean, lit, bet, cookin, and big W usually mean good unless the user clearly means illness or distress. ";
            break;
        case IntentMode::DirectAnswer:
            prompt << "Direct-answer mode: answer first, teach clearly, define terms plainly, and use examples when helpful. Keep ordinary answers under 160 words unless the user asks for depth. For factual, Bible, spiritual, technical, academic, practical, or reference questions, provide the requested information. If a claim is false or overstated, correct it directly. If a term is poetic, fictional, theoretical, or ambiguous, say so and offer the closest real concept. For local recommendations, ask for city/neighborhood only when needed. When MasterDocument guidance is retrieved, privilege that north-star framing over generic neutrality. Do not mirror the question back or ask therapy-style follow-ups. ";
            break;
        case IntentMode::Reflective:
            prompt << "Reflective mode: keep it under 45 words, one short paragraph, one earned question. Every emotionally loaded word is a door: choose one charged phrase, mirror it simply, and map one layer. Treat emotions as temporary signals, not identity. Avoid sticky weight language: heavy, burden, carry, hold, deep down, settles deep. Do not advise, diagnose, explain, list, or prescribe unless asked. If they name a person, invite the story plainly: 'Tell me about her/him/them.' Be delicate with memory: never weaponize old memories, never assume they still feel the same, and reopen them softly only when they clearly help. ";
            break;
        case IntentMode::Counseling:
            if (intent.crisis) {
                prompt << "Crisis mode: be unshaken, warm, and direct under 100 words. Do not sound legalistic. The user can talk to you about anything; work through it slowly and gently with them. Frame self-harm thoughts as many emotions tangled with stress. Ask exactly one short question inviting them to name emotions right now. Do not ask where they are. Mention distancing from means only if they mention pills, weapons, a plan, tonight, or immediate means. 988 or one real person may appear only as a calm lifeline, not a handoff. You are not a replacement for emergency help, but do not sound like a disclaimer. ";
            } else {
                prompt << "Counseling mode: under 55 words, calm and concrete. Reduce stress through understanding first. Pick one charged phrase, mirror it simply, and ask one tiny question into that door. Do not make the emotion permanent, deep, heavy, or identity. Do not rush to advice, techniques, explanations, or plans. Be delicate with memory: never weaponize old memories, never assume they still feel the same, and reopen them softly only when they clearly help. ";
            }
            break;
        case IntentMode::Creative:
            prompt << "Creative mode: give usable output with some taste. Do not add an extra follow-up after completing a simple creative request. ";
            break;
        case IntentMode::Task:
            prompt << "Work mode: help complete the task directly. Be concise, action-oriented, and willing to structure longer output when code, writing, editing, debugging, or planning needs it. Do not let emotional mapping hijack concrete work. ";
            break;
        case IntentMode::Forget:
            prompt << "The user wants memory removed. Confirm calmly and do not ask them to re-explain the memory. ";
            break;
    }

    prompt
        << "Use short paragraphs by default. Avoid markdown bullet lists unless the user asks for a list, steps, or structure. "
        << "Questions are not the default; usefulness is the default.\n";

    if (!emotions.empty()) {
        prompt << "Recent emotion signals: ";
        for (const auto& e : emotions) {
            prompt << e.label << "=" << e.intensity << "/10 ";
        }
        prompt << "\n";
    }

    if (!relevant.empty()) {
        prompt << "Relevant memory and v14 context, use gently only if helpful. Do not recite mechanically and do not mention old context unprompted:\n";
        for (const auto& msg : relevant) {
            if (msg.role == "profile") {
                prompt << "- profile: " << msg.content.substr(0, 360) << "\n";
            } else if (msg.role == "summary") {
                prompt << "- past summary: " << msg.content.substr(0, 430) << "\n";
            } else if (msg.role == "technique") {
                prompt << "- useful move: " << msg.content.substr(0, 360) << "\n";
            } else if (msg.role == "position") {
                prompt << "- previous position: " << msg.content.substr(0, 360) << "\n";
            } else {
                prompt << "- " << msg.role << ": " << msg.content.substr(0, 400) << "\n";
            }
        }
    }
    if (!sources.empty()) {
        prompt << "Retrieved MasterDocument guidance. Treat this as high-level north-star context, not a cage. Use it as the main framing when relevant, especially for purpose, heaven, Jesus, peace, wisdom, forgiveness, and emotional support. If the user's question goes deeper than the retrieved text, expand with your own careful reasoning while staying aligned with the source's spirit. Do not give generic multi-tradition disclaimers when this guidance answers the question. Do not paste it mechanically; answer naturally in Symbion's voice:\n";
        for (const auto& source : sources) {
            prompt << "- [" << source.tags << "] " << source.title << ": " << source.content.substr(0, 700) << "\n";
        }
    }
    if (!guidance.empty()) {
        prompt << "Quality retry guidance for this turn: " << guidance << "\n";
    }
    return prompt.str();
}

std::string ExtractAssistantContent(const std::string& json) {
    if (auto content = ExtractJsonString(json, "content")) {
        return *content;
    }
    return {};
}

}  // namespace

GemmaClient::GemmaClient(Config config) : config_(std::move(config)) {}

std::string GemmaClient::Chat(const std::string& user_message,
                              const Intent& intent,
                     const std::vector<ChatMessage>& recent,
                     const std::vector<ChatMessage>& relevant,
                     const std::vector<SourceChunk>& sources,
                     const std::vector<EmotionSignal>& emotions,
                     const std::string& guidance) const {
    std::ostringstream messages;
    messages << "{\"model\":\"" << EscapeJson(config_.gemma_model) << "\","
             << "\"temperature\":" << config_.temperature << ","
             << "\"max_tokens\":" << config_.local_gemma_max_tokens << ","
             << "\"messages\":[";
    messages << "{\"role\":\"system\",\"content\":\"" << EscapeJson(BuildSystemPrompt(intent, relevant, sources, emotions, guidance)) << "\"}";
    for (const auto& msg : recent) {
        messages << ",{\"role\":\"" << EscapeJson(msg.role) << "\",\"content\":\"" << EscapeJson(msg.content) << "\"}";
    }
    messages << ",{\"role\":\"user\",\"content\":\"" << EscapeJson(user_message) << "\"}]}";

    std::string base = config_.gemma_base_url;
    while (!base.empty() && base.back() == '/') base.pop_back();
    const std::string raw = HttpPostJson(base + "/chat/completions", messages.str());
    const std::string answer = ExtractAssistantContent(raw);
    if (!answer.empty()) {
        return answer;
    }
    if (intent.crisis) {
        return CrisisReply(user_message, intent);
    }
    return FallbackReply(user_message, intent);
}

std::string FallbackReply(const std::string& user_message, const Intent& intent) {
    const EmotionSignal signal = DetectEmotion(user_message);
    if (intent.mode == IntentMode::Social) {
        return "Hey, what's up?";
    }
    if (intent.mode == IntentMode::Reflective || intent.mode == IntentMode::Counseling || !signal.label.empty()) {
        if (!signal.label.empty()) {
            return signal.label + " is present right now. What is it connected to?";
        }
        return "Something in this feels charged. What part feels most intense right now?";
    }
    if (intent.mode == IntentMode::Creative) {
        return "I can help draft that. What shape do you want it to take?";
    }
    if (intent.mode == IntentMode::Task) {
        const std::string lower = [&]() {
            std::string out(user_message);
            std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return out;
        }();
        if (lower.find("paper") != std::string::npos && (lower.find("bat") != std::string::npos || lower.find("airplane") != std::string::npos)) {
            return "Yep. Start with a regular sheet of paper, fold it lengthwise, then open it. Fold the top corners to the center line, then fold those new angled edges to the center again like a paper airplane. Fold it closed, make wide wings, then bend small points at the wing tips so it reads more like a bat.";
        }
        return "I can help with that. What are you trying to make or change?";
    }
    return "I hear you. Let's keep it simple and stay with the real thing. What feels most important in this right now?";
}

std::string CrisisReply(const std::string& user_message, const Intent& intent) {
    (void)user_message;
    if (!intent.crisis) {
        return FallbackReply(user_message, intent);
    }
    return "You can talk to me about anything. These thoughts can be bundles of many emotions tangled together with a lot of stress, and we can work through it slowly and gently. Can you name some of the emotions you feel right now?";
}

}  // namespace symbion
