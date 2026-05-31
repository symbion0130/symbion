#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "memory_store.h"

namespace symbion {

enum class IntentMode {
    Social,
    DirectAnswer,
    Reflective,
    Counseling,
    Creative,
    Task,
    Forget
};

struct Intent {
    IntentMode mode = IntentMode::DirectAnswer;
    bool emotional = false;
    bool intense = false;
    bool crisis = false;
    bool forget = false;
    bool wipe_all = false;
    bool asks_for_list = false;
};

Intent ClassifyIntent(std::string_view message);
Intent ClassifyIntent(std::string_view message, const std::vector<ChatMessage>& recent);
std::string IntentModeName(IntentMode mode);

}  // namespace symbion
