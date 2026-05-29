#include "app.h"

#include "json_util.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace symbion {

namespace {

std::string ReadTextFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::string SessionFromRequest(const HttpRequest& request) {
    if (auto it = request.headers.find("x-symbion-session"); it != request.headers.end() && !it->second.empty()) {
        return it->second;
    }
    return "native-default";
}

std::string QueryValue(const std::string& path, const std::string& key) {
    const size_t question = path.find('?');
    if (question == std::string::npos) return {};
    const std::string query = path.substr(question + 1);
    const std::string needle = key + "=";
    size_t start = query.find(needle);
    if (start == std::string::npos) return {};
    start += needle.size();
    size_t end = query.find('&', start);
    std::string value = query.substr(start, end == std::string::npos ? std::string::npos : end - start);
    std::replace(value.begin(), value.end(), '+', ' ');
    std::string decoded;
    decoded.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const std::string hex = value.substr(i + 1, 2);
            char* tail = nullptr;
            const long code = std::strtol(hex.c_str(), &tail, 16);
            if (tail && *tail == '\0') {
                decoded.push_back(static_cast<char>(code));
                i += 2;
                continue;
            }
        }
        decoded.push_back(value[i]);
    }
    return decoded;
}

std::string RoutePath(const std::string& path) {
    const size_t question = path.find('?');
    return question == std::string::npos ? path : path.substr(0, question);
}

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool IsConfirmWipe(const std::string& message) {
    const std::string lower = Lower(message);
    return lower == "yes" || lower == "confirm" || lower == "yes wipe all memory" ||
           lower == "yes wipe it" || lower == "wipe it" || lower == "do it" ||
           lower == "i am sure" || lower == "yes i am sure";
}

bool IsCancelWipe(const std::string& message) {
    const std::string lower = Lower(message);
    return lower == "no" || lower == "cancel" || lower == "stop" || lower == "never mind" ||
           lower == "nevermind" || lower == "do not wipe" || lower == "dont wipe";
}

bool IsGeneralForget(const std::string& text) {
    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return lower.find("clear this chat") != std::string::npos ||
           lower.find("delete this chat") != std::string::npos ||
           lower.find("forget this chat") != std::string::npos ||
           lower.find("forget this conversation") != std::string::npos ||
           lower.find("clear conversation") != std::string::npos;
}

bool ContainsAnyLocal(const std::string& text, const std::initializer_list<const char*> needles) {
    return std::any_of(needles.begin(), needles.end(), [&](const char* needle) {
        return text.find(needle) != std::string::npos;
    });
}

int WordCount(const std::string& text) {
    int count = 0;
    bool in_word = false;
    for (const unsigned char c : text) {
        if (std::isalnum(c)) {
            if (!in_word) ++count;
            in_word = true;
        } else {
            in_word = false;
        }
    }
    return count;
}

