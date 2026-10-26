#include "commands.hpp"

#include <algorithm>
#include <cctype>

namespace voice {

const std::vector<CommandInfo>& command_table() {
    // Phrases are written the way the speech models spell them: lowercase, multi-word names split
    // into dictionary words ("claw shot"). Several aliases per item make recognition forgiving.
    static const std::vector<CommandInfo> table = {
        {Command::IronBoots, "Iron Boots", "Botas de Ferro",
            {"iron boots", "iron boot", "heavy boots", "boots"},
            {"botas de ferro", "bota de ferro", "botas", "bota"}},
        {Command::Clawshot, "Clawshot", "Gancho",
            {"claw shot", "hook shot", "hook"},
            {"gancho", "garra"}},
        {Command::DoubleClawshot, "Double Clawshots", "Gancho Duplo",
            {"double claw shot", "double claw", "double hook", "double hook shot"},
            {"gancho duplo", "ganchos duplos", "garra dupla"}},
        {Command::Boomerang, "Gale Boomerang", "Bumerangue",
            {"boomerang", "gale boomerang"},
            {"bumerangue", "bumerangue do vento"}},
        {Command::Bow, "Hero's Bow", "Arco",
            {"bow", "bow and arrow", "arrows"},
            {"arco", "arco e flecha", "flecha", "flechas"}},
        {Command::Lantern, "Lantern", "Lanterna",
            {"lantern", "lamp"},
            {"lanterna", "lamparina"}},
        {Command::Spinner, "Spinner", "Pião",
            {"spinner"},
            {"pião", "peão"}},
        {Command::BallAndChain, "Ball and Chain", "Bola de Ferro",
            {"ball and chain", "iron ball"},
            {"bola de ferro", "bola e corrente"}},
        {Command::DominionRod, "Dominion Rod", "Bastão do Domínio",
            {"dominion rod", "dominion"},
            {"bastão do domínio", "cetro do domínio", "bastão", "cetro"}},
        {Command::Bombs, "Bombs", "Bombas",
            {"bombs", "bomb"},
            {"bombas", "bomba"}},
        {Command::WaterBombs, "Water Bombs", "Bombas Aquáticas",
            {"water bombs", "water bomb"},
            {"bombas aquáticas", "bomba aquática", "bombas de água"}},
        {Command::Bomblings, "Bomblings", "Bombinhas",
            {"bug bombs", "bug bomb", "little bombs"},
            {"bombinhas", "bombinha"}},
        {Command::Slingshot, "Slingshot", "Estilingue",
            {"slingshot", "sling shot"},
            {"estilingue"}},
        {Command::FishingRod, "Fishing Rod", "Vara de Pescar",
            {"fishing rod", "fishing"},
            {"vara de pescar", "vara"}},
        {Command::HawkEye, "Hawkeye", "Olho de Falcão",
            {"hawk eye", "hawkeye"},
            {"olho de falcão", "olho de águia"}},
        {Command::HorseCall, "Horse Call", "Chamar Cavalo",
            {"horse call", "horse", "call horse"},
            {"chamar cavalo", "cavalo", "chamar o cavalo"}},
        {Command::Bottle, "Bottle", "Garrafa",
            {"bottle"},
            {"garrafa"}},
    };
    return table;
}

const CommandInfo& info(Command command) {
    return command_table()[static_cast<size_t>(command)];
}

const char* display_name(Command command, Language language) {
    const CommandInfo& i = info(command);
    return language == Language::Portuguese ? i.display_pt : i.display_en;
}

const std::vector<std::string>& phrases(const CommandInfo& i, Language language) {
    return language == Language::Portuguese ? i.phrases_pt : i.phrases_en;
}

std::string normalize(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    bool pendingSpace = false;
    for (const char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isspace(u)) {
            pendingSpace = !out.empty();
            continue;
        }
        if (pendingSpace) {
            out.push_back(' ');
            pendingSpace = false;
        }
        out.push_back(u < 0x80 ? static_cast<char>(std::tolower(u)) : c);
    }
    return out;
}

namespace {

std::vector<std::string> split_words(const std::string& phrase) {
    std::vector<std::string> words;
    size_t start = 0;
    while (start < phrase.size()) {
        const size_t end = phrase.find(' ', start);
        const size_t stop = end == std::string::npos ? phrase.size() : end;
        if (stop > start) {
            words.push_back(phrase.substr(start, stop - start));
        }
        start = stop + 1;
    }
    return words;
}

std::string json_escape(const std::string& s) {
    std::string out;
    for (const char c : s) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}

}  // namespace

