#include "intent_router.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace symbion {

namespace {

std::string Lower(std::string_view value) {
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

bool ContainsAny(const std::string& text, const std::vector<std::string>& needles) {
    return std::any_of(needles.begin(), needles.end(), [&](const std::string& needle) {
        return text.find(needle) != std::string::npos;
    });
}

bool IsGreetingOnly(const std::string& text) {
    return text == "hi" || text == "hello" || text == "hey" || text == "helo" || text == "yo" ||
           text == "good morning" || text == "good afternoon" || text == "good evening";
}

bool LooksLikeDirectQuestion(const std::string& text) {
    return ContainsAny(text, {
        "what is", "what are", "who is", "who was", "where is", "where was",
        "when did", "when was", "why did", "how do", "how does", "how did",
        "tell me about", "explain", "summarize", "define", "which", "what verse",
        "what chapter", "what book", "list", "name the", "give me"
    });
}

bool LooksLikeTask(const std::string& text) {
    return ContainsAny(text, {
        "make", "create", "build", "fix", "change", "update", "delete", "move",
        "rename", "run", "install", "open", "write code", "implement"
    });
}

bool LooksLikeCreative(const std::string& text) {
    return ContainsAny(text, {
        "write a poem", "write a story", "draft", "brainstorm", "ideas for",
        "make a song", "roleplay"
    });
}

}  // namespace

Intent ClassifyIntent(std::string_view message) {
    const std::string text = Lower(message);
    Intent intent;

    intent.asks_for_list = ContainsAny(text, {"list", "what are the", "name the", "give me the"});
    intent.emotional = ContainsAny(text, {
        "i feel", "i'm feeling", "im feeling", "i am feeling", "i'm sad", "im sad",
        "i'm anxious", "im anxious", "i am anxious", "i'm scared", "im scared",
        "overwhelmed", "stressed", "lonely", "angry", "hurt", "grief", "ashamed",
        "depressed", "hopeless", "panic", "afraid", "confused about my life"
    });
    const bool trauma_related = ContainsAny(text, {
        "trauma", "ptsd", "flashback", "nightmare", "nightmares", "abuse", "assaulted",
        "molested", "raped", "touched me", "touch me", "back there", "body is always on guard",
        "always on guard", "freeze instead", "fight back", "blaming myself", "dirty",
        "scared to sleep", "ashamed for surviving", "after what happened", "smelled",
        "smell", "felt scared", "suddenly felt scared"
    });
    intent.crisis = ContainsAny(text, {
        "suicide", "kill myself", "end my life", "can't go on", "cannot go on",
        "want to die", "wants to die",
        "want to disappear", "do not want to wake up", "don't want to wake up",
        "plan to end my life", "pills", "overdose", "holding a gun", "holding pills"
    });
    intent.intense = intent.crisis || ContainsAny(text, {
        "unbearable", "panic", "terrified", "emergency"
    });

    if (IsGreetingOnly(text)) {
        intent.mode = IntentMode::Social;
    } else if (intent.intense) {
        intent.mode = IntentMode::Counseling;
    } else if (intent.emotional || trauma_related) {
        intent.emotional = true;
        intent.mode = IntentMode::Reflective;
    } else if (LooksLikeCreative(text)) {
        intent.mode = IntentMode::Creative;
    } else if (LooksLikeTask(text)) {
        intent.mode = IntentMode::Task;
    } else if (LooksLikeDirectQuestion(text) || text.find('?') != std::string::npos) {
        intent.mode = IntentMode::DirectAnswer;
    } else {
        intent.mode = IntentMode::DirectAnswer;
    }

    return intent;
}

std::string IntentModeName(IntentMode mode) {
    switch (mode) {
        case IntentMode::Social: return "social";
        case IntentMode::DirectAnswer: return "direct_answer";
        case IntentMode::Reflective: return "reflective";
        case IntentMode::Counseling: return "counseling";
        case IntentMode::Creative: return "creative";
        case IntentMode::Task: return "task";
        case IntentMode::Clarify: return "clarify";
    }
    return "direct_answer";
}

}  // namespace symbion
