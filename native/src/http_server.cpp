#include "http_server.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cctype>
#include <exception>
#include <iostream>
#include <sstream>

namespace symbion {

namespace {

std::string ReasonPhrase(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        default: return "Internal Server Error";
    }
}

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool SendAll(SOCKET socket, std::string_view data) {
    size_t sent_total = 0;
    while (sent_total < data.size()) {
        const int sent = send(socket, data.data() + sent_total, static_cast<int>(data.size() - sent_total), 0);
        if (sent <= 0) return false;
        sent_total += static_cast<size_t>(sent);
    }
    return true;
}

size_t HeaderContentLength(std::string_view request) {
    const size_t header_end = request.find("\r\n\r\n");
    if (header_end == std::string_view::npos) return 0;
    std::string headers(request.substr(0, header_end));
    std::istringstream lines(headers);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        if (Lower(line.substr(0, colon)) == "content-length") {
            const std::string value = line.substr(colon + 1);
            return static_cast<size_t>(std::stoul(value));
        }
    }
    return 0;
}

HttpRequest ParseRequest(std::string request) {
    HttpRequest parsed;
    const size_t header_end = request.find("\r\n\r\n");
    const std::string headers = request.substr(0, header_end);
    parsed.body = header_end == std::string::npos ? "" : request.substr(header_end + 4);

    std::istringstream lines(headers);
    std::string line;
    if (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream first(line);
        std::string target;
        first >> parsed.method >> target;
        parsed.path = target;
    }

    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = Lower(line.substr(0, colon));
        std::string value = line.substr(colon + 1);
        const size_t first = value.find_first_not_of(" \t");
        parsed.headers[key] = first == std::string::npos ? "" : value.substr(first);
    }
    return parsed;
}

std::string Serialize(const HttpResponse& response) {
    std::ostringstream out;
    out << "HTTP/1.1 " << response.status << ' ' << ReasonPhrase(response.status) << "\r\n"
        << "Content-Type: " << response.content_type << "; charset=utf-8\r\n"
        << "Content-Length: " << response.body.size() << "\r\n"
        << "Cache-Control: no-store\r\n"
        << "Connection: close\r\n\r\n"
        << response.body;
    return out.str();
}

void HandleClient(SOCKET client, const RouteHandler& handler) {
    std::string request;
    request.resize(65536);
    const int received = recv(client, request.data(), static_cast<int>(request.size()), 0);
    if (received <= 0) {
        closesocket(client);
        return;
    }
    request.resize(static_cast<size_t>(received));

    size_t header_end = request.find("\r\n\r\n");
    if (header_end != std::string::npos) {
        const size_t expected_body = HeaderContentLength(request);
        while (request.size() < header_end + 4 + expected_body) {
            char buffer[8192] = {};
            const int more = recv(client, buffer, static_cast<int>(sizeof(buffer)), 0);
            if (more <= 0) break;
            request.append(buffer, static_cast<size_t>(more));
            header_end = request.find("\r\n\r\n");
        }
    }

    HttpResponse response;
    try {
        response = handler(ParseRequest(std::move(request)));
    } catch (const std::exception& ex) {
        response = JsonResponse(std::string("{\"error\":\"internal_error\",\"detail\":\"") + ex.what() + "\"}", 500);
    } catch (...) {
        response = JsonResponse("{\"error\":\"internal_error\"}", 500);
    }
    SendAll(client, Serialize(response));
    shutdown(client, SD_BOTH);
    closesocket(client);
}

}  // namespace

HttpServer::HttpServer(int port) : port_(port) {}

int HttpServer::Run(const std::atomic_bool& running, const RouteHandler& handler) {
    WSADATA wsa = {};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }

    SOCKET server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server == INVALID_SOCKET) {
        WSACleanup();
        return 1;
    }

    BOOL reuse = TRUE;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<u_short>(port_));

    if (bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
        listen(server, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "bind/listen failed on 127.0.0.1:" << port_ << "\n";
        closesocket(server);
        WSACleanup();
        return 1;
    }

    std::cout << "Symbion native backend listening on http://127.0.0.1:" << port_ << "\n";
    while (running) {
        fd_set readfds = {};
        FD_ZERO(&readfds);
        FD_SET(server, &readfds);
        timeval timeout = {1, 0};
        const int ready = select(0, &readfds, nullptr, nullptr, &timeout);
        if (ready <= 0) continue;
        SOCKET client = accept(server, nullptr, nullptr);
        if (client != INVALID_SOCKET) {
            HandleClient(client, handler);
        }
    }

    closesocket(server);
    WSACleanup();
    return 0;
}

HttpResponse JsonResponse(std::string body, int status) {
    return {status, "application/json", std::move(body)};
}

HttpResponse TextResponse(std::string body, std::string content_type, int status) {
    return {status, std::move(content_type), std::move(body)};
}

}  // namespace symbion
