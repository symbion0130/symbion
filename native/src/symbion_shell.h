#pragma once

#include <windows.h>

#include <memory>
#include <optional>
#include <string>

class SymbionShell {
public:
    explicit SymbionShell(std::wstring initial_url);
    ~SymbionShell();

    SymbionShell(const SymbionShell&) = delete;
    SymbionShell& operator=(const SymbionShell&) = delete;

    int Run(HINSTANCE instance, int show_command);

private:
    static constexpr wchar_t kWindowClassName[] = L"SymbionNativeShellWindow";

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);

    bool AcquireSingleInstance();
    void FocusExistingInstance();
    void ResolveRuntimeConfiguration();
    bool RegisterWindowClass(HINSTANCE instance);
    bool CreateMainWindow(HINSTANCE instance, int show_command);
    void BuildMenu();
    void InitializeWebView();
    void NavigateWebView(const std::wstring& url);
    void ResizeWebView();
    void PaintPlaceholder();
    void SetStatus(std::wstring status);
    void SetTrayTooltip(const std::wstring& tooltip);
    void ShowMainWindow();
    void HideMainWindow();
    void ToggleMainWindow();
    void StartTray();
    void RemoveTray();
    void ShowTrayMenu();
    void RefreshRuntimeStatus();
    bool ProbeBackend();
    bool StartBackend();
    void StopBackend();
    bool RestartBackend();
    bool StartGemma();
    bool StopGemma();
    void SwitchProvider(const std::wstring& provider);
    void OpenAnalytics();
    void ReloadWebView();
    void OpenDevTools();
    void HandleCommand(WORD command_id);
    std::optional<std::wstring> ReadConfigString(const std::string& key) const;
    std::optional<int> ReadConfigInt(const std::string& key) const;
    std::optional<bool> ReadConfigBool(const std::string& key) const;

    HWND hwnd_ = nullptr;
    HINSTANCE instance_ = nullptr;
    std::wstring url_;
    std::wstring status_;
    std::wstring repo_root_;
    std::wstring config_path_;
    std::wstring python_path_;
    std::wstring api_key_;
    std::wstring provider_;
    std::wstring health_url_;
    std::wstring local_gemma_status_url_;
    std::wstring local_gemma_models_url_;
    std::wstring gemma_start_script_;
    std::wstring gemma_stop_command_;
    std::wstring backend_status_ = L"backend unknown";
    std::wstring gemma_status_ = L"Gemma unknown";
    std::wstring build_version_ = L"?";
    std::string config_json_;
    int web_port_ = 8000;
    bool tray_enabled_ = true;
    bool owns_backend_ = false;
    bool quitting_ = false;
    bool webview_ready_ = false;
    HANDLE single_instance_mutex_ = nullptr;
    UINT single_instance_message_ = 0;
    NOTIFYICONDATAW tray_data_ = {};
    PROCESS_INFORMATION backend_process_ = {};
    PROCESS_INFORMATION gemma_process_ = {};

    struct WebViewState;
    std::unique_ptr<WebViewState> webview_;
};

std::wstring ResolveInitialUrl(PWSTR command_line);
