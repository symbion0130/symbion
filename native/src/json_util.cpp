#include "json_util.h"

namespace symbion {

namespace {

std::optional<size_t> FindJsonValueStart(const std::string& json, const std::string& key) {
    const std::string quoted_key = "\"" + key + "\"";
    size_t pos = json.find(quoted_key);
    if (pos == std::string::npos) return std::nullopt;
    pos = json.find(':', pos + quoted_key.size());
    if (pos == std::string::npos) return std::nullopt;
    pos = json.find_first_not_of(" \t\r\n", pos + 1);
    if (pos == std::string::npos) return std::nullopt;
    return pos;
}

}  // namespace

std::string EscapeJson(std::string_view value) {
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

std::optional<std::string> ExtractJsonString(const std::string& json, const std::string& key) {
    return ExtractJsonStringSimple(json, key);
}

std::optional<std::string> ExtractJsonStringSimple(const std::string& json, const std::string& key) {
    auto start = FindJsonValueStart(json, key);
    if (!start || json[*start] != '"') return std::nullopt;
    size_t pos = *start + 1;

    std::string raw;
    bool escaped = false;
    for (; pos < json.size(); ++pos) {
        const char c = json[pos];
        if (escaped) {
            raw.push_back('\\');
            raw.push_back(c);
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '"') {
            return JsonUnescape(raw);
        }
        raw.push_back(c);
    }
    return std::nullopt;
}

std::optional<int> ExtractJsonInt(const std::string& json, const std::string& key) {
    auto start = FindJsonValueStart(json, key);
    if (!start) return std::nullopt;
    size_t end = *start;
    if (end < json.size() && json[end] == '-') ++end;
    while (end < json.size() && json[end] >= '0' && json[end] <= '9') ++end;
    if (end == *start) return std::nullopt;
    return std::stoi(json.substr(*start, end - *start));
}

std::optional<double> ExtractJsonDouble(const std::string& json, const std::string& key) {
    auto start = FindJsonValueStart(json, key);
    if (!start) return std::nullopt;
    size_t end = *start;
    if (end < json.size() && json[end] == '-') ++end;
    while (end < json.size() && json[end] >= '0' && json[end] <= '9') ++end;
    if (end < json.size() && json[end] == '.') {
        ++end;
        while (end < json.size() && json[end] >= '0' && json[end] <= '9') ++end;
    }
    if (end == *start) return std::nullopt;
    return std::stod(json.substr(*start, end - *start));
}

}  // namespace symbion
