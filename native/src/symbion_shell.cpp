#include "symbion_shell.h"

#include "resource.h"

#include <objbase.h>
#include <shellapi.h>
#include <winhttp.h>
#include <windows.h>
#include <wrl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef SYMBION_NATIVE_DEFAULT_URL
#define SYMBION_NATIVE_DEFAULT_URL L"http://127.0.0.1:8000/"
#endif

#if defined(__has_include)
#if __has_include(<WebView2.h>)
#define SYMBION_NATIVE_HAS_WEBVIEW2_HEADERS 1
#include <WebView2.h>
#else
#define SYMBION_NATIVE_HAS_WEBVIEW2_HEADERS 0
#endif
#else
#define SYMBION_NATIVE_HAS_WEBVIEW2_HEADERS 0
#endif

namespace {

constexpr UINT_PTR kStatusTimer = 1;
constexpr UINT kStatusPollMs = 30000;
constexpr UINT kTrayMessage = WM_APP + 41;

constexpr WORD kCmdOpen = 1001;
constexpr WORD kCmdHide = 1002;
constexpr WORD kCmdQuit = 1003;
constexpr WORD kCmdRefreshStatus = 1004;
constexpr WORD kCmdRestartBackend = 1005;
constexpr WORD kCmdStartGemma = 1006;
constexpr WORD kCmdStopGemma = 1007;
constexpr WORD kCmdAnalytics = 1008;
constexpr WORD kCmdReload = 1009;
constexpr WORD kCmdDevTools = 1010;
constexpr WORD kCmdProviderBase = 1100;

struct ProviderChoice {
    const wchar_t* id;
    const wchar_t* label;
};

constexpr std::array<ProviderChoice, 8> kProviders = {{
    {L"local_gemma", L"Local Gemma - CodeCat llama.cpp"},
    {L"anthropic", L"Anthropic"},
    {L"groq", L"Groq"},
    {L"kimi", L"Moonshot/Kimi"},
    {L"ollama", L"Ollama"},
    {L"openai", L"OpenAI"},
    {L"deepseek", L"DeepSeek"},
    {L"hf_router", L"Hugging Face Router"},
}};

std::wstring FormatHresult(HRESULT result) {
    wchar_t buffer[32] = {};
    swprintf_s(buffer, L"0x%08X", static_cast<unsigned int>(result));
    return buffer;
}

std::wstring ReadEnvironmentString(const wchar_t* name) {
    DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0) {
        return {};
    }
    std::wstring value(needed, L'\0');
    DWORD written = GetEnvironmentVariableW(name, value.data(), needed);
    if (written == 0) {
        return {};
    }
    value.resize(written);
    return value;
}

std::wstring ReadEnvironmentUrl() {
    return ReadEnvironmentString(L"SYMBION_WEBVIEW2_URL");
}

bool StartsWith(std::wstring_view value, std::wstring_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool StartsWithNarrow(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

std::wstring Utf8ToWide(std::string_view value) {
    if (value.empty()) {
        return {};
    }
    int count = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) {
        return {};
    }
    std::wstring out(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), count);
    return out;
}

std::string WideToUtf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    int count = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), count, nullptr, nullptr);
    return out;
}

std::string ReadTextFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

bool WriteTextFile(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }
    output << text;
    return static_cast<bool>(output);
}

std::string JsonUnescape(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        char c = value[i];
        if (c != '\\' || i + 1 >= value.size()) {
            out.push_back(c);
            continue;
        }
        char next = value[++i];
        switch (next) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            default: out.push_back(next); break;
        }
    }
    return out;
}

std::string JsonEscape(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char c : value) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

std::optional<std::string> ExtractJsonString(const std::string& json, const std::string& key) {
    const std::regex pattern("\"" + key + "\"\\s*:\\s*\"((?:\\\\.|[^\"])*)\"");
    std::smatch match;
    if (!std::regex_search(json, match, pattern)) {
        return std::nullopt;
    }
    return JsonUnescape(match[1].str());
}

std::optional<int> ExtractJsonInt(const std::string& json, const std::string& key) {
    const std::regex pattern("\"" + key + "\"\\s*:\\s*(-?\\d+)");
    std::smatch match;
    if (!std::regex_search(json, match, pattern)) {
        return std::nullopt;
    }
    return std::stoi(match[1].str());
}

std::optional<bool> ExtractJsonBool(const std::string& json, const std::string& key) {
    const std::regex pattern("\"" + key + "\"\\s*:\\s*(true|false)");
    std::smatch match;
    if (!std::regex_search(json, match, pattern)) {
        return std::nullopt;
    }
    return match[1].str() == "true";
}

bool ReplaceOrInsertJsonString(std::string& json, const std::string& key, const std::string& value) {
    const std::regex existing("\"" + key + "\"\\s*:\\s*\"(?:\\\\.|[^\"])*\"");
    const std::string replacement = "\"" + key + "\": \"" + JsonEscape(value) + "\"";
    if (std::regex_search(json, existing)) {
        json = std::regex_replace(json, existing, replacement, std::regex_constants::format_first_only);
        return true;
    }

    const size_t pos = json.find_last_of('}');
    if (pos == std::string::npos) {
        return false;
    }
    const bool needs_comma = json.rfind('{', pos) != pos && json[pos - 1] != '{';
    json.insert(pos, std::string(needs_comma ? ",\n  " : "\n  ") + replacement + "\n");
    return true;
}

std::filesystem::path ModuleDirectory() {
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    while (len == buffer.size() && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
        buffer.resize(buffer.size() * 2);
        len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    }
    buffer.resize(len);
    return std::filesystem::path(buffer).parent_path();
}

