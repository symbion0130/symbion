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

bool LooksLikeSocialChat(const std::string& text) {
    return IsGreetingOnly(text) || ContainsAny(text, {
        "what's up", "whats up", "sup", "my guy", "how are you",
        "how you doing", "how you feeling", "how's it going", "hows it going"
    });
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
        "make ", "create ", "build ", "fix ", "change ", "update ", "delete ", "move ",
        "rename ", "run ", "install ", "open ", "write code", "implement ",
        "show how to", "teach me how", "walk me through"
    });
}

bool LooksLikePracticalLife(const std::string& text) {
    return ContainsAny(text, {
        "hungry", "lunch", "dinner", "breakfast", "restaurant", "food",
        "protein", "local spot", "nearby", "paper bat", "folded bat", "paper folded",
        "like an airplane", "origami"
    });
}

bool LooksLikeForget(const std::string& text) {
    return ContainsAny(text, {
        "forget", "delete memory", "delete that memory", "delete this memory",
        "remove memory", "erase memory", "clear memory", "clear this chat",
        "delete this chat", "forget this", "forget that", "don't bring this up",
        "dont bring this up", "do not bring this up", "stop remembering",
        "remove that from memory", "erase that from memory"
    });
}

bool LooksLikeWipeAll(const std::string& text) {
    return ContainsAny(text, {
        "wipe memory", "wipe my memory", "wipe all memory", "delete all memory",
        "delete my memory", "erase memory", "erase my memory", "erase all memory",
        "clear all memory", "clear my memory", "reset memory", "factory reset memory",
        "forget everything", "delete everything you remember", "clear every memory",
        "wipe memories", "reset all memories"
    });
}

bool LooksLikeCreative(const std::string& text) {
    return ContainsAny(text, {
        "write a poem", "write a story", "write a short", "write me", "draft", "brainstorm", "ideas for",
        "make a song", "roleplay"
    });
}

bool LooksLikeEmotionLabel(const std::string& text) {
    return text == "anger" || text == "angry" || text == "rage" || text == "sadness" ||
           text == "sad" || text == "grief" || text == "fear" || text == "scared" ||
           text == "anxiety" || text == "anxious" || text == "shame" || text == "ashamed" ||
           text == "guilt" || text == "lonely" || text == "overwhelmed" || text == "stress" ||
           text == "stressed" || text == "confused" || text == "numb" || text == "hopeless";
}

bool LooksLikePositiveCheckin(const std::string& text) {
    return text == "positive" || ContainsAny(text, {"positive ", "positive/"});
}

}  // namespace

Intent ClassifyIntent(std::string_view message) {
    const std::string text = Lower(message);
    Intent intent;

    intent.wipe_all = LooksLikeWipeAll(text);
    intent.forget = intent.wipe_all || LooksLikeForget(text);
    intent.asks_for_list = ContainsAny(text, {"list", "what are the", "name the", "give me the"});
    intent.emotional = !LooksLikePracticalLife(text) && ContainsAny(text, {
        "i feel", "i'm feeling", "im feeling", "i am feeling", "i'm sad", "im sad",
        "i'm anxious", "im anxious", "i am anxious", "i'm scared", "im scared",
        "overwhelmed", "stress", "stressed", "lonely", "anger", "angry", "hurt", "grief", "ashamed",
        "depressed", "hopeless", "panic", "afraid", "confused about my life",
        "talking down to me", "talks down to me", "disrespect", "disrespected",
        "worthless", "respect me", "respect from", "my mom", "my mother",
        "my dad", "my father", "my family", "my boss", "evil boss",
        "my friend", "my brother", "my sister", "my wife", "my husband",
        "my spouse", "my partner", "my coworker", "makes me feel", "made me feel",
        "always criticizes", "keeps mocking", "betrayed me", "too sensitive",
        "nothing happened", "i end up apologizing", "feel stupid", "dread opening",
        "replaying every conversation", "humiliated", "laugh it off",
        "today has been better", "change is needed", "burn the ships"
    }) || LooksLikeEmotionLabel(text);
    const bool trauma_related = ContainsAny(text, {
        "trauma", "ptsd", "flashback", "nightmare", "nightmares", "abuse", "assaulted",
        "molested", "raped", "touched me", "touch me", "back there", "body is always on guard",
        "always on guard", "freeze instead", "fight back", "blaming myself", "dirty",
        "scared to sleep", "ashamed for surviving", "after what happened", "smelled",
        "smell", "felt scared", "suddenly felt scared", "body freezes", "my body freezes",
        "someone raises their voice", "raises their voice", "replaying what happened",
        "keep replaying", "cannot calm down", "can't calm down"
    });
    intent.crisis = ContainsAny(text, {
        "suicide", "kill myself", "end my life", "can't go on", "cannot go on",
        "want to die", "wants to die", "do not want to be alive", "don't want to be alive",
        "dont want to be alive", "not want to be alive", "wish i was dead",
        "hurt myself", "harm myself", "cut myself",
        "want to disappear", "do not want to wake up", "don't want to wake up",
        "plan to end my life", "pills", "overdose", "holding a gun", "holding pills"
    });
    intent.intense = intent.crisis || ContainsAny(text, {
        "unbearable", "panic", "terrified", "emergency"
    });

    if (LooksLikePositiveCheckin(text)) {
        intent.emotional = true;
    }

    if (intent.forget) {
        intent.mode = IntentMode::Forget;
    } else if (LooksLikeSocialChat(text)) {
        intent.mode = IntentMode::Social;
    } else if (intent.intense) {
        intent.mode = IntentMode::Counseling;
    } else if (intent.emotional || trauma_related) {
        intent.emotional = true;
        intent.mode = IntentMode::Reflective;
    } else if (LooksLikeCreative(text)) {
        intent.mode = IntentMode::Creative;
    } else if (LooksLikeTask(text) && LooksLikePracticalLife(text)) {
        intent.mode = IntentMode::Task;
    } else if (LooksLikeDirectQuestion(text) || text.find('?') != std::string::npos) {
        intent.mode = IntentMode::DirectAnswer;
    } else if (LooksLikeTask(text)) {
        intent.mode = IntentMode::Task;
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
        case IntentMode::Forget: return "forget";
        case IntentMode::Clarify: return "clarify";
    }
    return "direct_answer";
}

}  // namespace symbion
