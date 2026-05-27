#pragma once

#include <string>
#include <string_view>

namespace symbion {

enum class IntentMode {
    Social,
    DirectAnswer,
    Reflective,
    Counseling,
    Creative,
    Task,
    Clarify
};

struct Intent {
    IntentMode mode = IntentMode::DirectAnswer;
    bool emotional = false;
    bool intense = false;
    bool crisis = false;
    bool asks_for_list = false;
};

Intent ClassifyIntent(std::string_view message);
std::string IntentModeName(IntentMode mode);

}  // namespace symbion