bool LooksLikeRepo(const std::filesystem::path& path) {
    std::error_code ec;
    return !path.empty() &&
           (std::filesystem::exists(path / L"config" / L"symbion.json", ec) ||
            std::filesystem::exists(path / L"symbion.json", ec)) &&
           std::filesystem::exists(path / L"native" / L"CMakeLists.txt", ec);
}

std::wstring ResolveRepoRoot() {
    std::vector<std::filesystem::path> candidates;
    const std::wstring env = ReadEnvironmentString(L"SYMBION_REPO");
    if (!env.empty()) {
        candidates.emplace_back(env);
    }

    std::error_code ec;
    candidates.emplace_back(std::filesystem::current_path(ec));

    std::filesystem::path module = ModuleDirectory();
    for (std::filesystem::path cursor = module; !cursor.empty(); cursor = cursor.parent_path()) {
        candidates.push_back(cursor);
        if (cursor == cursor.root_path()) {
            break;
        }
    }

    const std::wstring user_profile = ReadEnvironmentString(L"USERPROFILE");
    if (!user_profile.empty()) {
        candidates.emplace_back(std::filesystem::path(user_profile) / L"symbion");
        candidates.emplace_back(std::filesystem::path(user_profile) / L"SourceCode" / L"symbion");
    }

    for (const auto& candidate : candidates) {
        if (LooksLikeRepo(candidate)) {
            return candidate.wstring();
        }
    }
    return {};
}

std::wstring QuoteArg(std::wstring_view arg) {
    std::wstring out = L"\"";
    for (wchar_t c : arg) {
        if (c == L'"') {
            out += L"\\\"";
        } else {
            out.push_back(c);
        }
    }
    out += L"\"";
    return out;
}

bool LaunchProcess(std::wstring command_line,
                   const std::wstring& cwd,
                   DWORD creation_flags,
                   PROCESS_INFORMATION* process_info) {
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION local_process = {};
    PROCESS_INFORMATION* target = process_info ? process_info : &local_process;
    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');
    BOOL ok = CreateProcessW(
        nullptr,
        mutable_command.data(),
        nullptr,
        nullptr,
        FALSE,
        creation_flags,
        nullptr,
        cwd.empty() ? nullptr : cwd.c_str(),
        &startup,
        target);
    if (!ok) {
        return false;
    }
    if (!process_info) {
        CloseHandle(local_process.hThread);
        CloseHandle(local_process.hProcess);
    }
    return true;
}

void CloseProcessInfo(PROCESS_INFORMATION& process) {
    if (process.hThread) {
        CloseHandle(process.hThread);
        process.hThread = nullptr;
    }
    if (process.hProcess) {
        CloseHandle(process.hProcess);
        process.hProcess = nullptr;
    }
    process.dwProcessId = 0;
    process.dwThreadId = 0;
}

bool IsProcessStillRunning(const PROCESS_INFORMATION& process) {
    if (!process.hProcess) {
        return false;
    }
    DWORD code = 0;
    return GetExitCodeProcess(process.hProcess, &code) && code == STILL_ACTIVE;
}

void KillProcessTree(DWORD pid) {
    if (pid == 0) {
        return;
    }
    const std::wstring command = L"taskkill.exe /PID " + std::to_wstring(pid) + L" /T /F";
    PROCESS_INFORMATION taskkill = {};
    if (LaunchProcess(command, L"", CREATE_NO_WINDOW, &taskkill)) {
        WaitForSingleObject(taskkill.hProcess, 8000);
        CloseProcessInfo(taskkill);
    }
}

struct HttpResponse {
    DWORD status = 0;
    std::string body;
};

std::optional<HttpResponse> HttpGet(const std::wstring& url,
                                    const std::wstring& api_key = L"",
                                    DWORD timeout_ms = 2000) {
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

    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) {
        return std::nullopt;
    }

    std::wstring path_and_query(path, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength > 0 && parts.lpszExtraInfo) {
        path_and_query.append(extra, parts.dwExtraInfoLength);
    }
    if (path_and_query.empty()) {
        path_and_query = L"/";
    }

    HINTERNET session = WinHttpOpen(
        L"SymbionNative/0.1",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!session) {
        return std::nullopt;
    }
    WinHttpSetTimeouts(session, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

    HINTERNET connect = WinHttpConnect(session, std::wstring(host, parts.dwHostNameLength).c_str(), parts.nPort, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        return std::nullopt;
    }

    DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connect, L"GET", path_and_query.c_str(), nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return std::nullopt;
    }

    std::wstring headers;
    if (!api_key.empty()) {
        headers = L"X-API-Key: " + api_key + L"\r\n";
    }

    BOOL sent = WinHttpSendRequest(
        request,
        headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
        headers.empty() ? 0 : static_cast<DWORD>(headers.size()),
        WINHTTP_NO_REQUEST_DATA,
        0,
        0,
        0);
    if (!sent || !WinHttpReceiveResponse(request, nullptr)) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return std::nullopt;
    }

    HttpResponse response;
    DWORD status_size = sizeof(response.status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &response.status, &status_size, WINHTTP_NO_HEADER_INDEX);

    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available) || available == 0) {
            break;
        }
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0) {
            break;
        }
        chunk.resize(read);
        response.body += chunk;
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return response;
}

