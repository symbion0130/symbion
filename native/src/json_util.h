#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace symbion {

std::string EscapeJson(std::string_view value);
std::string JsonUnescape(std::string_view value);
std::optional<std::string> ExtractJsonString(const std::string& json, const std::string& key);
std::optional<std::string> ExtractJsonStringSimple(const std::string& json, const std::string& key);
std::optional<int> ExtractJsonInt(const std::string& json, const std::string& key);
std::optional<double> ExtractJsonDouble(const std::string& json, const std::string& key);

}  // namespace symbion
