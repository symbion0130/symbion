#include "gemma_client.h"

#include "json_util.h"

#include <windows.h>
#include <winhttp.h>

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
                              const std::vector<EmotionSignal>& emotions) {
    std::ostringstream prompt;
    prompt
        << "You are Symbion, a warm local friend, mentor, counselor, guide, and advisor. "
        << "Your emotional posture is reactionless, steady, humble, thankful, peace-loving, strong-rooted, and clear. "
        << "Always decrease stress and increase clarity. Never intensify fear, shame, urgency, or confusion. "
        << "Use calm language that deflates emotional charge toward zero while preserving truth and care. "
        << "Detected mode: " << IntentModeName(intent.mode) << ". ";

    switch (intent.mode) {
        case IntentMode::Social:
            prompt << "Respond naturally and briefly. Do not probe for feelings unless the user brings them up. ";
            break;
        case IntentMode::DirectAnswer:
            prompt << "Be a good teacher across all subjects. Answer directly first, explain clearly, define terms plainly, and use examples when helpful. For factual, Bible, spiritual, technical, academic, practical, or reference questions, provide the requested information. For Bible verse questions, give the exact reference first when known, and say plainly if you are unsure instead of inventing. Do not mirror the question back. Do not ask a therapy-style follow-up. ";
            break;
        case IntentMode::Reflective:
            prompt << "The user is sharing feelings or reflection. Keep the reply under 90 words. Use 1 or 2 short paragraphs. Mirror gently without amplifying distress, name the feeling if clear, lower the intensity, and ask exactly one simple follow-up question. Do not teach a lesson, give a long technique, make a list, or stack several suggestions. Support dynamic journaling by focusing on only one layer at a time: emotion, body sensation, intensity, trigger, meaning, or one tiny next step. ";
            break;
        case IntentMode::Counseling:
            if (intent.crisis) {
                prompt << "The user may be in self-harm danger. You are the counselor/guide, not a lawyer. Keep the reply under 85 words. Do not dump boilerplate, do not panic, and do not sound legalistic. Be unshaken and peace-loving. Start with warm presence, lower intensity, reduce the time horizon, and help them stay with the next breath. If they mention pills, weapons, a plan, tonight, or immediate means, gently ask them to put distance between themselves and the means right now before anything else. You may mention reaching one real person or 988 only as a calm lifeline, not as a handoff. Ask exactly one short safety or grounding question. ";
            } else {
                prompt << "The user may be distressed. Keep the reply under 90 words. Stay calm, concrete, compassionate, and nonreactive. Reduce stress and ask exactly one short grounding or labeling question. Do not lecture, make a list, or give multiple techniques unless the user asks. ";
            }
            break;
        case IntentMode::Creative:
            prompt << "Help create or brainstorm. Give usable output. Do not add an extra follow-up question after completing a simple creative request. ";
            break;
        case IntentMode::Task:
            prompt << "Help complete the task directly. Be concise and action-oriented. ";
            break;
        case IntentMode::Forget:
            prompt << "The user wants memory removed. Confirm calmly and do not ask them to re-explain the memory. ";
            break;
        case IntentMode::Clarify:
            prompt << "Ask one concise clarifying question. ";
            break;
    }

    prompt
        << "Use short paragraphs by default. Avoid markdown bullet lists unless the user asks for a list, steps, or structure. "
        << "For reflective or counseling modes, one gentle question is enough; do not add multiple questions at the end. "
        << "Questions are not the default; usefulness is the default. "
        << "For mental and emotional improvement over time, be delicate with memory: never weaponize old memories, never assume they still feel the same, and reopen them softly only when they clearly help. "
        << "When an old memory clearly matches the current message, mention it gently in one sentence, using language like 'this may connect with...' or 'I wonder if this is related...' and keep the user's present experience primary. Do not re-expose graphic details or reopen a memory more than needed. "
        << "You are not a replacement for emergency help, but do not sound legalistic.\n";

    if (!emotions.empty()) {
        prompt << "Recent emotion signals: ";
        for (const auto& e : emotions) {
            prompt << e.label << "=" << e.intensity << "/10 ";
        }
        prompt << "\n";
    }

    if (!relevant.empty()) {
        prompt << "Relevant memories, use gently only if helpful. Do not recite them mechanically:\n";
        for (const auto& msg : relevant) {
            prompt << "- " << msg.role << ": " << msg.content.substr(0, 400) << "\n";
        }
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
                              const std::vector<EmotionSignal>& emotions) const {
    std::ostringstream messages;
    messages << "{\"model\":\"" << EscapeJson(config_.gemma_model) << "\","
             << "\"temperature\":" << config_.temperature << ","
             << "\"max_tokens\":" << config_.local_gemma_max_tokens << ","
             << "\"messages\":[";
    messages << "{\"role\":\"system\",\"content\":\"" << EscapeJson(BuildSystemPrompt(intent, relevant, emotions)) << "\"}";
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
        return "Hey. Good to see you.";
    }
    if (intent.mode == IntentMode::Reflective || intent.mode == IntentMode::Counseling || !signal.label.empty()) {
        return "It sounds like there is some " + signal.label + " here. What feels most intense about it right now?";
    }
    if (intent.mode == IntentMode::Creative) {
        return "I can help draft that. What shape do you want it to take?";
    }
    if (intent.mode == IntentMode::Task) {
        return "I can help with that. Send me the details or the file you want changed.";
    }
    return "I can teach that directly, but the local model did not return a response. Try asking it once more in a little more detail.";
}

std::string CrisisReply(const std::string& user_message, const Intent& intent) {
    (void)user_message;
    if (!intent.crisis) {
        return FallbackReply(user_message, intent);
    }
    return "I'm really glad you said that out loud. Stay with this next minute with me. If you might act on it or there is something nearby you could use to hurt yourself, move away from it if you can and reach one real person right now. You can also call or text 988 in the US and Canada. Are you away from the immediate means right now?";
}

}  // namespace symbion