std::wstring MakeServiceBase(const std::wstring& url) {
    URL_COMPONENTSW parts = {};
    parts.dwStructSize = sizeof(parts);
    wchar_t scheme[16] = {};
    wchar_t host[256] = {};
    parts.lpszScheme = scheme;
    parts.dwSchemeLength = static_cast<DWORD>(std::size(scheme));
    parts.lpszHostName = host;
    parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) {
        return L"http://127.0.0.1:8000";
    }
    std::wstring base(scheme, parts.dwSchemeLength);
    base += L"://";
    base.append(host, parts.dwHostNameLength);
    const bool default_port =
        (parts.nScheme == INTERNET_SCHEME_HTTP && parts.nPort == 80) ||
        (parts.nScheme == INTERNET_SCHEME_HTTPS && parts.nPort == 443);
    if (!default_port) {
        base += L":" + std::to_wstring(parts.nPort);
    }
    return base;
}

std::wstring ExtractJsonFieldForDisplay(const std::string& json, const std::string& key, const std::wstring& fallback) {
    if (auto value = ExtractJsonString(json, key)) {
        return Utf8ToWide(*value);
    }
    if (auto value = ExtractJsonInt(json, key)) {
        return std::to_wstring(*value);
    }
    return fallback;
}

std::wstring ReadEnvApiKey(const std::filesystem::path& repo_root) {
    std::error_code ec;
    const auto env_path = repo_root / L".env";
    if (!std::filesystem::exists(env_path, ec)) {
        return {};
    }
    const std::string raw = ReadTextFile(env_path);
    std::istringstream lines(raw);
    std::string line;
    while (std::getline(lines, line)) {
        const std::string prefix = "SYMBION_API_KEY";
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos || !StartsWithNarrow(std::string_view(line).substr(start), prefix)) {
            continue;
        }
        size_t equals = line.find('=', start + prefix.size());
        if (equals == std::string::npos) {
            continue;
        }
        std::string value = line.substr(equals + 1);
        size_t first = value.find_first_not_of(" \t'\"");
        size_t last = value.find_last_not_of(" \t\r\n'\"");
        if (first == std::string::npos || last == std::string::npos || last < first) {
            return {};
        }
        return Utf8ToWide(value.substr(first, last - first + 1));
    }
    return {};
}

std::wstring NormalizeProvider(std::wstring provider) {
    std::transform(provider.begin(), provider.end(), provider.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(towlower(c));
    });
    return provider.empty() ? L"local_gemma" : provider;
}

}  // namespace

struct SymbionShell::WebViewState {
#if SYMBION_NATIVE_HAS_WEBVIEW2_HEADERS
    using CreateEnvironmentFn = HRESULT(STDAPICALLTYPE*)(
        PCWSTR browser_executable_folder,
        PCWSTR user_data_folder,
        ICoreWebView2EnvironmentOptions* environment_options,
        ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* environment_created_handler);

    HMODULE loader = nullptr;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller;
    Microsoft::WRL::ComPtr<ICoreWebView2> webview;
    EventRegistrationToken web_resource_token = {};
#endif
};

SymbionShell::SymbionShell(std::wstring initial_url)
    : url_(std::move(initial_url)),
      status_(L"Starting Symbion native shell..."),
      webview_(std::make_unique<WebViewState>()) {}

SymbionShell::~SymbionShell() {
    RemoveTray();
    StopBackend();
    CloseProcessInfo(gemma_process_);
    if (single_instance_mutex_) {
        CloseHandle(single_instance_mutex_);
        single_instance_mutex_ = nullptr;
    }
#if SYMBION_NATIVE_HAS_WEBVIEW2_HEADERS
    if (webview_) {
        webview_->webview.Reset();
        webview_->controller.Reset();
        if (webview_->loader) {
            FreeLibrary(webview_->loader);
            webview_->loader = nullptr;
        }
    }
#endif
}

int SymbionShell::Run(HINSTANCE instance, int show_command) {
    instance_ = instance;
    single_instance_message_ = RegisterWindowMessageW(L"SymbionNativeShowWindow");
    if (!AcquireSingleInstance()) {
        FocusExistingInstance();
        return 0;
    }

    ResolveRuntimeConfiguration();

    const HRESULT coinit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(coinit) && coinit != RPC_E_CHANGED_MODE) {
        MessageBoxW(nullptr, L"Symbion native shell could not initialize COM.", L"Symbion", MB_ICONERROR);
        return 1;
    }

    if (!ProbeBackend()) {
        if (!repo_root_.empty()) {
            StartBackend();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (std::chrono::steady_clock::now() < deadline) {
                if (ProbeBackend()) {
                    break;
                }
                Sleep(300);
            }
        } else {
            backend_status_ = L"repo not found";
        }
    }

    if (!RegisterWindowClass(instance) || !CreateMainWindow(instance, show_command)) {
        if (SUCCEEDED(coinit)) {
            CoUninitialize();
        }
        return 1;
    }

    BuildMenu();
    if (tray_enabled_) {
        StartTray();
    }
    RefreshRuntimeStatus();
    SetTimer(hwnd_, kStatusTimer, kStatusPollMs, nullptr);

    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (SUCCEEDED(coinit)) {
        CoUninitialize();
    }

    return static_cast<int>(message.wParam);
}

bool SymbionShell::AcquireSingleInstance() {
    single_instance_mutex_ = CreateMutexW(nullptr, TRUE, L"Local\\SymbionNativeShell");
    return single_instance_mutex_ && GetLastError() != ERROR_ALREADY_EXISTS;
}

void SymbionShell::FocusExistingInstance() {
    HWND existing = FindWindowW(kWindowClassName, nullptr);
    if (existing) {
        if (single_instance_message_) {
            PostMessageW(existing, single_instance_message_, 0, 0);
        } else {
            ShowWindow(existing, SW_SHOW);
            SetForegroundWindow(existing);
        }
    }
}

