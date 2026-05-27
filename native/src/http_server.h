#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace symbion {

struct HttpRequest {
    std::string method;
    std::string path;
    std::string body;
    std::unordered_map<std::string, std::string> headers;
};

struct HttpResponse {
    int status = 200;
    std::string content_type = "application/json";
    std::string body;
};

using RouteHandler = std::function<HttpResponse(const HttpRequest&)>;

class HttpServer {
public:
    explicit HttpServer(int port);
    int Run(const std::atomic_bool& running, const RouteHandler& handler);

private:
    int port_;
};

HttpResponse JsonResponse(std::string body, int status = 200);
HttpResponse TextResponse(std::string body, std::string content_type = "text/plain", int status = 200);

}  // namespace symbion