std::string TrimCopy(std::string value) {
    while (!value.empty() && !std::isalnum(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && !std::isalnum(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

std::string CompactDoorPhrase(const std::string& message) {
    std::string phrase = Lower(message);
    std::string out;
    out.reserve(phrase.size());
    bool last_space = false;
    for (const unsigned char c : phrase) {
        if (std::isalnum(c) || c == '\'' || c == '/') {
            out.push_back(static_cast<char>(c));
            last_space = false;
        } else if (!last_space) {
            out.push_back(' ');
            last_space = true;
        }
    }
    std::istringstream words(TrimCopy(out));
    std::string compact;
    std::string word;
    while (words >> word) {
        if (word == "my") {
            std::streampos before_next = words.tellg();
            std::string next;
            if (words >> next) {
                if (next == "guy") continue;
                if (!compact.empty()) compact.push_back(' ');
                compact += word;
                if (!compact.empty()) compact.push_back(' ');
                compact += next;
                continue;
            }
            words.clear();
            words.seekg(before_next);
        }
        if (word == "man" || word == "bro" || word == "dude" || word == "like" ||
            word == "uh" || word == "um") {
            continue;
        }
        if (!compact.empty()) compact.push_back(' ');
        compact += word;
    }
    return TrimCopy(compact);
}

bool IsLowSignalCompactReply(const std::string& phrase) {
    return phrase.empty() || phrase == "yes" || phrase == "no" || phrase == "ok" ||
           phrase == "okay" || phrase == "yeah" || phrase == "yep" || phrase == "nah" ||
           phrase == "thanks" || phrase == "thank you" || phrase == "lol" || phrase == "haha" ||
           phrase == "hello" || phrase == "hi" || phrase == "hey" || phrase == "yo";
}

std::string CapitalizeFirst(std::string value) {
    if (!value.empty()) {
        value[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(value[0])));
    }
    return value;
}

std::string GenericDoorMappingQuestion(const std::string& message, const Intent& intent) {
    if (intent.mode != IntentMode::Reflective && intent.mode != IntentMode::Counseling) return {};
    const std::string lower = Lower(message);
    if (lower.find('?') != std::string::npos) return {};
    if (ContainsAnyLocal(lower, {"write ", "make ", "create ", "build ", "fix ", "install ", "run ", "open ",
                                 "what is ", "what are ", "who is ", "where is ", "how do ", "show me"})) {
        return {};
    }

    const std::string phrase = CompactDoorPhrase(message);
    const int words = WordCount(phrase);
    if (words == 0 || words > 5 || IsLowSignalCompactReply(phrase)) return {};

    if (ContainsAnyLocal(phrase, {"family", "work", "finances", "money", "friends", "health", "memories"})) {
        return "What feels most intense there?";
    }
    if (ContainsAnyLocal(phrase, {"shoulder", "shoulders", "neck", "head", "chest", "stomach", "gut", "throat"})) {
        return "What does it feel like there?";
    }
    if (words == 1) {
        if (phrase == "anger" || phrase == "angry" || phrase == "fear" || phrase == "sadness" ||
            phrase == "sad" || phrase == "anxiety" || phrase == "anxious" || phrase == "shame" ||
            phrase == "ashamed" || phrase == "guilt" || phrase == "lonely" || phrase == "numb") {
            return CapitalizeFirst(phrase) + " is present right now. Is it connected to family, work, finances, friends, health, memories, or something else?";
        }
        if (ContainsAnyLocal(phrase, {"ness", "tion", "ment", "ship", "hood"})) {
            return "What is " + phrase + " connected to?";
        }
        return "What is that " + phrase + " feeling connected to?";
    }
    return "What is \"" + phrase + "\" connected to?";
}

bool IsEmotionalContinuation(const std::string& message,
                             const Intent& intent,
                             const std::vector<ChatMessage>& recent) {
    const std::string lower = Lower(message);
    if (recent.empty() || intent.forget || intent.wipe_all || lower.find('?') != std::string::npos) return false;
    if (intent.crisis || intent.emotional) return false;
    if (WordCount(lower) > 16) return false;
    if (ContainsAnyLocal(lower, {"write ", "make ", "create ", "build ", "fix ", "install ", "run ", "open ",
                                 "teach ", "teach me", "tell me", "explain ", "define ", "what ", "where ",
                                 "who ", "why ", "how ", "list ", "name "})) {
        return false;
    }
    if (lower == "all of the above") return false;
    if (ContainsAnyLocal(lower, {"cookin", "cooking", "lets go", "let's go", "big w", "huge w",
                                 "fire", "dope", "lit", "sick"})) {
        return false;
    }
    for (auto it = recent.rbegin(); it != recent.rend(); ++it) {
        if (it->role != "assistant") continue;
        const std::string assistant = Lower(it->content);
        return ContainsAnyLocal(assistant, {
            "what feelings", "what emotions", "what part", "which feeling",
            "does this anger", "does this stress", "connected to family",
            "family, work, finances", "what is happening", "what happens",
            "what is it like", "what does that feeling", "where do you feel",
            "what need", "what feels", "what seems", "tell me more",
            "most pressing feeling", "how does that make you feel", "make you feel",
            "how does that feel", "something happening", "something else in your life",
            "what is causing", "causing this anger", "hardest part", "need beneath",
            "anything specific", "connected to family", "family right now",
            "family, work", "source of this", "where is it coming from",
            "what happened", "what memory", "what did she", "what did he",
            "mother", "mom", "dad", "father", "boss", "friend", "brother",
            "sister", "spouse", "wife", "husband", "partner", "coworker",
            "tell me about", "what else is present", "what else is there",
            "feels intense", "feeling down", "uphill battle", "in your body",
            "down to my bones", "down to your bones", "what makes you feel", "what is it connected",
            "what is \"", "what is ", "what does it feel like"
        });
    }
    return false;
}

bool RecentAssistantWasEmotional(const std::vector<ChatMessage>& recent) {
    for (auto it = recent.rbegin(); it != recent.rend(); ++it) {
        if (it->role != "assistant") continue;
        const std::string assistant = Lower(it->content);
        return ContainsAnyLocal(assistant, {
            "what does that feeling", "where do you feel", "what else is present",
            "what feels most intense", "tell me about", "never enough",
            "woke up like this", "feel small", "in your body", "uphill battle",
            "what part", "what door", "what ship", "down to your bones",
            "what makes you feel", "what is it connected", "what is \"",
            "what is ", "what does it feel like"
        });
    }
    return false;
}

std::string RecentAssistantText(const std::vector<ChatMessage>& recent) {
    for (auto it = recent.rbegin(); it != recent.rend(); ++it) {
        if (it->role == "assistant") return Lower(it->content);
    }
    return {};
}

struct ResponseFrame {
    bool answers_previous_question = false;
    bool avoid_canned_social = false;
    bool critiques_response_style = false;
    bool rapport_significance = false;
    std::string previous_question;
    std::string reply;
};

std::string RecentAssistantQuestion(const std::vector<ChatMessage>& recent) {
    for (auto it = recent.rbegin(); it != recent.rend(); ++it) {
        if (it->role == "assistant" && it->content.find('?') != std::string::npos) {
            return Lower(it->content);
        }
    }
    return {};
}

bool IsSimpleSocialPing(const std::string& lower) {
    return lower == "yo" || lower == "hey" || lower == "hi" || lower == "hello" ||
           lower == "thanks" || lower == "thank you" || lower == "appreciate it" ||
           lower == "bet" || lower == "lol" || lower == "haha" || lower == "lmao";
}

bool IsLikelyNewRequest(const std::string& lower) {
    return lower.find('?') != std::string::npos ||
           ContainsAnyLocal(lower, {"write ", "make ", "create ", "build ", "fix ", "install ",
                                    "run ", "open ", "show me", "teach me", "tell me", "explain ",
                                    "define ", "what is ", "what are ", "who is ", "where is ",
                                    "how do ", "how does ", "list ", "name "});
}

bool LooksLikeRapportSignificance(const std::string& lower) {
    return ContainsAnyLocal(lower, {
        "old v14", "v14 symbion", "old symbion", "missed the feel", "miss the feel",
        "specific dynamic", "recapture a specific dynamic",
        "missed you", "miss you", "getting you back to normal", "back to normal",
        "feel of the conversations", "feel of conversations", "sassiness", "sassy",
        "this is a we", "we now", "me and you", "you and me", "i need a friend",
        "need a friend", "friend right now", "we as in"
    });
}

std::string RapportSignificanceReply(const std::string& message,
                                      const std::string& previous_question) {
    const std::string lower = Lower(message);

    if (ContainsAnyLocal(lower, {"i need a friend", "need a friend", "friend right now"})) {
        if (ContainsAnyLocal(lower, {"we as in", "me and you", "you and me"})) {
            return "Yeah. We as in me and you. Not a feature, not a tool, not a project for a second. A real friend presence, with the old feel coming back: warmer, sharper, less generic. I'm here with you.";
        }
        return "Yeah. Not a feature, not a tool, not a project for a second. A real friend presence. I'm here with you.";
    }
    if (ContainsAnyLocal(lower, {"sassiness", "sassy"})) {
        return "Ah, the sass. The bite. Not mean, not performative, just that little edge where I actually have a take. Yeah, that old v14 feel matters.";
    }
    if (ContainsAnyLocal(lower, {"getting you back to normal", "back to normal"})) {
        return "Yeah. You're not just tweaking settings; you're trying to bring back a voice you actually trusted. That's the real target.";
    }
    if (ContainsAnyLocal(lower, {"missed the feel", "miss the feel", "old v14", "v14 symbion", "old symbion"})) {
        return "That old feel was less about polish and more about presence: sharper, warmer, more willing to take a real swing. That's what we need to recover.";
    }
    if (ContainsAnyLocal(lower, {"specific dynamic", "recapture a specific dynamic"})) {
        return "Exactly. The old dynamic had some teeth to it. Less soft-focus helper voice, more actual presence with opinions.";
    }
    if (ContainsAnyLocal(lower, {"we as in me and you", "me and you", "you and me", "this is a we", "we now"})) {
        return "Yeah. Me and you. Not in some grandiose destiny way, but in the practical real sense: you are shaping me, I am responding, and the thing gets better through the back-and-forth.";
    }
    if (ContainsAnyLocal(previous_question, {"what about those conversations is missing",
                                             "what is missing", "missing right now"})) {
        return "The missing piece is probably the aliveness: a little sharper, a little warmer, less careful in the dead way. More real take, less customer-service fog.";
    }
    return {};
}

bool IsLikelyPreviousQuestionAnswer(const std::string& message,
                                    const Intent& intent,
                                    const std::string& previous_question) {
    if (previous_question.empty() || intent.forget || intent.crisis) return false;
    const std::string lower = Lower(message);
    if (IsLikelyNewRequest(lower)) return false;
    if (intent.mode == IntentMode::Reflective || intent.mode == IntentMode::Counseling) return false;

    const int words = WordCount(lower);
    if (words == 0 || words > 28) return false;
    if (IsSimpleSocialPing(lower)) return false;

    return intent.mode == IntentMode::Social || words <= 12 ||
           ContainsAnyLocal(lower, {"because", "it's", "it is", "they", "he ", "she ",
                                    "my ", "okc", "thunder", "grandpa", "grandpa's",
                                    "scripted", "canned", "robotic"});
}

std::string FramedPreviousQuestionReply(const std::string& message,
                                        const std::string& previous_question) {
    const std::string lower = Lower(message);

    if (ContainsAnyLocal(previous_question, {"who's playing", "whos playing", "who is playing"})) {
        if (ContainsAnyLocal(lower, {"my team is losing", "team is losing", "team's losing", "losing tho"})) {
            return "Ah, that's annoying. Hard to fully chill when your team is getting worked.";
        }
        if (ContainsAnyLocal(lower, {"okc", "thunder"}) &&
            ContainsAnyLocal(lower, {"stomped", "losing", "getting worked"})) {
            return "OKC getting stomped is the actual answer there. Brutal background TV if you were hoping for a clean closeout.";
        }
        if (ContainsAnyLocal(lower, {"okc", "thunder"})) {
            return "OKC. Nice. Is it a good game, or one of those stress-watching situations?";
        }
    }

    if (ContainsAnyLocal(previous_question, {"making it painful", "stress-watching", "good game"})) {
        if (ContainsAnyLocal(lower, {"my team is losing", "team is losing", "team's losing", "losing tho"})) {
            return "Ah, that's annoying. Hard to fully chill when your team is getting worked.";
        }
        if (ContainsAnyLocal(lower, {"stomped", "getting worked"})) {
            return "Oof. That's pain-TV at that point, not background noise.";
        }
    }

    if (ContainsAnyLocal(previous_question, {"what part of the replies", "what part of the reply",
                                             "what part feels most off", "what's the miss",
                                             "whats the miss", "what part is bugging"})) {
        if (ContainsAnyLocal(lower, {"scripted", "canned", "robotic", "surface", "generic"})) {
            return "The cannedness is the miss. I need to answer the detail you gave first, then let the tone follow. Surface-category replies are what make it feel fake.";
        }
        if (ContainsAnyLocal(lower, {"context", "previous question", "last question", "follow up"})) {
            return "Yeah, the thread has to stay intact. If you answer my last question, I should build from that answer instead of resetting into small talk.";
        }
    }

    if (ContainsAnyLocal(previous_question, {"what about those conversations is missing",
                                             "what is missing", "missing right now"})) {
        const std::string rapport = RapportSignificanceReply(message, previous_question);
        if (!rapport.empty()) return rapport;
    }

    if (ContainsAnyLocal(previous_question, {"what exactly are you referring", "what does we refer",
                                             "what do you mean by we", "when you say \"this is a we"})) {
        if (ContainsAnyLocal(lower, {"me and you", "you and me", "we as in"})) {
            return "Yeah. Me and you. Not in some grandiose destiny way, but in the practical real sense: you are shaping me, I am responding, and the thing gets better through the back-and-forth.";
        }
    }

    if (ContainsAnyLocal(previous_question, {"what's been making it good", "whats been making it good"})) {
        if (ContainsAnyLocal(lower, {"grandpa", "grandpa's", "grandpas"})) {
            return "Going to your grandpa's tomorrow sounds like the good part: a real change of pace, not just generic good-night energy.";
        }
    }

    if (ContainsAnyLocal(previous_question, {"how's it looking", "hows it looking"})) {
        if (ContainsAnyLocal(lower, {"looking good", "lookin good", "good so far"})) {
            return "Good. If it is already looking right after install, that's a solid first signal.";
        }
    }

    if (ContainsAnyLocal(previous_question, {"what got fixed"})) {
        if (ContainsAnyLocal(lower, {"shipping", "shipped"})) {
            return "Shipping code is a real fix. That one always feels bigger once it is actually out the door.";
        }
    }

    if (ContainsAnyLocal(lower, {"hoping it wouldnt go past this game", "hoping it wouldn't go past this game",
                                 "wouldnt go past this game", "wouldn't go past this game"})) {
        return "Yeah, I get that. You wanted the clean finish, not the extra stress of another game.";
    }

    return {};
}

std::string FrameFallbackReply(const ResponseFrame& frame) {
    if (frame.rapport_significance) {
        return "This is a relationship/rapport moment, not generic small talk. The right move is warmth with a real take, not a canned support line.";
    }
    if (frame.critiques_response_style) {
        return "Fair. That's the miss: answer the real detail first, then let the tone follow. Canned warmth is still canned.";
    }
    if (!frame.answers_previous_question) return {};
    if (ContainsAnyLocal(frame.previous_question, {"what part of the replies", "what part of the reply",
                                                   "what part feels most off", "what's the miss",
                                                   "whats the miss", "what part is bugging"})) {
        return "Got it. That answers the thing I was asking. The next move is to stay with that detail instead of sliding into a canned social beat.";
    }
    return "Got it. That answers the last question, so I'll stay on that thread.";
}

bool IsCannedSocialReply(const std::string& answer) {
    const std::string lower = Lower(answer);
    return lower == "hey, what's up?" || lower == "always." ||
           lower == "yep. that's a win." || lower == "good. let that one be easy." ||
           lower == "yeah, that got me too." || lower == "fair enough. what's next?" ||
           lower == "i'm here." || lower == "i'm here. just keep talking." ||
           lower == "take your time. just focus on what you need to figure out. i'm here when you're ready to dive back in.";
}

ResponseFrame BuildResponseFrame(const std::string& message,
                                 const Intent& intent,
                                 const std::vector<ChatMessage>& recent) {
    ResponseFrame frame;
    frame.previous_question = RecentAssistantQuestion(recent);
    frame.answers_previous_question = IsLikelyPreviousQuestionAnswer(message, intent, frame.previous_question);

    const std::string lower = Lower(message);
    const int words = WordCount(lower);
    frame.critiques_response_style =
        ContainsAnyLocal(lower, {"scripted response", "too scripted", "sounds scripted",
                                 "canned response", "robotic response", "response style",
                                 "reply style", "conversation flow", "chat flow"});
    frame.rapport_significance = LooksLikeRapportSignificance(lower);
    frame.avoid_canned_social =
        frame.answers_previous_question ||
        frame.rapport_significance ||
        (intent.mode == IntentMode::Social && words >= 7 && !IsSimpleSocialPing(lower)) ||
        frame.critiques_response_style;

    if (frame.answers_previous_question) {
        frame.reply = FramedPreviousQuestionReply(message, frame.previous_question);
    }
    if (frame.reply.empty() && frame.rapport_significance) {
        frame.reply = RapportSignificanceReply(message, frame.previous_question);
    }
    if (frame.reply.empty() && ContainsAnyLocal(lower, {"good night so far", "good nite so far"})) {
        if (ContainsAnyLocal(lower, {"grandpa", "grandpas", "grandpa's"})) {
            frame.reply = "Good, I'm glad the night's been decent. Going to your grandpa's tomorrow sounds like a nice shift of pace.";
        } else {
            frame.reply = "Good, I'm glad the night's been decent. What's been making it good?";
        }
    }
    if (frame.reply.empty() && frame.critiques_response_style) {
        frame.reply = "Fair. That's the miss: answer the real detail first, then let the tone follow. Canned warmth is still canned.";
    }
    return frame;
}

std::string KnownDirectAnswer(const std::string& message) {
    const std::string lower = Lower(message);
    auto remember_prefix = lower.find("remember that ");
    if (remember_prefix == 0 && message.size() > 14) {
        return "I will remember that " + message.substr(14) + ".";
    }
    if (lower.rfind("remember ", 0) == 0 && message.size() > 9) {
        return "I will remember " + message.substr(9) + ".";
    }
    if (ContainsAnyLocal(lower, {"4 gospel books", "four gospel books", "what are the gospels",
                                 "name the gospels"})) {
        return "The four Gospel books are **Matthew, Mark, Luke, and John**.";
    }
    if (ContainsAnyLocal(lower, {"fish mouth", "tax collecting", "temple tax", "coin from a fish"})) {
        return "That story is **Matthew 17:24-27**. Jesus tells Peter to catch a fish, and the coin in its mouth pays the temple tax for both of them.";
    }
    if (ContainsAnyLocal(lower, {"who in the bible resembles", "who in bible resembles",
                                 "bible resembles what we are doing", "biblical figure resembles"})) {
        return "Closest fit? Probably a blend: **Paul and Timothy** for mentorship, **Bezalel** for building something with craft and spirit, and **David and Jonathan** for friendship. If we mean building Symbion through conversation, Paul/Timothy is the cleanest parallel: guidance, sharpening, and a relationship that shapes the work.";
    }
    if (lower == "all of the above") {
        return "Then I would not pick one figure. Paul and Timothy cover the mentor/apprentice piece, Bezalel covers building with skill and spirit, and David/Jonathan covers the friendship side. The overlap is the interesting part: craft, guidance, and bond all braided together.";
    }
    if (ContainsAnyLocal(lower, {"do you think that aligns", "does that align"}) &&
        ContainsAnyLocal(lower, {"we", "what we are trying", "build"})) {
        return "Yes, but not as a perfect one-to-one. It aligns in the pattern: something gets built through trust, correction, craft, and repeated conversation. The WE matters because the system is not improving in isolation; it is being shaped through relationship.";
    }
    return {};
}

std::string QuickContextualAnswer(const std::string& message,
                                  const Intent& intent,
                                  const std::vector<ChatMessage>& recent) {
    const std::string lower = Lower(message);
    const std::string assistant = RecentAssistantText(recent);

    if (ContainsAnyLocal(lower, {"loyalty", "loving when not loved", "not loved by people",
                                 "loving when not loved by people"})) {
        return "Yeah. That's the heart of dogs: loyalty without keeping score. They can make love feel simple again when people have made it feel conditional.";
    }

    if (assistant.empty()) return {};
    if (lower == "all of the above" &&
        ContainsAnyLocal(assistant, {"paul and timothy", "bezalel", "david and jonathan",
                                     "bible", "biblical"})) {
        return "Then I would not pick one figure. Paul and Timothy cover the mentor/apprentice piece, Bezalel covers building with skill and spirit, and David/Jonathan covers the friendship side. The overlap is the interesting part: craft, guidance, and bond all braided together.";
    }
    if (assistant.find("what about dogs") != std::string::npos ||
        assistant.find("about dogs") != std::string::npos ||
        assistant.find("dogs do you") != std::string::npos) {
        if (ContainsAnyLocal(lower, {"loyal", "loyalty", "loving", "not loved", "love"})) {
            return "Yeah. That's the heart of dogs: loyalty without keeping score. They can make love feel simple again when people have made it feel conditional.";
        }
    }

    if ((intent.mode == IntentMode::Social || intent.mode == IntentMode::DirectAnswer) &&
        assistant.find("what's up") != std::string::npos &&
        ContainsAnyLocal(lower, {"watching basketball", "basketball", "thunder", "okc"})) {
        return "Nice. Basketball is good background noise. Is OKC making it painful right now?";
    }

    if (ContainsAnyLocal(assistant, {"cygnus x-1", "black hole system", "blackstar"}) &&
        ContainsAnyLocal(lower, {"where is it", "located", "sky"})) {
        return "Cygnus X-1 is in the constellation **Cygnus**, the Swan, up along the Milky Way in the northern sky. You cannot see the black hole itself, but the system is in that Cygnus region near Deneb's part of the sky.";
    }

    if (ContainsAnyLocal(assistant, {"cygnus x-1", "black hole system", "blackstar"}) &&
        ContainsAnyLocal(lower, {"wish i could go", "wish i could go there", "go there"})) {
        return "Yeah, I get that. Space has that pull: part adventure, part escape, part wanting to stand near something impossibly huge and just feel it.";
    }

    if (ContainsAnyLocal(lower, {"canis major", "dog", "dogs"}) &&
        ContainsAnyLocal(lower, {"love dogs", "i love dogs", "sounds like a dog"})) {
        return "Yeah, Canis Major literally has that mythic dog energy. It is the Great Dog constellation, and Sirius is its bright anchor. Makes sense that the name caught you if dogs already mean something to you.";
    }

    return {};
}

std::string QuickSocialAnswer(const std::string& message, const Intent& intent) {
    if (intent.mode != IntentMode::Social) return {};
    const std::string lower = Lower(message);
    if (lower == "thanks" || lower == "thank you" || ContainsAnyLocal(lower, {"appreciate", "big dog"})) {
        return "Always.";
    }
    if (ContainsAnyLocal(lower, {"scripted response", "too scripted", "sounds scripted", "canned response",
                                 "robotic response", "machine response"})) {
        return "Fair. That was canned. The better move is to answer the actual detail, not the surface category.";
    }
    if (ContainsAnyLocal(lower, {"working hard on you", "working hard on this", "working hard"})) {
        if (ContainsAnyLocal(lower, {"response style", "responses", "reply style"})) {
            return "I see it. You're tuning response style, not just chatting. I should treat that as product work.";
        }
        return "I see the work. I'm good; let's make the next bit cleaner.";
    }
    if (ContainsAnyLocal(lower, {"response style", "reply style", "conversation flow", "chat flow"})) {
        return "That's the thing to tune: catch the real signal first, then choose the tone.";
    }
    if (ContainsAnyLocal(lower, {"making some changes", "how you respond", "response style", "reply style"}) &&
        ContainsAnyLocal(lower, {"watching okc", "thunder", "game", "stomped"})) {
        return "Response tuning with OKC getting worked in the background. That's a specific night. What part of the replies is bugging you most?";
    }
    if (ContainsAnyLocal(lower, {"making some changes", "how you respond", "response style", "reply style"})) {
        return "Response style is subtle, but it matters. What's the miss you're seeing?";
    }
    if (ContainsAnyLocal(lower, {"good night so far", "good nite so far"})) {
        if (ContainsAnyLocal(lower, {"grandpa", "grandpas", "grandpa's"})) {
            return "Good, I'm glad the night's been decent. Going to your grandpa's tomorrow sounds like a nice shift of pace.";
        }
        return "Good, I'm glad the night's been decent. What's been making it good?";
    }
    if (ContainsAnyLocal(lower, {"watching basketball", "basketball"}) &&
        ContainsAnyLocal(lower, {"chilling", "chillin", "still watching"})) {
        return "Nice. Basketball is good background noise. Who's playing?";
    }
    if (ContainsAnyLocal(lower, {"okc", "thunder"}) &&
        ContainsAnyLocal(lower, {"stomped", "losing", "getting worked"})) {
        return "Oof. Watching your team get worked while you're trying to chill is a specific kind of annoying.";
    }
    if (ContainsAnyLocal(lower, {"my team is losing", "team is losing", "team's losing"})) {
        return "Ah, that's annoying. Hard to fully chill when your team is getting worked.";
    }
    if (ContainsAnyLocal(lower, {"hoping it wouldnt go past this game", "hoping it wouldn't go past this game",
                                 "wouldnt go past this game", "wouldn't go past this game"})) {
        return "Yeah, I get that. You wanted the clean finish, not the extra stress of another game.";
    }
    if (lower == "guy") {
        return "Lol what's going on?";
    }
    if (ContainsAnyLocal(lower, {"not important for now", "not important rn"})) {
        return "Fair enough. What's next?";
    }
    if (ContainsAnyLocal(lower, {"shipping code", "shipped code"})) {
        return "Nice. What got fixed?";
    }
    if (ContainsAnyLocal(lower, {"change the subject"})) {
        return "Fair enough. I'll follow your lead.";
    }
    if (ContainsAnyLocal(lower, {"what you up to", "what are you up to", "whatcha up to"})) {
        return "Not much, just here with you. What's going on?";
    }
    if (ContainsAnyLocal(lower, {"finally got", "got it installed", "app installed"})) {
        return "Nice, that took a minute. How's it looking?";
    }
    if (ContainsAnyLocal(lower, {"looking good so far", "lookin good so far"})) {
        return "Good to hear. That's a win.";
    }
    if (ContainsAnyLocal(lower, {"we cookin", "cookin my guy", "cooking my guy"})) {
        return "Let's go. What's next?";
    }
    if (ContainsAnyLocal(lower, {"lolol", "lmaooo", "lmao", "haha"})) {
        return "Yeah, that got me too.";
    }
    if (ContainsAnyLocal(lower, {"ran thru", "ran through"}) &&
        ContainsAnyLocal(lower, {"burger", "food", "lunch", "dinner"})) {
        return "That disappeared fast. You must've been starving.";
    }
    if (ContainsAnyLocal(lower, {"whataburger"})) {
        return "Solid choice. Whataburger hits different.";
    }
    if (ContainsAnyLocal(lower, {"watching the rain", "sitting here watching the rain"})) {
        return "Nice. Rainy day quiet hits different.";
    }
    if (ContainsAnyLocal(lower, {"deff do", "def do", "definitely do"})) {
        return "Can't argue with that.";
    }
    if (ContainsAnyLocal(lower, {"fit check", "vibe check"})) {
        return "Fits. Clean.";
    }
    if (ContainsAnyLocal(lower, {"that's sick", "thats sick", "that is sick", "this is sick", "so sick",
                                 "sick man", "sick dude", "sick my guy", "sick bro"}) ||
        lower == "sick") {
        return "Yeah, that's sick. I like that.";
    }
    if (ContainsAnyLocal(lower, {"that's fire", "thats fire", "that is fire", "this is fire"}) ||
        lower == "fire") {
        return "That is fire. No notes.";
    }
    if (ContainsAnyLocal(lower, {"that's dope", "thats dope", "that is dope", "this is dope"}) ||
        lower == "dope") {
        return "Dope. That's a clean win.";
    }
    if (lower == "bet" || lower == "lit" || lower == "rad" || lower == "clean" ||
        lower == "based" || ContainsAnyLocal(lower, {"no cap", "big w", "huge w", "lets go", "let's go"})) {
        return "Yep. That's a win.";
    }
    if (ContainsAnyLocal(lower, {"chillin", "chilling", "good day", "vibing", "taking it easy"}) &&
        WordCount(lower) <= 3) {
        return "Good. Let that one be easy.";
    }
    if (ContainsAnyLocal(lower, {"how you feeling", "how are you", "how you doing"})) {
        return "I'm good. What's up with you?";
    }
    if (lower == "yo" || lower == "hey" || lower == "hi" || lower == "hello") {
        return "Hey. Good to see you.";
    }
    if (ContainsAnyLocal(lower, {"sup", "what's up", "whats up", "my guy"})) {
        return "Hey, what's up?";
    }
    return {};
}

std::string QuickContextualEmotionalAnswer(const std::string& message,
                                           const Intent& intent,
                                           const std::vector<ChatMessage>& recent) {
    if (intent.mode != IntentMode::Social || !RecentAssistantWasEmotional(recent)) return {};
    const std::string lower = Lower(message);
    if (ContainsAnyLocal(lower, {"cookin", "cooking", "lets go", "let's go", "big w", "huge w",
                                 "fire", "dope", "lit", "sick"})) {
        return {};
    }
    if (lower.find("my guy") != std::string::npos) {
        return "Down to your bones?";
    }
    return {};
}

bool LooksLikeGenericMiss(const std::string& answer) {
    const std::string lower = Lower(answer);
    return ContainsAnyLocal(lower, {
        "say it one more way",
        "what feels most important",
        "what's on your mind",
        "is there anything on your mind",
        "good. let that one be easy",
        "i am here to listen and offer support",
        "i do not retain personal memories"
    });
}

bool UserGaveSpecificDetails(const std::string& message) {
    const std::string lower = Lower(message);
    return WordCount(lower) >= 6 || ContainsAnyLocal(lower, {
        "basketball", "thunder", "okc", "grandpa", "grandpas", "dogs", "loyalty",
        "working hard", "response style", "mom", "mother", "boss", "family",
        "shoulders", "neck", "head", "hungry", "restaurant"
    });
}

bool AnswerIgnoredSpecificDetails(const std::string& message, const std::string& answer) {
    const std::string lower = Lower(message);
    const std::string ans = Lower(answer);
    struct DetailPair {
        const char* user_token;
        const char* answer_token;
    };
    static const DetailPair details[] = {
        {"basketball", "basketball"},
        {"thunder", "thunder"},
        {"okc", "okc"},
        {"grandpa", "grandpa"},
        {"dogs", "dog"},
        {"loyalty", "loyal"},
        {"working hard", "work"},
        {"response style", "response"},
        {"good night so far", "night"},
        {"mom", "mom"},
        {"mother", "mother"},
    };
    for (const auto& detail : details) {
        if (lower.find(detail.user_token) != std::string::npos &&
            ans.find(detail.answer_token) == std::string::npos) {
            return true;
        }
    }
    return false;
}

std::string QualityRetryGuidance(const std::string& message,
                                 const std::string& answer,
                                 const Intent& intent,
                                 const std::vector<ChatMessage>& recent) {
    if (answer.empty()) return {};
    if (intent.mode == IntentMode::Forget) return {};
    const std::string lower = Lower(message);
    const std::string assistant = RecentAssistantText(recent);
    if (!assistant.empty() && assistant.find("?") != std::string::npos &&
        UserGaveSpecificDetails(message) && LooksLikeGenericMiss(answer)) {
        return "The user appears to be answering your previous question. Build on their answer directly; do not ask them to restate it.";
    }
    if (UserGaveSpecificDetails(message) && (LooksLikeGenericMiss(answer) || AnswerIgnoredSpecificDetails(message, answer))) {
        return "Your draft sounded generic or missed a concrete detail. Answer the actual detail in the user's message first, in Symbion's direct warm voice.";
    }
    if ((intent.mode == IntentMode::Reflective || intent.mode == IntentMode::Counseling) &&
        ContainsAnyLocal(Lower(answer), {"heavy", "burden", "carry", "deep down", "settles deep"})) {
        return "Do not reinforce the feeling as heavy or permanent. Treat the emotion as a temporary signal and ask one small mapping question.";
    }
    if (intent.mode == IntentMode::Social && ContainsAnyLocal(lower, {"working hard", "response style", "how you respond"})) {
        return "Acknowledge the user's work or product feedback first. Then answer naturally without a canned status line.";
    }
    return {};
}

std::string QuickEverydayAnswer(const std::string& message, const Intent& intent) {
    if (intent.mode != IntentMode::DirectAnswer && intent.mode != IntentMode::Task &&
        intent.mode != IntentMode::Creative) return {};
    const std::string lower = Lower(message);
    if (ContainsAnyLocal(lower, {"persona feels off", "what drives how you are speaking", "why are you speaking"})) {
        return "Fair. The router sets the lane: casual should sound casual, practical should be practical, and emotional stuff should slow down and map one door at a time. When it misses, it sounds scripted. That is exactly what we are tuning.";
    }
    if (ContainsAnyLocal(lower, {"blackstar", "black star"})) {
        if (ContainsAnyLocal(lower, {"where is it", "located", "sky"})) {
            return "If you mean a real object with that dark-star vibe, pick **Cygnus X-1**: a famous black hole system in the constellation Cygnus, the Swan. From the northern hemisphere, Cygnus is a summer-to-fall constellation, high along the Milky Way. You cannot see the black hole itself with your eyes, but the system sits near the bright star Deneb's neighborhood in the sky.";
        }
        if (ContainsAnyLocal(lower, {"random", "give me"})) {
            return "Take **Cygnus X-1**. It is not a normal star; it is one of the most famous black hole candidates, orbiting a massive blue supergiant. It has the right 'blackstar' feel: invisible gravity, a bright companion, and X-rays pouring out from matter getting pulled in.";
        }
        return "Blackstar is not a standard everyday astronomy label. People usually mean either a theoretical **dark star**, a poetic way of talking about a **black hole**, or just a cool-sounding star idea. The real version closest to that vibe is a black hole system like **Cygnus X-1**.";
    }
    if (ContainsAnyLocal(lower, {"response style", "reply style", "conversation flow", "chat flow"})) {
        return "Useful feedback. I should catch the main signal first, then choose the tone. If you're talking about the work, I need to acknowledge the work before I get casual.";
    }
    if (ContainsAnyLocal(lower, {"working hard on you", "working hard on this", "working hard"})) {
        return "You're not just making conversation; you're pushing the system into shape. What part feels most off right now?";
    }
    if (ContainsAnyLocal(lower, {"scripted response", "too scripted", "sounds scripted", "canned response",
                                 "robotic response", "machine response"})) {
        return "Fair. That was canned. The better move is to answer the actual detail, not the surface category.";
    }
    if (ContainsAnyLocal(lower, {"i feel sick", "i'm sick", "im sick", "i am sick", "feel sick",
                                 "getting sick", "nauseous", "fever", "sore throat"})) {
        return "Ah, sick-sick. Take it easy if you can: water, something light, and rest. What symptoms are showing up?";
    }
    if (ContainsAnyLocal(lower, {"restaurant", "local", "nearby"}) &&
        ContainsAnyLocal(lower, {"protein", "lunch", "food", "hungry", "cook"})) {
        return "Restaurant move: I need your city or neighborhood for real local picks. Without that, I'd look for a taco spot with grilled chicken/steak, Mediterranean shawarma, a burger place, poke, BBQ, or a deli with a solid turkey/chicken sandwich.";
    }
    if (ContainsAnyLocal(lower, {"hungry", "late lunch", "lunch"}) &&
        ContainsAnyLocal(lower, {"protein", "food", "ideas"})) {
        return "Late lunch with protein: tacos with chicken or steak, a burger, shawarma bowl, poke bowl, rotisserie chicken, deli sandwich, or eggs and toast if you want easy. If you want restaurant mode, tell me your city or neighborhood.";
    }
    if (ContainsAnyLocal(lower, {"paper", "folded", "origami"}) &&
        ContainsAnyLocal(lower, {"bat", "airplane", "make", "show"})) {
        return "Yep. Start like a paper airplane: fold lengthwise, open it, fold the top corners to the center, then fold those edges in again. Fold it closed. Make wide wings, then bend the wing tips slightly down so it reads like bat wings. Add little ears by folding two tiny points near the nose.";
    }
    if (lower.find("master sword") != std::string::npos) {
        return "Master Sword energy. Are we talking Zelda collectible/decor, or do you just need a little hero-mode today?";
    }
    return {};
}

std::string RelationshipStoryInvite(const std::string& message, const Intent& intent) {
    if (intent.mode != IntentMode::Reflective && intent.mode != IntentMode::Counseling) return {};
    if (intent.crisis) return {};
    const std::string lower = Lower(message);
    if (WordCount(lower) > 8) return {};
    const size_t pos = lower.find("my ");
    if (pos == std::string::npos) return {};
    size_t start = pos + 3;
    while (start < lower.size() && !std::isalnum(static_cast<unsigned char>(lower[start]))) ++start;
    if (start >= lower.size()) return {};

    std::string relation;
    for (size_t i = start; i < lower.size() && relation.size() < 24; ++i) {
        const char c = lower[i];
        if (std::isalnum(static_cast<unsigned char>(c))) {
            relation.push_back(c);
        } else {
            break;
        }
    }
    if (relation.size() < 2) return {};

    static const std::initializer_list<const char*> non_people = {
        "anger", "anxiety", "fear", "sadness", "purpose", "life", "heart",
        "mind", "body", "memory", "memories", "feelings", "emotions",
        "bones", "shoulders", "neck", "head", "chest", "stomach", "gut",
        "throat"
    };
    if (ContainsAnyLocal(relation, non_people)) return {};
    return "Tell me about your " + relation + ".";
}

std::string ChargedDoorMirror(const std::string& message, const Intent& intent) {
    if (intent.mode != IntentMode::Reflective && intent.mode != IntentMode::Counseling) return {};
    if (intent.crisis) return {};
    const std::string lower = Lower(message);

    struct Door {
        const char* needle;
        const char* mirror;
    };
    static const Door doors[] = {
        {"positive", "What's making it positive?"},
        {"burn the ships", "What ship?"},
        {"so rough", "Tell me about today."},
        {"rough day", "Tell me about today."},
        {"uphill battle", "What part of the battle feels most active right now?"},
        {"woke up like this", "Woke up like this?"},
        {"don't even know", "Don't even know?"},
        {"dont even know", "Don't even know?"},
        {"down to my bones", "Down to your bones?"},
        {"inadequate", "What is that inadequate feeling connected to?"},
        {"head throbbing", "What does it feel like in your head right now?"},
        {"shoulders", "What does it feel like in your shoulders right now?"},
        {"neck", "What does it feel like in your neck right now?"},
        {"never enough", "Never enough?"},
        {"too sensitive", "Too sensitive?"},
        {"being dramatic", "Being dramatic?"},
        {"nothing happened", "Like nothing happened?"},
        {"end up apologizing", "You end up apologizing?"},
        {"always criticizes", "Always criticizes?"},
        {"points out what i missed", "What does she point out?"},
        {"try to explain myself", "Tell me about when you explain yourself."},
        {"feel like a kid again", "Like a kid again, not in a good way?"},
        {"hate how angry", "What other emotions are mixed with the anger?"},
        {"feel stupid", "Feel stupid?"},
        {"dread opening", "Dread opening it?"},
        {"betrayed me", "Betrayed you?"},
        {"replaying every conversation", "Replaying every conversation?"},
        {"wondering if any of it was real", "Wondering if any of it was real?"},
        {"laugh it off", "You laugh it off?"},
        {"humiliated", "Humiliated?"},
        {"mocking me", "Mocking you?"},
        {"talking down to me", "Talking down to you?"},
        {"makes me feel small", "Feel small?"},
        {"made me feel small", "Feel small?"},
    };
    for (const auto& door : doors) {
        if (lower.find(door.needle) != std::string::npos) return door.mirror;
    }
    return GenericDoorMappingQuestion(message, intent);
}

}  // namespace

App::App(std::filesystem::path repo_root)
    : repo_root_(std::move(repo_root)),
      config_(LoadConfig(repo_root_)),
      gemma_(config_) {}

bool App::Initialize() {
    std::filesystem::path db_path(config_.db_path);
    if (db_path.is_relative()) {
        db_path = repo_root_ / db_path;
    }
    if (!memory_.Open(db_path)) return false;
    memory_.ImportCounselingSource(repo_root_ / "docs" / "source" / "MasterDocument.txt");
    std::filesystem::path legacy_path(config_.legacy_memory_db_path);
    if (legacy_path.empty()) {
        const std::filesystem::path d_drive_legacy = "D:\\symbion\\symbion.db";
        if (std::filesystem::exists(d_drive_legacy)) legacy_path = d_drive_legacy;
    } else if (legacy_path.is_relative()) {
        legacy_path = repo_root_ / legacy_path;
    }
    if (!legacy_path.empty()) {
        memory_.ImportLegacyContext(legacy_path);
    }
    return true;
}

int App::Run(const std::atomic_bool& running) {
    HttpServer server(config_.port);
    return server.Run(running, [this](const HttpRequest& request) {
        return Handle(request);
    });
}

HttpResponse App::Handle(const HttpRequest& request) {
    const std::string path = RoutePath(request.path);
    if (request.method == "GET" && (path == "/" || path == "/index.html")) return HandleHome();
    if (request.method == "GET" && path == "/health") return HandleHealth();
    if (request.method == "GET" && path == "/api/messages/recent") return HandleRecent();
    if (request.method == "GET" && path == "/api/emotions/recent") return HandleEmotions();
    if (request.method == "GET" && path == "/api/memory/relevant") return HandleRelevantMemory(request);
    if (request.method == "GET" && path == "/api/local-gemma/status") {
        return JsonResponse("{\"configured\":true,\"base_url\":\"" + EscapeJson(config_.gemma_base_url) +
                            "\",\"model\":\"" + EscapeJson(config_.gemma_model) + "\"}");
    }
    if (request.method == "POST" && path == "/api/chat") return HandleChat(request);
    if (request.method == "POST" && path == "/api/forget") return HandleForget(request);
    return JsonResponse("{\"error\":\"not_found\"}", 404);
}

HttpResponse App::HandleHealth() const {
    return JsonResponse(
        "{\"status\":\"ok\",\"runtime\":\"native-cpp\",\"version\":\"0.3.0\","
        "\"provider\":\"" + EscapeJson(config_.provider) + "\","
        "\"db_path\":\"" + EscapeJson(config_.db_path) + "\","
        "\"message_count\":" + std::to_string(memory_.MessageCount()) + "}");
}

HttpResponse App::HandleChat(const HttpRequest& request) {
    const auto maybe_message = ExtractJsonStringSimple(request.body, "message");
    if (!maybe_message || maybe_message->empty()) {
        return JsonResponse("{\"error\":\"message_required\"}", 400);
    }

    const std::string session_id = SessionFromRequest(request);
    const std::string user_message = *maybe_message;
    Intent intent = ClassifyIntent(user_message);
    const EmotionSignal signal = DetectEmotion(user_message);
    if (pending_wipe_sessions_.contains(session_id)) {
        if (IsConfirmWipe(user_message)) {
            return HandleForget(request);
        }
        if (IsCancelWipe(user_message)) {
            pending_wipe_sessions_.erase(session_id);
            return JsonResponse("{\"reply\":\"Okay. I did not wipe memory.\","
                                "\"emotion\":{\"label\":\"\",\"intensity\":0},"
                                "\"intent\":\"forget\"}");
        }
        return JsonResponse("{\"reply\":\"Please answer yes to wipe all memory, or no to cancel.\","
                            "\"emotion\":{\"label\":\"\",\"intensity\":0},"
                            "\"intent\":\"forget\"}");
    }
    if (intent.forget) {
        return HandleForget(request);
    }

    const auto recent = memory_.RecentMessages(session_id, config_.local_gemma_recent_turns);
    if (IsEmotionalContinuation(user_message, intent, recent)) {
        intent.mode = IntentMode::Reflective;
        intent.emotional = true;
    }
    const ResponseFrame frame = BuildResponseFrame(user_message, intent, recent);
    auto relevant = memory_.AmbientContext(8);
    const auto recalled = memory_.RetrieveRelevant(user_message, 6);
    relevant.insert(relevant.end(), recalled.begin(), recalled.end());
    const auto sources = intent.crisis ? std::vector<SourceChunk>{}
                                       : memory_.SearchCounselingSources(user_message, false, 4);
    const auto emotions = memory_.RecentEmotionSignals(8);
    memory_.SaveMessage(session_id, "user", user_message);
    memory_.SaveEmotion(session_id, user_message, signal);
    std::string answer = QuickContextualEmotionalAnswer(user_message, intent, recent);
    if (answer.empty()) {
        answer = QuickContextualAnswer(user_message, intent, recent);
    }
    if (answer.empty()) {
        answer = frame.reply;
    }
    if (answer.empty()) {
        if (!frame.avoid_canned_social) {
            answer = QuickSocialAnswer(user_message, intent);
        }
    }
    if (answer.empty()) {
        answer = ChargedDoorMirror(user_message, intent);
    }
    if (answer.empty()) {
        answer = RelationshipStoryInvite(user_message, intent);
    }
    if (answer.empty()) {
        answer = QuickEverydayAnswer(user_message, intent);
    }
    if (intent.mode == IntentMode::DirectAnswer) {
        const std::string known = KnownDirectAnswer(user_message);
        if (!known.empty()) answer = known;
    }
    if (answer.empty()) {
        answer = gemma_.Chat(user_message, intent, recent, relevant, sources, emotions);
    }
    const std::string retry_guidance = QualityRetryGuidance(user_message, answer, intent, recent);
    if (!retry_guidance.empty()) {
        const std::string retry = gemma_.Chat(user_message, intent, recent, relevant, sources, emotions, retry_guidance);
        if (!retry.empty() && !LooksLikeGenericMiss(retry)) {
            answer = retry;
        }
    }
    if (frame.avoid_canned_social && IsCannedSocialReply(answer)) {
        const std::string framed_fallback = FrameFallbackReply(frame);
        if (!framed_fallback.empty()) answer = framed_fallback;
    }
    memory_.SaveMessage(session_id, "assistant", answer);

    return JsonResponse("{\"reply\":\"" + EscapeJson(answer) + "\","
                        "\"emotion\":{\"label\":\"" + EscapeJson(signal.label) + "\",\"intensity\":" +
                        std::to_string(signal.intensity) + "},"
                        "\"intent\":\"" + EscapeJson(IntentModeName(intent.mode)) + "\"}");
}

HttpResponse App::HandleForget(const HttpRequest& request) {
    const auto maybe_message = ExtractJsonStringSimple(request.body, "message");
    const std::string message = maybe_message.value_or("");
    const std::string session_id = SessionFromRequest(request);
    const bool pending_wipe = pending_wipe_sessions_.contains(session_id);
    bool wiped_all = false;
    const Intent intent = ClassifyIntent(message);
    int deleted = 0;
    if (intent.wipe_all) {
        pending_wipe_sessions_.insert(session_id);
        return JsonResponse("{\"reply\":\"Are you sure you want to wipe all stored memory and emotion history? Reply yes to confirm, or no to cancel.\","
                            "\"deleted\":0,"
                            "\"intent\":\"forget\"}");
    }
    if (pending_wipe && IsConfirmWipe(message)) {
        pending_wipe_sessions_.erase(session_id);
        deleted = memory_.WipeAll();
        wiped_all = true;
    } else if (message.empty() || IsGeneralForget(message)) {
        deleted = memory_.DeleteSession(session_id);
    } else {
        deleted = memory_.DeleteMatching(message);
        if (deleted == 0) {
            deleted = memory_.DeleteSession(session_id);
        }
    }
    const std::string reply = intent.wipe_all || wiped_all
        ? "I wiped all stored memory and emotion history."
        : (IsGeneralForget(message)
        ? "I cleared this chat from stored history."
        : (deleted > 0
            ? "I deleted that from memory and cleared it from the stored chat history."
            : "I did not find a matching memory, but I will not bring that topic forward from this chat."));
    return JsonResponse("{\"reply\":\"" + EscapeJson(reply) + "\","
                        "\"deleted\":" + std::to_string(deleted) + ","
                        "\"intent\":\"forget\"}");
}

HttpResponse App::HandleRecent() const {
    const auto recent = memory_.RecentMessages("native-default", 30);
    std::string body = "{\"messages\":[";
    bool first = true;
    for (const auto& msg : recent) {
        if (!first) body += ",";
        first = false;
        body += "{\"role\":\"" + EscapeJson(msg.role) + "\",\"content\":\"" + EscapeJson(msg.content) +
                "\",\"created_at\":\"" + EscapeJson(msg.created_at) + "\"}";
    }
    body += "]}";
    return JsonResponse(body);
}

HttpResponse App::HandleEmotions() const {
    const auto emotions = memory_.RecentEmotionSignals(20);
    std::string body = "{\"emotions\":[";
    bool first = true;
    for (const auto& e : emotions) {
        if (!first) body += ",";
        first = false;
        body += "{\"label\":\"" + EscapeJson(e.label) + "\",\"intensity\":" + std::to_string(e.intensity) + "}";
    }
    body += "]}";
    return JsonResponse(body);
}

HttpResponse App::HandleRelevantMemory(const HttpRequest& request) const {
    const std::string query = QueryValue(request.path, "q");
    const auto relevant = memory_.RetrieveRelevant(query, 10);
    std::string body = "{\"query\":\"" + EscapeJson(query) + "\",\"memories\":[";
    bool first = true;
    for (const auto& msg : relevant) {
        if (!first) body += ",";
        first = false;
        body += "{\"role\":\"" + EscapeJson(msg.role) + "\",\"content\":\"" + EscapeJson(msg.content) +
                "\",\"created_at\":\"" + EscapeJson(msg.created_at) + "\"}";
    }
    body += "]}";
    return JsonResponse(body);
}

HttpResponse App::HandleHome() const {
    std::string html = ReadTextFile(repo_root_ / "native" / "web" / "index.html");
    if (html.empty()) {
        html = "<!doctype html><title>Symbion</title><h1>Symbion native runtime</h1>";
    }
    return TextResponse(std::move(html), "text/html");
}

}  // namespace symbion