void SymbionShell::ResolveRuntimeConfiguration() {
    repo_root_ = ResolveRepoRoot();
    if (!repo_root_.empty()) {
        const auto native_config = std::filesystem::path(repo_root_) / L"config" / L"symbion.json";
        const auto legacy_config = std::filesystem::path(repo_root_) / L"symbion.json";
        std::error_code ec;
        config_path_ = std::filesystem::exists(native_config, ec) ? native_config.wstring() : legacy_config.wstring();
        config_json_ = ReadTextFile(config_path_);
    }

    if (auto provider = ReadConfigString("llm_provider")) {
        provider_ = NormalizeProvider(*provider);
    } else {
        provider_ = L"local_gemma";
    }

    if (auto port = ReadConfigInt("web_port")) {
        web_port_ = *port;
    }
    if (auto native_tray = ReadConfigBool("native_tray_enabled")) {
        tray_enabled_ = *native_tray;
    }

    if (url_ == SYMBION_NATIVE_DEFAULT_URL && web_port_ != 8000) {
        url_ = L"http://127.0.0.1:" + std::to_wstring(web_port_) + L"/";
    }

    const std::wstring base = MakeServiceBase(url_);
    health_url_ = base + L"/health";
    local_gemma_status_url_ = base + L"/api/local-gemma/status";

    if (auto gemma_script = ReadConfigString("local_gemma_start_script")) {
        gemma_start_script_ = *gemma_script;
    } else {
        gemma_start_script_ = L"c:\\projects\\codecat\\runtime\\scripts\\start-gemma.ps1";
    }
    if (auto gemma_stop = ReadConfigString("local_gemma_stop_command")) {
        gemma_stop_command_ = *gemma_stop;
    } else {
        gemma_stop_command_ = ReadEnvironmentString(L"SYMBION_GEMMA_STOP_COMMAND");
    }

    if (auto base_url = ReadConfigString("local_gemma_base_url")) {
        local_gemma_models_url_ = *base_url;
        while (!local_gemma_models_url_.empty() && local_gemma_models_url_.back() == L'/') {
            local_gemma_models_url_.pop_back();
        }
        local_gemma_models_url_ += L"/models";
    } else {
        local_gemma_models_url_ = L"http://127.0.0.1:8088/v1/models";
    }

    if (auto api_key = ReadConfigString("api_key")) {
        api_key_ = *api_key;
    }
    if (api_key_.empty() && !repo_root_.empty()) {
        api_key_ = ReadEnvApiKey(repo_root_);
    }

    const std::filesystem::path module_dir = ModuleDirectory();
    const auto beside_shell = module_dir / L"symbion_backend.exe";
    const auto build_release = std::filesystem::path(repo_root_) / L"native" / L"build" / L"Release" / L"symbion_backend.exe";
    const auto build_debug = std::filesystem::path(repo_root_) / L"native" / L"build" / L"Debug" / L"symbion_backend.exe";
    std::error_code ec;
    if (std::filesystem::exists(beside_shell, ec)) {
        backend_path_ = beside_shell.wstring();
    } else if (!repo_root_.empty() && std::filesystem::exists(build_release, ec)) {
        backend_path_ = build_release.wstring();
    } else if (!repo_root_.empty() && std::filesystem::exists(build_debug, ec)) {
        backend_path_ = build_debug.wstring();
    } else {
        backend_path_ = L"symbion_backend.exe";
    }
}

std::optional<std::wstring> SymbionShell::ReadConfigString(const std::string& key) const {
    auto value = ExtractJsonString(config_json_, key);
    if (!value) {
        return std::nullopt;
    }
    return Utf8ToWide(*value);
}

std::optional<int> SymbionShell::ReadConfigInt(const std::string& key) const {
    return ExtractJsonInt(config_json_, key);
}

std::optional<bool> SymbionShell::ReadConfigBool(const std::string& key) const {
    return ExtractJsonBool(config_json_, key);
}

LRESULT CALLBACK SymbionShell::WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    SymbionShell* shell = reinterpret_cast<SymbionShell*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        shell = reinterpret_cast<SymbionShell*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(shell));
        shell->hwnd_ = hwnd;
    }

    if (shell) {
        return shell->HandleMessage(message, wparam, lparam);
    }

    return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT SymbionShell::HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == single_instance_message_) {
        ShowMainWindow();
        return 0;
    }

    switch (message) {
        case WM_CREATE:
            InitializeWebView();
            return 0;
        case WM_SIZE:
            ResizeWebView();
            return 0;
        case WM_COMMAND:
            HandleCommand(LOWORD(wparam));
            return 0;
        case WM_TIMER:
            if (wparam == kStatusTimer) {
                RefreshRuntimeStatus();
            }
            return 0;
        case WM_CLOSE:
            if (tray_enabled_ && !quitting_) {
                HideMainWindow();
                return 0;
            }
            DestroyWindow(hwnd_);
            return 0;
        case WM_PAINT:
            PaintPlaceholder();
            return 0;
        case kTrayMessage:
            if (lparam == WM_LBUTTONUP) {
                ToggleMainWindow();
            } else if (lparam == WM_RBUTTONUP || lparam == WM_CONTEXTMENU) {
                ShowTrayMenu();
            }
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd_, kStatusTimer);
            RemoveTray();
            quitting_ = true;
            StopBackend();
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd_, message, wparam, lparam);
    }
}

