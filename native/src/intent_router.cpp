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

bool IsExactSocialChat(const std::string& text) {
    return IsGreetingOnly(text) ||
           text == "what's up" || text == "whats up" || text == "sup" ||
           text == "what's up my guy" || text == "whats up my guy" ||
           text == "what's up my uy" || text == "whats up my uy" ||
           text == "my guy" || text == "guy" ||
           text == "how are you" || text == "how you doing" || text == "how you feeling" ||
           text == "how's it going" || text == "hows it going" ||
           text == "what you up to" || text == "what are you up to" || text == "whatcha up to" ||
           text == "chillin" || text == "chilling" || text == "vibing" ||
           text == "thanks" || text == "thank you" || text == "appreciate it" ||
           text == "bet" || text == "fair enough" || text == "deff" ||
           text == "you was bugging" || text == "you were bugging" ||
           text == "you bugging" || text == "youre bugging" || text == "you're bugging";
}

bool LooksLikeSocialChat(const std::string& text) {
    return IsExactSocialChat(text) || ContainsAny(text, {
        "just hanging", "hanging out", "good day",
        "good vibes", "vibing", "taking it easy", "all good", "appreciate",
        "thanks", "thank you", "big dog", "we cookin", "cookin my guy",
        "lolol", "lmao", "fit check", "vibe check", "looking good so far",
        "finally got", "got it installed", "app installed", "not important for now",
        "shipping code", "just sitting here", "watching ", "change the subject",
        "fair enough", "deff", "burger", "scripted response", "too scripted",
        "sounds scripted", "scripted responses", "keep getting scripted", "getting scripted",
        "canned response", "canned responses", "robotic response", "machine response",
        "tired of it", "getting tired of it",
        "working hard", "response style", "reply style", "conversation flow", "chat flow",
        "making some changes", "how you respond", "good night so far", "good nite so far",
        "watching basketball", "basketball", "my team", "team is losing", "team's losing",
        "the game", "the score", "wouldnt go past this game", "wouldn't go past this game"
    });
}

bool LooksLikePositiveSlang(const std::string& text);

bool LooksLikeSocialSignal(const std::string& text) {
    return IsExactSocialChat(text) || LooksLikePositiveSlang(text) ||
           ContainsAny(text, {"change the subject", "not important for now", "fair enough"});
}

bool LooksLikeOpenEmotionalThread(const std::string& text) {
    return ContainsAny(text, {
        "i feel", "i'm feeling", "im feeling", "i am feeling",
        "ashamed", "shame", "stuck", "not enough", "inadequate",
        "destructive habit", "destructive habits", "habits that were destructive",
        "not being good", "hurting people", "people around me",
        "afraid", "anxious", "anxiety", "pressure", "wrong step",
        "rough", "uphill battle", "down to my bones", "kill myself",
        "hurt myself", "want to die", "what makes you feel",
        "what is it connected", "what feels most intense", "which habit",
        "most damage", "truth on the table", "slowly and gently",
        "you aren't the same", "you arent the same", "not the same",
        "you are not the same", "i hate that you aren't", "i hate that you arent"
    });
}

bool RecentHasOpenEmotionalThread(const std::vector<ChatMessage>& recent) {
    int scanned = 0;
    for (auto it = recent.rbegin(); it != recent.rend() && scanned < 6; ++it, ++scanned) {
        if (LooksLikeOpenEmotionalThread(Lower(it->content))) return true;
    }
    return false;
}

bool PreviousUserWasSocialSignal(const std::vector<ChatMessage>& recent) {
    for (auto it = recent.rbegin(); it != recent.rend(); ++it) {
        if (it->role != "user") continue;
        return LooksLikeSocialSignal(Lower(it->content));
    }
    return false;
}

bool LooksLikePositiveSlang(const std::string& text) {
    if (ContainsAny(text, {"i feel sick", "i'm sick", "im sick", "feel sick", "getting sick"})) {
        return false;
    }
    return text == "sick" || text == "fire" || text == "dope" || text == "lit" ||
           text == "bet" || text == "rad" || text == "clean" || text == "based" ||
           ContainsAny(text, {
               "that's sick", "thats sick", "that is sick", "this is sick", "so sick",
               "pretty sick", "sick man", "sick dude", "sick my guy", "sick bro",
               "that's fire", "thats fire", "that is fire", "this is fire",
               "that's dope", "thats dope", "that is dope", "this is dope",
               "that's lit", "thats lit", "that is lit", "this is lit",
               "no cap", "big w", "huge w", "lets go", "let's go",
               "cookin", "cooking", "clean win"
           });
}

