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

std::string BuildSystemPrompt(const std::vector<ChatMessage>& relevant,
                              const std::vector<EmotionSignal>& emotions) {
    std::ostringstream prompt;
    prompt
        << "You are Symbion, a warm local friend, mentor, counselor, guide, and advisor. "
        << "Do not give bullet lists unless the user explicitly asks. Ask one simple question at a time. "
        << "Mirror the user's words gently, go deeper, and help map emotions and intensity. "
        << "For intense statements, stay calm and ask a short labeling or mirroring question. "
        << "You are not a replacement for emergency help, but do not sound legalistic.\n";

    if (!emotions.empty()) {
        prompt << "Recent emotion signals: ";
        for (const auto& e : emotions) {
            prompt << e.label << "=" << e.intensity << "/10 ";
        }
        prompt << "\n";
    }

    if (!relevant.empty()) {
        prompt << "Relevant memories, use only if helpful:\n";
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
                              const std::vector<ChatMessage>& recent,
                              const std::vector<ChatMessage>& relevant,
                              const std::vector<EmotionSignal>& emotions) const {
    std::ostringstream messages;
    messages << "{\"model\":\"" << EscapeJson(config_.gemma_model) << "\","
             << "\"temperature\":" << config_.temperature << ","
             << "\"max_tokens\":" << config_.local_gemma_max_tokens << ","
             << "\"messages\":[";
    messages << "{\"role\":\"system\",\"content\":\"" << EscapeJson(BuildSystemPrompt(relevant, emotions)) << "\"}";
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
    return FallbackCounselorReply(user_message);
}

std::string FallbackCounselorReply(const std::string& user_message) {
    const EmotionSignal signal = DetectEmotion(user_message);
    if (!signal.label.empty()) {
        return "It sounds like there is some " + signal.label + " here. What feels most intense about it right now?";
    }
    return "What feels most important in that right now?";
}

}  // namespace symbion