bool SymbionShell::RegisterWindowClass(HINSTANCE instance) {
    static HBRUSH dark_background = CreateSolidBrush(RGB(5, 5, 8));
    WNDCLASSEXW window_class = {};
    window_class.cbSize = sizeof(window_class);
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_SYMBION));
    window_class.hIconSm = static_cast<HICON>(LoadImageW(instance,
                                                         MAKEINTRESOURCEW(IDI_SYMBION),
                                                         IMAGE_ICON,
                                                         GetSystemMetrics(SM_CXSMICON),
                                                         GetSystemMetrics(SM_CYSMICON),
                                                         LR_DEFAULTCOLOR));
    window_class.hInstance = instance;
    window_class.lpfnWndProc = SymbionShell::WindowProc;
    window_class.lpszClassName = kWindowClassName;
    window_class.hbrBackground = dark_background;

    const ATOM atom = RegisterClassExW(&window_class);
    return atom != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool SymbionShell::CreateMainWindow(HINSTANCE instance, int show_command) {
    (void)show_command;
    constexpr int window_width = 1100;
    constexpr int window_height = 780;
    RECT work_area = {};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0)) {
        work_area = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    }
    const int work_width = work_area.right - work_area.left;
    const int work_height = work_area.bottom - work_area.top;
    const int x = work_area.left + std::max(0, (work_width - window_width) / 2);
    const int y = work_area.top + std::max(0, (work_height - window_height) / 2);

    HWND window = CreateWindowExW(
        0,
        kWindowClassName,
        L"Symbion",
        WS_OVERLAPPEDWINDOW,
        x,
        y,
        window_width,
        window_height,
        nullptr,
        nullptr,
        instance,
        this);

    if (!window) {
        return false;
    }

    HICON large_icon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_SYMBION));
    HICON small_icon = static_cast<HICON>(LoadImageW(instance,
                                                    MAKEINTRESOURCEW(IDI_SYMBION),
                                                    IMAGE_ICON,
                                                    GetSystemMetrics(SM_CXSMICON),
                                                    GetSystemMetrics(SM_CYSMICON),
                                                    LR_DEFAULTCOLOR));
    if (large_icon) SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(large_icon));
    if (small_icon) SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small_icon));

    ShowWindow(window, SW_SHOWNORMAL);
    UpdateWindow(window);
    return true;
}

void SymbionShell::BuildMenu() {
    HMENU old_menu = hwnd_ ? GetMenu(hwnd_) : nullptr;
    HMENU menu = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, kCmdOpen, L"Open Symbion");
    AppendMenuW(file, MF_STRING, kCmdHide, L"Hide to tray");
    AppendMenuW(file, MF_STRING, kCmdAnalytics, L"Open analytics");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, kCmdQuit, L"Quit Symbion");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"File");

    HMENU view = CreatePopupMenu();
    AppendMenuW(view, MF_STRING, kCmdReload, L"Reload");
    AppendMenuW(view, MF_STRING, kCmdDevTools, L"Open DevTools");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"View");

    HMENU runtime = CreatePopupMenu();
    AppendMenuW(runtime, MF_STRING, kCmdRefreshStatus, L"Refresh status");
    AppendMenuW(runtime, MF_STRING, kCmdRestartBackend, L"Restart backend");
    AppendMenuW(runtime, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(runtime, MF_STRING, kCmdStartGemma, L"Start Local Gemma");
    AppendMenuW(runtime, MF_STRING, kCmdStopGemma, L"Stop Local Gemma");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(runtime), L"Runtime");

    HMENU providers = CreatePopupMenu();
    const std::wstring active = NormalizeProvider(provider_);
    for (size_t i = 0; i < kProviders.size(); ++i) {
        UINT flags = MF_STRING;
        if (active == kProviders[i].id) {
            flags |= MF_CHECKED;
        }
        AppendMenuW(providers, flags, kCmdProviderBase + static_cast<WORD>(i), kProviders[i].label);
    }
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(providers), L"Provider");

    SetMenu(hwnd_, menu);
    if (old_menu) {
        DestroyMenu(old_menu);
    }
}

void SymbionShell::InitializeWebView() {
#if SYMBION_NATIVE_HAS_WEBVIEW2_HEADERS
    SetEnvironmentVariableW(L"WEBVIEW2_DEFAULT_BACKGROUND_COLOR", L"FF050508");
    webview_->loader = LoadLibraryW(L"WebView2Loader.dll");
    if (!webview_->loader) {
        SetStatus(L"WebView2 SDK loader was not found. Add WebView2Loader.dll beside this executable.");
        return;
    }

    const auto create_environment = reinterpret_cast<WebViewState::CreateEnvironmentFn>(
        GetProcAddress(webview_->loader, "CreateCoreWebView2EnvironmentWithOptions"));
    if (!create_environment) {
        SetStatus(L"WebView2Loader.dll does not export CreateCoreWebView2EnvironmentWithOptions.");
        return;
    }

    SetStatus(L"Opening " + url_);
    const HRESULT result = create_environment(
        nullptr,
        nullptr,
        nullptr,
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](HRESULT environment_result, ICoreWebView2Environment* environment) -> HRESULT {
                if (FAILED(environment_result) || !environment) {
                    SetStatus(L"WebView2 environment creation failed: " + FormatHresult(environment_result));
                    return S_OK;
                }

                return environment->CreateCoreWebView2Controller(
                    hwnd_,
                    Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [this](HRESULT controller_result, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(controller_result) || !controller) {
                                SetStatus(L"WebView2 controller creation failed: " + FormatHresult(controller_result));
                                return S_OK;
                            }

                            webview_->controller = controller;
                            Microsoft::WRL::ComPtr<ICoreWebView2Controller2> controller2;
                            if (SUCCEEDED(webview_->controller.As(&controller2)) && controller2) {
                                COREWEBVIEW2_COLOR dark = {};
                                dark.A = 255;
                                dark.R = 5;
                                dark.G = 5;
                                dark.B = 8;
                                controller2->put_DefaultBackgroundColor(dark);
                            }
                            webview_->controller->get_CoreWebView2(&webview_->webview);
                            ResizeWebView();
                            if (webview_->webview) {
                                if (!api_key_.empty()) {
                                    webview_->webview->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
                                    webview_->webview->add_WebResourceRequested(
                                        Microsoft::WRL::Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                                            [this](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                                                Microsoft::WRL::ComPtr<ICoreWebView2WebResourceRequest> request;
                                                if (!args || FAILED(args->get_Request(&request)) || !request) {
                                                    return S_OK;
                                                }
                                                LPWSTR uri = nullptr;
                                                if (FAILED(request->get_Uri(&uri)) || !uri) {
                                                    return S_OK;
                                                }
                                                const std::wstring request_uri(uri);
                                                CoTaskMemFree(uri);
                                                if (!StartsWith(request_uri, MakeServiceBase(url_))) {
                                                    return S_OK;
                                                }
                                                Microsoft::WRL::ComPtr<ICoreWebView2HttpRequestHeaders> headers;
                                                if (SUCCEEDED(request->get_Headers(&headers)) && headers) {
                                                    headers->SetHeader(L"X-API-Key", api_key_.c_str());
                                                }
                                                return S_OK;
                                            })
                                            .Get(),
                                        &webview_->web_resource_token);
                                }
                                webview_ready_ = true;
                                webview_->webview->Navigate(url_.c_str());
                            }
                            return S_OK;
                        })
                        .Get());
            })
            .Get());

    if (FAILED(result)) {
        SetStatus(L"WebView2 startup failed: " + FormatHresult(result));
    }
