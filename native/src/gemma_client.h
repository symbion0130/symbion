#pragma once

#include "config.h"
#include "intent_router.h"
#include "memory_store.h"

#include <string>
#include <vector>

namespace symbion {

class GemmaClient : public SummaryGenerator {
public:
    explicit GemmaClient(Config config);

    std::string Chat(const std::string& user_message,
                     const Intent& intent,
                     const std::vector<ChatMessage>& recent,
                     const std::vector<ChatMessage>& relevant,
                     const std::vector<SourceChunk>& sources,
                     const std::vector<EmotionSignal>& emotions,
                     const std::string& guidance = "") const;
    std::string SummarizeSessionWindow(const std::vector<ChatMessage>& messages) const override;

private:
    Config config_;
};

std::string FallbackReply(const std::string& user_message, const Intent& intent);
std::string CrisisReply(const std::string& user_message, const Intent& intent);

}  // namespace symbion
