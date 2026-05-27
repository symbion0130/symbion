#pragma once

#include "config.h"
#include "memory_store.h"

#include <string>
#include <vector>

namespace symbion {

class GemmaClient {
public:
    explicit GemmaClient(Config config);

    std::string Chat(const std::string& user_message,
                     const std::vector<ChatMessage>& recent,
                     const std::vector<ChatMessage>& relevant,
                     const std::vector<EmotionSignal>& emotions) const;

private:
    Config config_;
};

std::string FallbackCounselorReply(const std::string& user_message);

}  // namespace symbion
