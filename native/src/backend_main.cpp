#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>

namespace {

std::atomic_bool g_running = true;

struct Config {
    int port = 8000;
    std::string provider = "local_gemma";
    std::string gemma_base_url = "http://127.0.0.1:8088/v1";
    std::string db_path = "symbion.db";
};

void HandleSignal(int) {
    g_running = false;
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

std::optional<std::string> ExtractJsonString(const std::string& json, const std::string& key) {
    const std::regex pattern("\"" + key + "\"\\s*:\\s*\"((?:\\\\.|[^\"])*)\"");
    std::smatch match;
    if (!std::regex_search(json, match, pattern)) {
        return std::nullopt;
    }
    return match[1].str();
}

std::optional<int> ExtractJsonInt(const std::string& json, const std::string& key) {
    const std::regex pattern("\"" + key + "\"\\s*:\\s*(-?\\d+)");
    std::smatch match;
    if (!std::regex_search(json, match, pattern)) {
        return std::nullopt;
    }
    return std::stoi(match[1].str());
}

Config LoadConfig(const std::filesystem::path& repo_root) {
    Config config;
    const std::string json = ReadTextFile(repo_root / "symbion.json");
    if (auto value = ExtractJsonInt(json, "web_port")) {
        config.port = *value;
    }
    if (auto value = ExtractJsonString(json, "llm_provider")) {
        config.provider = *value;
    }
    if (auto value = ExtractJsonString(json, "local_gemma_base_url")) {
        config.gemma_base_url = *value;
    }
    if (auto value = ExtractJsonString(json, "db_path")) {
        config.db_path = *value;
    }
    return config;
}

std::string HttpDate() {
    return "Wed, 27 May 2026 00:00:00 GMT";
}

std::string ReasonPhrase(int status) {
    switch (status) {
        case 200: return "OK";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        default: return "Internal Server Error";
    }
}

std::string MakeResponse(int status, std::string_view content_type, std::string_view body) {
    std::ostringstream out;
    out << "HTTP/1.1 " << status << ' ' << ReasonPhrase(status) << "\r\n"
        << "Content-Type: " << content_type << "; charset=utf-8\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Cache-Control: no-store\r\n"
        << "Date: " << HttpDate() << "\r\n"
        << "Connection: close\r\n\r\n"
        << body;
    return out.str();
}

std::string EscapeJson(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char c : value) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

std::string Route(const Config& config, const std::filesystem::path& repo_root, std::string_view method, std::string_view path) {
    if (method != "GET") {
        return MakeResponse(405, "application/json", "{\"error\":\"method_not_allowed\"}");
    }

    if (path == "/health") {
        const std::string body =
            "{\"status\":\"ok\",\"runtime\":\"native-cpp\",\"version\":\"0.2.0\","
            "\"provider\":\"" + EscapeJson(config.provider) + "\","
            "\"db_path\":\"" + EscapeJson(config.db_path) + "\"}";
        return MakeResponse(200, "application/json", body);
    }

    if (path == "/api/local-gemma/status") {
        const std::string body =
            "{\"configured\":true,\"running\":null,\"base_url\":\"" +
            EscapeJson(config.gemma_base_url) +
            "\",\"note\":\"Native status probe is scaffolded; model calls migrate next.\"}";
        return MakeResponse(200, "application/json", body);
    }

    if (path == "/" || path == "/index.html") {
        std::string html = ReadTextFile(repo_root / "native" / "web" / "index.html");
        if (html.empty()) {
            html = "<!doctype html><title>Symbion</title><h1>Symbion native runtime</h1>";
        }
        return MakeResponse(200, "text/html", html);
    }

    return MakeResponse(404, "application/json", "{\"error\":\"not_found\"}");
}

bool SendAll(SOCKET socket, std::string_view data) {
    size_t sent_total = 0;
    while (sent_total < data.size()) {
        const int sent = send(socket, data.data() + sent_total, static_cast<int>(data.size() - sent_total), 0);
        if (sent <= 0) {
            return false;
        }
        sent_total += static_cast<size_t>(sent);
    }
    return true;
}

void HandleClient(SOCKET client, const Config& config, const std::filesystem::path& repo_root) {
    std::string request(8192, '\0');
    const int received = recv(client, request.data(), static_cast<int>(request.size() - 1), 0);
    if (received <= 0) {
        closesocket(client);
        return;
    }
    request.resize(static_cast<size_t>(received));

    std::istringstream first_line(request.substr(0, request.find("\r\n")));
    std::string method;
    std::string target;
    first_line >> method >> target;
    const size_t query = target.find('?');
    if (query != std::string::npos) {
        target.resize(query);
    }

    const std::string response = Route(config, repo_root, method, target);
    SendAll(client, response);
    shutdown(client, SD_BOTH);
    closesocket(client);
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    std::filesystem::path repo_root = std::filesystem::current_path();
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string_view(argv[i]) == "--repo") {
            repo_root = argv[i + 1];
        }
    }

    const Config config = LoadConfig(repo_root);

    WSADATA wsa = {};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }

    SOCKET server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server == INVALID_SOCKET) {
        std::cerr << "socket failed\n";
        WSACleanup();
        return 1;
    }

    BOOL reuse = TRUE;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<u_short>(config.port));

    if (bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
        listen(server, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "bind/listen failed on 127.0.0.1:" << config.port << "\n";
        closesocket(server);
        WSACleanup();
        return 1;
    }

    std::cout << "Symbion native backend listening on http://127.0.0.1:" << config.port << "\n";
    while (g_running) {
        fd_set readfds = {};
        FD_ZERO(&readfds);
        FD_SET(server, &readfds);
        timeval timeout = {1, 0};
        const int ready = select(0, &readfds, nullptr, nullptr, &timeout);
        if (ready <= 0) {
            continue;
        }
        SOCKET client = accept(server, nullptr, nullptr);
        if (client != INVALID_SOCKET) {
            HandleClient(client, config, repo_root);
        }
    }

    closesocket(server);
    WSACleanup();
    return 0;
}