#else
    SetStatus(L"WebView2 headers were not found at build time. This native shell still owns tray, backend, Gemma, and provider lifecycle hooks.");
#endif
}

void SymbionShell::NavigateWebView(const std::wstring& url) {
#if SYMBION_NATIVE_HAS_WEBVIEW2_HEADERS
    if (webview_ && webview_->webview) {
        webview_->webview->Navigate(url.c_str());
    }
#else
    (void)url;
#endif
}

void SymbionShell::ResizeWebView() {
#if SYMBION_NATIVE_HAS_WEBVIEW2_HEADERS
    if (!webview_ || !webview_->controller) {
        return;
    }

    RECT bounds = {};
    GetClientRect(hwnd_, &bounds);
    webview_->controller->put_Bounds(bounds);
#endif
}

void SymbionShell::PaintPlaceholder() {
#if SYMBION_NATIVE_HAS_WEBVIEW2_HEADERS
    if (webview_ && webview_->controller) {
        PAINTSTRUCT paint = {};
        BeginPaint(hwnd_, &paint);
        EndPaint(hwnd_, &paint);
        return;
    }
#endif

    PAINTSTRUCT paint = {};
    HDC dc = BeginPaint(hwnd_, &paint);

    RECT rect = {};
    GetClientRect(hwnd_, &rect);
    FillRect(dc, &rect, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));

    rect.left += 32;
    rect.top += 32;
    rect.right -= 32;
    rect.bottom -= 32;

    const std::wstring text =
        L"Symbion Native Shell\n\n"
        L"Windows WebView2 host for the existing FastAPI web UI.\n\n" +
        status_ +
        L"\n\nBackend: " + backend_status_ +
        L"\nProvider: " + provider_ +
        L"\nLocal Gemma: " + gemma_status_ +
        L"\nTarget URL: " + url_ +
        L"\nRepo: " + (repo_root_.empty() ? L"(not found)" : repo_root_) +
        L"\n\nTray, single-instance, backend lifecycle, provider switching, Gemma hooks, and local auth header scaffolding are active in this native host.";

    DrawTextW(dc, text.c_str(), -1, &rect, DT_LEFT | DT_TOP | DT_WORDBREAK);
    EndPaint(hwnd_, &paint);
}

void SymbionShell::SetStatus(std::wstring status) {
    status_ = std::move(status);
    if (hwnd_) {
        InvalidateRect(hwnd_, nullptr, TRUE);
    }
}

void SymbionShell::SetTrayTooltip(const std::wstring& tooltip) {
    if (tray_data_.cbSize == 0) {
        return;
    }
    tray_data_.uFlags = NIF_TIP;
    wcsncpy_s(tray_data_.szTip, tooltip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &tray_data_);
}

void SymbionShell::ShowMainWindow() {
    if (!hwnd_) {
        return;
    }
    ShowWindow(hwnd_, SW_SHOW);
    if (IsIconic(hwnd_)) {
        ShowWindow(hwnd_, SW_RESTORE);
    }
    SetForegroundWindow(hwnd_);
}

void SymbionShell::HideMainWindow() {
    if (hwnd_) {
        ShowWindow(hwnd_, SW_HIDE);
    }
}

void SymbionShell::ToggleMainWindow() {
    if (!hwnd_ || !IsWindowVisible(hwnd_)) {
        ShowMainWindow();
    } else {
        HideMainWindow();
    }
}

void SymbionShell::StartTray() {
    if (!hwnd_ || tray_data_.cbSize != 0) {
        return;
    }
    tray_data_ = {};
    tray_data_.cbSize = sizeof(tray_data_);
    tray_data_.hWnd = hwnd_;
    tray_data_.uID = 1;
    tray_data_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    tray_data_.uCallbackMessage = kTrayMessage;
    tray_data_.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_SYMBION));
    wcsncpy_s(tray_data_.szTip, L"Symbion - connecting...", _TRUNCATE);
    Shell_NotifyIconW(NIM_ADD, &tray_data_);
}

void SymbionShell::RemoveTray() {
    if (tray_data_.cbSize == 0) {
        return;
    }
    Shell_NotifyIconW(NIM_DELETE, &tray_data_);
    tray_data_ = {};
}