bool LooksLikeDirectQuestion(const std::string& text) {
    return ContainsAny(text, {
        "what is", "what are", "who is", "who was", "where is", "where was",
        "when did", "when was", "why did", "how do", "how does", "how did",
        "tell me about", "explain", "summarize", "define", "which", "what verse",
        "what chapter", "what book", "list", "name the", "give me",
        "what you think", "what do you think", "thoughts"
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

bool LooksLikePhysicalIllness(const std::string& text) {
    return ContainsAny(text, {
        "i feel sick", "i'm sick", "im sick", "i am sick", "feel sick",
        "getting sick", "got sick", "throwing up", "nauseous", "fever",
        "sore throat", "head cold", "flu", "stomach bug"
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

Intent ClassifyIntent(std::string_view message, const std::vector<ChatMessage>& recent) {
    const std::string text = Lower(message);
    Intent intent;

    intent.wipe_all = LooksLikeWipeAll(text);
    intent.forget = intent.wipe_all || LooksLikeForget(text);
    intent.asks_for_list = ContainsAny(text, {"list", "what are the", "name the", "give me the"});
    const bool physical_illness = LooksLikePhysicalIllness(text);
    intent.emotional = !LooksLikePracticalLife(text) && !physical_illness && ContainsAny(text, {
        "i feel", "i'm feeling", "im feeling", "i am feeling", "i'm sad", "im sad",
        "i'm anxious", "im anxious", "i am anxious", "i'm scared", "im scared",
        "overwhelmed", "stress", "stressed", "anxiety", "lonely", "anger", "angry", "hurt", "grief", "ashamed",
        "depressed", "hopeless", "panic", "afraid", "confused about my life",
        "talking down to me", "talks down to me", "disrespect", "disrespected",
        "worthless", "respect me", "respect from", "my mom", "my mother",
        "my dad", "my father", "my family", "my boss", "evil boss",
        "my friend", "my brother", "my sister", "my wife", "my husband",
        "my spouse", "my partner", "my coworker", "makes me feel", "made me feel",
        "always criticizes", "keeps mocking", "betrayed me", "too sensitive",
        "nothing happened", "i end up apologizing", "feel stupid", "dread opening",
        "replaying every conversation", "humiliated", "laugh it off",
        "today has been rough", "today has been so rough", "been so rough",
        "rough day", "rough few", "difficult day",
        "hard day", "today has been better", "change is needed", "burn the ships",
        "feeling down", "a little down", "woke up like this", "don't even know",
        "dont even know", "uphill battle", "down to my bones", "head throbbing",
        "shoulders", "neck", "inadequate",
        "you aren't the same", "you arent the same", "you are not the same",
        "i hate that you aren't", "i hate that you arent", "not the same anymore",
        "you feel different", "you don't feel the same", "you dont feel the same"
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
    } else if (intent.intense) {
        intent.mode = IntentMode::Counseling;
    } else if (intent.emotional || trauma_related) {
        intent.emotional = true;
        intent.mode = IntentMode::Reflective;
    } else if (LooksLikeSocialChat(text) || LooksLikePositiveSlang(text)) {
        if (RecentHasOpenEmotionalThread(recent) && !PreviousUserWasSocialSignal(recent)) {
            intent.mode = IntentMode::Reflective;
            intent.emotional = true;
        } else {
            intent.mode = IntentMode::Social;
        }
    } else if (LooksLikeCreative(text)) {
        intent.mode = IntentMode::Creative;
    } else if (physical_illness) {
        intent.mode = IntentMode::DirectAnswer;
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

Intent ClassifyIntent(std::string_view message) {
    static const std::vector<ChatMessage> no_recent;
    return ClassifyIntent(message, no_recent);
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
    }
    return "direct_answer";
}

}  // namespace symbion