std::vector<std::string> active_phrases(
    Language language, const std::function<bool(const std::string&)>& knownWord) {
    std::vector<std::string> out;
    for (const CommandInfo& i : command_table()) {
        for (const std::string& phrase : phrases(i, language)) {
            bool ok = true;
            if (knownWord) {
                for (const std::string& word : split_words(phrase)) {
                    if (!knownWord(word)) {
                        ok = false;
                        break;
                    }
                }
            }
            if (ok && std::find(out.begin(), out.end(), phrase) == out.end()) {
                out.push_back(phrase);
            }
        }
    }
    return out;
}

std::string grammar_json(const std::vector<std::string>& list) {
    std::string json = "[";
    for (const std::string& phrase : list) {
        json += "\"" + json_escape(phrase) + "\", ";
    }
    json += "\"[unk]\"]";
    return json;
}

namespace {

bool is_filler(const std::string& word, Language language) {
    static const std::vector<std::string> en = {"and", "of", "the", "a"};
    static const std::vector<std::string> pt = {"de", "do", "da", "e", "o", "a"};
    const auto& list = language == Language::Portuguese ? pt : en;
    return std::find(list.begin(), list.end(), word) != list.end();
}

// True if `words` appear in `phrase` in order and contiguously.
bool contains_run(const std::vector<std::string>& phrase, const std::vector<std::string>& words) {
    if (words.empty() || words.size() > phrase.size()) {
        return false;
    }
    for (size_t start = 0; start + words.size() <= phrase.size(); ++start) {
        if (std::equal(words.begin(), words.end(), phrase.begin() + static_cast<long>(start))) {
            return true;
        }
    }
    return false;
}

}  // namespace

bool match(std::string_view text, Language language, Command& out) {
    // Vosk can return a whole phrase, a fragment of one ("duplo", "de falcão") or mark unknown
    // speech with "[unk]". Drop the markers, then:
    //   1. an exact phrase wins;
    //   2. otherwise the words must form a run inside phrases of exactly ONE command, and contain
    //      at least one meaningful word. Ambiguous fragments ("ferro": boots or ball?) are ignored
    //      rather than guessed.
    std::vector<std::string> words;
    for (const std::string& w : split_words(normalize(text))) {
        if (w != "[unk]") {
            words.push_back(w);
        }
    }
    if (words.empty()) {
        return false;
    }
    std::string joined;
    for (size_t i = 0; i < words.size(); ++i) {
        joined += (i ? " " : "") + words[i];
    }
    // Exact phrase, or the longest phrase the utterance starts with ("iron boots iron boots").
    size_t bestLength = 0;
    for (const CommandInfo& i : command_table()) {
        for (const std::string& phrase : phrases(i, language)) {
            if (joined == phrase) {
                out = i.command;
                return true;
            }
            const bool prefix = joined.size() > phrase.size() &&
                                joined.compare(0, phrase.size(), phrase) == 0 &&
                                joined[phrase.size()] == ' ';
            if (prefix && phrase.size() > bestLength) {
                bestLength = phrase.size();
                out = i.command;
            }
        }
    }
    if (bestLength > 0) {
        return true;
    }
    if (std::all_of(words.begin(), words.end(),
            [&](const std::string& w) { return is_filler(w, language); }))
    {
        return false;
    }
    bool found = false;
    Command candidate = Command::Count;
    for (const CommandInfo& i : command_table()) {
        for (const std::string& phrase : phrases(i, language)) {
            if (contains_run(split_words(phrase), words)) {
                if (found && candidate != i.command) {
                    return false;  // ambiguous
                }
                found = true;
                candidate = i.command;
                break;
            }
        }
    }
    if (found) {
        out = candidate;
    }
    return found;
}

std::string result_text(std::string_view json) {
    const size_t key = json.find("\"text\"");
    if (key == std::string_view::npos) {
        return {};
    }
    const size_t open = json.find('"', json.find(':', key) + 1);
    if (open == std::string_view::npos) {
        return {};
    }
    std::string out;
    for (size_t i = open + 1; i < json.size(); ++i) {
        if (json[i] == '\\' && i + 1 < json.size()) {
            out.push_back(json[++i]);
        } else if (json[i] == '"') {
            return out;
        } else {
            out.push_back(json[i]);
        }
    }
    return {};
}

}  // namespace voice