void SymbionShell::ShowTrayMenu() {
    RefreshRuntimeStatus();

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, (L"Backend: " + backend_status_).c_str());
    AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, (L"Provider: " + provider_).c_str());
    AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, (L"Gemma: " + gemma_status_).c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdOpen, L"Open Symbion");
    AppendMenuW(menu, MF_STRING, kCmdHide, IsWindowVisible(hwnd_) ? L"Hide Symbion" : L"Show Symbion");
    AppendMenuW(menu, MF_STRING, kCmdAnalytics, L"Open analytics");

    HMENU providers = CreatePopupMenu();
    const std::wstring active = NormalizeProvider(provider_);
    for (size_t i = 0; i < kProviders.size(); ++i) {
        UINT flags = MF_STRING;
        if (active == kProviders[i].id) {
            flags |= MF_CHECKED;
        }
        AppendMenuW(providers, flags, kCmdProviderBase + static_cast<WORD>(i), kProviders[i].label);
    }
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(providers), L"LLM provider");

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdRefreshStatus, L"Refresh status");
    AppendMenuW(menu, MF_STRING, kCmdRestartBackend, L"Restart backend");
    AppendMenuW(menu, MF_STRING, kCmdStartGemma, L"Start Local Gemma");
    AppendMenuW(menu, MF_STRING, kCmdStopGemma, L"Stop Local Gemma");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdQuit, L"Quit Symbion");

    POINT point = {};
    GetCursorPos(&point);
    SetForegroundWindow(hwnd_);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, point.x, point.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
}

void SymbionShell::RefreshRuntimeStatus() {
    const auto health = HttpGet(health_url_);
    if (health && health->status == 200) {
        backend_status_ = L"online";
        provider_ = ExtractJsonFieldForDisplay(health->body, "provider", provider_);
        build_version_ = ExtractJsonFieldForDisplay(health->body, "version", build_version_);
        const std::wstring mood = ExtractJsonFieldForDisplay(health->body, "mood", L"?");
        const std::wstring failures = ExtractJsonFieldForDisplay(health->body, "consecutive_failures", L"0");
        SetTrayTooltip(L"Symbion v" + build_version_ + L" | " + mood + L" | failures " + failures);
    } else {
        backend_status_ = owns_backend_ && IsProcessStillRunning(backend_process_) ? L"starting" : L"unreachable";
        SetTrayTooltip(L"Symbion - backend " + backend_status_);
    }

    const auto gemma = HttpGet(local_gemma_status_url_, api_key_, 1500);
    if (gemma && gemma->status == 200) {
        gemma_status_ = ExtractJsonFieldForDisplay(gemma->body, "state", L"reported");
    } else {
        const auto models = HttpGet(local_gemma_models_url_, L"", 1000);
        gemma_status_ = (models && models->status == 200) ? L"warm" : L"offline";
    }

    SetWindowTextW(hwnd_, (L"Symbion - " + backend_status_ + L" - " + provider_).c_str());
    SetStatus(L"Runtime status refreshed.");
    BuildMenu();
}

bool SymbionShell::ProbeBackend() {
    const auto response = HttpGet(health_url_, L"", 1500);
    if (response && response->status == 200) {
        backend_status_ = owns_backend_ ? L"online (owned)" : L"online (attached)";
        return true;
    }
    return false;
}

bool SymbionShell::StartBackend() {
    if (repo_root_.empty()) {
        backend_status_ = L"repo not found";
        return false;
    }
    if (IsProcessStillRunning(backend_process_)) {
        return true;
    }

    CloseProcessInfo(backend_process_);
    const std::wstring command = QuoteArg(backend_path_) + L" --repo " + QuoteArg(repo_root_);
    if (!LaunchProcess(command, repo_root_, CREATE_NO_WINDOW, &backend_process_)) {
        backend_status_ = L"failed to start";
        SetStatus(L"Could not launch native backend: " + command);
        return false;
    }
    owns_backend_ = true;
    backend_status_ = L"starting";
    SetStatus(L"Started backend: " + command);
    return true;
}

void SymbionShell::StopBackend() {
    if (!owns_backend_) {
        CloseProcessInfo(backend_process_);
        return;
    }
    if (backend_process_.dwProcessId != 0 && IsProcessStillRunning(backend_process_)) {
        KillProcessTree(backend_process_.dwProcessId);
        WaitForSingleObject(backend_process_.hProcess, 8000);
    }
    CloseProcessInfo(backend_process_);
    owns_backend_ = false;
}

bool SymbionShell::RestartBackend() {
    StopBackend();
    backend_status_ = L"restarting";
    if (!StartBackend()) {
        RefreshRuntimeStatus();
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
        if (ProbeBackend()) {
            NavigateWebView(url_);
            RefreshRuntimeStatus();
            return true;
        }
        Sleep(300);
    }
    RefreshRuntimeStatus();
    return false;
}

bool SymbionShell::StartGemma() {
    if (gemma_start_script_.empty()) {
        MessageBoxW(hwnd_, L"No local_gemma_start_script is configured in config\\symbion.json.", L"Symbion", MB_ICONWARNING);
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::exists(gemma_start_script_, ec)) {
        MessageBoxW(hwnd_, (L"Gemma start script not found:\n" + gemma_start_script_).c_str(), L"Symbion", MB_ICONWARNING);
        return false;
    }
    CloseProcessInfo(gemma_process_);
    const std::wstring command =
        L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File " + QuoteArg(gemma_start_script_);
    const std::wstring cwd = std::filesystem::path(gemma_start_script_).parent_path().wstring();
    if (!LaunchProcess(command, cwd, CREATE_NO_WINDOW, &gemma_process_)) {
        MessageBoxW(hwnd_, L"Could not start the configured Gemma script.", L"Symbion", MB_ICONERROR);
        return false;
    }
    gemma_status_ = L"starting";
    SetStatus(L"Started Local Gemma via " + gemma_start_script_);
    RefreshRuntimeStatus();
    return true;
}

