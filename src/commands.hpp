// Voice command table: which phrases (English and Portuguese) map to which item.
// Pure code with no game dependencies, so it can be tested on its own.
#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace voice {

enum class Language { English = 0, Portuguese = 1 };

enum class Command {
    IronBoots,
    Clawshot,
    DoubleClawshot,
    Boomerang,
    Bow,
    Lantern,
    Spinner,
    BallAndChain,
    DominionRod,
    Bombs,
    WaterBombs,
    Bomblings,
    Slingshot,
    FishingRod,
    HawkEye,
    HorseCall,
    Bottle,
    Count,
};

struct CommandInfo {
    Command command;
    const char* display_en;
    const char* display_pt;
    std::vector<std::string> phrases_en;
    std::vector<std::string> phrases_pt;
};

const std::vector<CommandInfo>& command_table();
const CommandInfo& info(Command command);
const char* display_name(Command command, Language language);
const std::vector<std::string>& phrases(const CommandInfo& info, Language language);

// Lowercases ASCII, collapses whitespace and trims. UTF-8 accents are left untouched (the speech
// models output lowercase accented text already).
std::string normalize(std::string_view text);

// Phrases allowed for a language. `known_word` filters out phrases containing words the speech
// model does not know (they would never be recognized); pass nullptr to keep all.
std::vector<std::string> active_phrases(Language language,
    const std::function<bool(const std::string& word)>& known_word);

// Vosk grammar JSON: ["phrase one", "phrase two", ..., "[unk]"]. "[unk]" absorbs other speech so
// unrelated words do not get forced onto a command.
std::string grammar_json(const std::vector<std::string>& phrases);

// Maps recognized text to a command. Returns false for empty text, "[unk]" or no match.
bool match(std::string_view text, Language language, Command& out);

// Extracts the "text" field from a Vosk result JSON ({"text" : "iron boots"}).
std::string result_text(std::string_view json);

}  // namespace voice