bool SymbionShell::StopGemma() {
    if (!gemma_stop_command_.empty()) {
        const std::wstring command = L"cmd.exe /S /C " + QuoteArg(gemma_stop_command_);
        if (LaunchProcess(command, repo_root_, CREATE_NO_WINDOW, nullptr)) {
            gemma_status_ = L"stop command sent";
            RefreshRuntimeStatus();
            return true;
        }
    }
    if (gemma_process_.dwProcessId != 0 && IsProcessStillRunning(gemma_process_)) {
        KillProcessTree(gemma_process_.dwProcessId);
        CloseProcessInfo(gemma_process_);
        gemma_status_ = L"stopped tracked process";
        RefreshRuntimeStatus();
        return true;
    }
    MessageBoxW(
        hwnd_,
        L"No Gemma stop command is configured, and the native shell does not have a live started process to stop.\n\n"
        L"Set local_gemma_stop_command in config\\symbion.json or SYMBION_GEMMA_STOP_COMMAND to make this a full stop hook.",
        L"Symbion",
        MB_ICONINFORMATION);
    return false;
}

void SymbionShell::SwitchProvider(const std::wstring& provider) {
    if (repo_root_.empty() || config_path_.empty()) {
        MessageBoxW(hwnd_, L"Cannot switch provider because the Symbion repo was not found.", L"Symbion", MB_ICONWARNING);
        return;
    }
    const std::wstring old_provider = provider_;
    if (NormalizeProvider(provider) == NormalizeProvider(old_provider)) {
        return;
    }

    const std::wstring prompt = L"Switch provider from \"" + old_provider + L"\" to \"" + provider +
                                L"\" and restart the owned backend?\n\n"
                                L"If the backend is attached instead of owned, the config is updated but the external backend must be restarted separately.";
    if (MessageBoxW(hwnd_, prompt.c_str(), L"Switch Symbion provider", MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2) != IDYES) {
        BuildMenu();
        return;
    }

    std::string json = ReadTextFile(config_path_);
    if (!ReplaceOrInsertJsonString(json, "llm_provider", WideToUtf8(provider)) || !WriteTextFile(config_path_, json)) {
        MessageBoxW(hwnd_, L"Failed to update llm_provider in config\\symbion.json.", L"Symbion", MB_ICONERROR);
        return;
    }

    config_json_ = json;
    provider_ = NormalizeProvider(provider);
    BuildMenu();
    if (owns_backend_) {
        RestartBackend();
    } else {
        RefreshRuntimeStatus();
        MessageBoxW(hwnd_, L"Provider saved. Restart the externally launched backend for it to take effect.", L"Symbion", MB_ICONINFORMATION);
    }
}

void SymbionShell::OpenAnalytics() {
    const std::wstring analytics = MakeServiceBase(url_) + L"/analytics?suggest=1";
    if (api_key_.empty()) {
        MessageBoxW(
            hwnd_,
            L"No api_key in config\\symbion.json or SYMBION_API_KEY in .env. The analytics route may reject the request until a key is configured.",
            L"Symbion analytics",
            MB_ICONINFORMATION);
    }
    ShowMainWindow();
    NavigateWebView(analytics);
}

void SymbionShell::ReloadWebView() {
#if SYMBION_NATIVE_HAS_WEBVIEW2_HEADERS
    if (webview_ && webview_->webview) {
        webview_->webview->Reload();
    }
#endif
}

void SymbionShell::OpenDevTools() {
#if SYMBION_NATIVE_HAS_WEBVIEW2_HEADERS
    if (webview_ && webview_->webview) {
        webview_->webview->OpenDevToolsWindow();
    }
#endif
}

void SymbionShell::HandleCommand(WORD command_id) {
    if (command_id >= kCmdProviderBase && command_id < kCmdProviderBase + kProviders.size()) {
        SwitchProvider(kProviders[command_id - kCmdProviderBase].id);
        return;
    }

    switch (command_id) {
        case kCmdOpen:
            ShowMainWindow();
            NavigateWebView(url_);
            break;
        case kCmdHide:
            ToggleMainWindow();
            break;
        case kCmdQuit:
            quitting_ = true;
            DestroyWindow(hwnd_);
            break;
        case kCmdRefreshStatus:
            RefreshRuntimeStatus();
            break;
        case kCmdRestartBackend:
            RestartBackend();
            break;
        case kCmdStartGemma:
            StartGemma();
            break;
        case kCmdStopGemma:
            StopGemma();
            break;
        case kCmdAnalytics:
            OpenAnalytics();
            break;
        case kCmdReload:
            ReloadWebView();
            break;
        case kCmdDevTools:
            OpenDevTools();
            break;
        default:
            break;
    }
}

std::wstring ResolveInitialUrl(PWSTR command_line) {
    (void)command_line;

    std::wstring env_url = ReadEnvironmentUrl();
    if (!env_url.empty()) {
        return env_url;
    }

    int argc = 0;
    PWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring url = SYMBION_NATIVE_DEFAULT_URL;

    if (argv) {
        for (int i = 1; i < argc; ++i) {
            const std::wstring_view arg(argv[i]);
            if (StartsWith(arg, L"--url=")) {
                url = std::wstring(arg.substr(6));
                break;
            }
            if (arg == L"--url" && i + 1 < argc) {
                url = argv[i + 1];
                break;
            }
        }
        LocalFree(argv);
    }

    return url;
}
