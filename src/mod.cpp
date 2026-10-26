// Voice Commands for Dusklight.
//
// Say an item's name ("Iron Boots" / "Botas de Ferro") and Link uses it, even if it is not on
// X or Y. The item is placed on an item button and that button is pressed for one frame.
// Saying the same name again takes it back off the button and restores whatever was there
// before (for the Iron Boots, Link first takes them off with the normal animation).
//
// The game requires an item to stay on a button while in use: Link drops the Iron Boots and puts
// away hand items as soon as they are not on X or Y. That is why the command swaps the item onto
// a button instead of using it "invisibly".
//
// Speech recognition is offline (Vosk) with a small grammar of item names; see voice_engine.cpp.

#include "global.h"

#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_player.h"
#include "d/d_com_inf_game.h"
#include "d/d_save.h"
#include "mods/service.hpp"
#include "mods/svc/config.h"
#include "mods/svc/hook.h"
#include "mods/svc/hook.hpp"
#include "mods/svc/host.h"
#include "mods/svc/log.h"
#include "mods/svc/resource.h"
#include "mods/svc/ui.h"

#include "commands.hpp"
#include "voice_engine.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

DEFINE_MOD();
IMPORT_SERVICE(ConfigService, svc_config);
IMPORT_SERVICE(HookService, svc_hook);
IMPORT_SERVICE(HostService, svc_host);
IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(ResourceService, svc_resource);
IMPORT_SERVICE(UiService, svc_ui);

namespace {

using voice::Command;
using voice::Language;

DEFINE_HOOK(&daAlink_c::setStickData, LinkSetStickData);

constexpr int kNoButton = -1;
constexpr u8 kEmptySlot = 0xFF;
constexpr int kInventorySlots = 24;
constexpr int kPressTimeoutFrames = 90;     // unpaused frames to wait for Link to accept a press
constexpr int kBootsRemoveTimeoutFrames = 120;

ConfigVarHandle g_cvarVoice = 0;      // bool
ConfigVarHandle g_cvarLanguage = 0;   // 0 English, 1 Portuguese
ConfigVarHandle g_cvarButton = 0;     // 0 Auto, 1 X, 2 Y
ConfigVarHandle g_cvarFeedback = 0;   // bool: toast what was understood

UiWindowHandle g_window = 0;
UiElementHandle g_statusText = 0;

voice::Engine g_engine;

int64_t get_int(ConfigVarHandle h, int64_t fallback) {
    int64_t v = fallback;
    if (h == 0 || svc_config->get_int(mod_ctx, h, &v) != MOD_OK) {
        return fallback;
    }
    return v;
}

bool get_bool(ConfigVarHandle h, bool fallback) {
    bool v = fallback;
    if (h == 0 || svc_config->get_bool(mod_ctx, h, &v) != MOD_OK) {
        return fallback;
    }
    return v;
}

Language language() {
    return get_int(g_cvarLanguage, 0) == 1 ? Language::Portuguese : Language::English;
}

bool pt() {
    return language() == Language::Portuguese;
}

void toast(const std::string& title, const std::string& body, uint32_t ms = 2000) {
    UiToastDesc desc = UI_TOAST_DESC_INIT;
    desc.title_rml = title.c_str();
    desc.body_rml = body.c_str();
    desc.duration_ms = ms;
    svc_ui->push_toast(mod_ctx, &desc);
}

// =============================================================================================
// Inventory
// =============================================================================================

bool item_matches(Command command, u8 item) {
    switch (command) {
    case Command::IronBoots:
        return item == dItemNo_HVY_BOOTS_e;
    case Command::Clawshot:
        return item == dItemNo_HOOKSHOT_e;
    case Command::DoubleClawshot:
        return item == dItemNo_W_HOOKSHOT_e;
    case Command::Boomerang:
        return item == dItemNo_BOOMERANG_e;
    case Command::Bow:
        return item == dItemNo_BOW_e;
    case Command::Lantern:
        return item == dItemNo_KANTERA_e;
    case Command::Spinner:
        return item == dItemNo_SPINNER_e;
    case Command::BallAndChain:
        return item == dItemNo_IRONBALL_e;
    case Command::DominionRod:
        return item == dItemNo_COPY_ROD_e || item == dItemNo_COPY_ROD_2_e;
    case Command::Bombs:
        return item == dItemNo_NORMAL_BOMB_e;
    case Command::WaterBombs:
        return item == dItemNo_WATER_BOMB_e;
    case Command::Bomblings:
        return item == dItemNo_POKE_BOMB_e;
    case Command::Slingshot:
        return item == dItemNo_PACHINKO_e;
    case Command::FishingRod:
        return daPy_py_c::checkFishingRodItem(item) != FALSE;
    case Command::HawkEye:
        return item == dItemNo_HAWK_EYE_e;
    case Command::HorseCall:
        return item == dItemNo_HORSE_FLUTE_e;
    case Command::Bottle:
        return daPy_py_c::checkBottleItem(item) != FALSE;
    case Command::Count:
        break;
    }
    return false;
}

// Inventory slot holding the item for a command, or kEmptySlot if Link does not have it.
u8 find_inventory_slot(Command command) {
    for (int slot = 0; slot < kInventorySlots; ++slot) {
        const u8 item = dComIfGs_getItem(slot, false);
        if (item != dItemNo_NONE_e && item_matches(command, item)) {
            return static_cast<u8>(slot);
        }
    }
    return kEmptySlot;
}

// =============================================================================================
// Pressing an item button
// =============================================================================================

struct PendingPress {
    int button = kNoButton;
    int delay = 0;   // frames to wait so a fresh button assignment is picked up first
    int timeout = 0;
};
PendingPress g_press;

void press_button(int button, int delay) {
    g_press = PendingPress{button, delay, kPressTimeoutFrames};
}

bool game_accepts_input() {
    return !dComIfGp_event_runCheck() && !dComIfGp_isPauseFlag();
}

// Runs right after Link samples the controller: add our virtual press to this frame's triggers.
void on_set_stick_data_post(ModContext*, void* args, void*, void*) {
    if (g_press.button == kNoButton || g_press.delay > 0 || !game_accepts_input()) {
        return;
    }
    auto* link = mods::arg<daAlink_c*>(args, 0);
    const u8 bit = static_cast<u8>(1u << g_press.button);
    link->mItemTrigger = static_cast<decltype(link->mItemTrigger)>(link->mItemTrigger | bit);
    g_press = PendingPress{};
}

void tick_press() {
    if (g_press.button == kNoButton || !game_accepts_input()) {
        return;  // keep waiting while menus or cutscenes are up
    }
    if (g_press.delay > 0) {
        --g_press.delay;
    } else if (--g_press.timeout <= 0) {
        g_press = PendingPress{};
    }
}

// =============================================================================================
// Swapping an item onto a button and back
// =============================================================================================

struct Swap {
    bool active = false;
    Command command = Command::Count;
    int button = kNoButton;
    u8 placedSlot = kEmptySlot;
    u8 previousSlot = kEmptySlot;
    u8 previousMix = kEmptySlot;
    bool waitingForBootsOff = false;
    int waitFrames = 0;
};
Swap g_swap;

int button_with_slot(u8 slot) {
    for (int b = 0; b < 2; ++b) {
        if (dComIfGs_getSelectItemIndex(b) == slot) {
            return b;
        }
    }
    return kNoButton;
}

int choose_button() {
    switch (get_int(g_cvarButton, 0)) {
    case 1:
        return SELECT_ITEM_X;
    case 2:
        return SELECT_ITEM_Y;
    default:
        // Auto: an empty button if there is one, otherwise X.
        for (int b = 0; b < 2; ++b) {
            if (dComIfGs_getSelectItemIndex(b) == kEmptySlot) {
                return b;
            }
        }
        return SELECT_ITEM_X;
    }
}

// Puts the previous item back, unless the player has re-assigned the button in the meantime.
void restore_swap() {
    if (g_swap.active && dComIfGs_getSelectItemIndex(g_swap.button) == g_swap.placedSlot) {
        dComIfGs_setMixItemIndex(g_swap.button, g_swap.previousMix);
        dComIfGs_setSelectItemIndex(g_swap.button, g_swap.previousSlot);
    }
    g_swap = Swap{};
}

void notify(const std::string& body) {
    toast(pt() ? "Comando de voz" : "Voice command", body, 1800);
}

void execute(Command command) {
    daAlink_c* link = daAlink_getAlinkActorClass();
    if (link == nullptr || dComIfGp_getPlayer(0) == nullptr) {
        return;
    }
    const std::string name = voice::display_name(command, language());
    if (daPy_py_c::checkNowWolf()) {
        notify(pt() ? "O lobo não usa itens." : "Wolf Link can't use items.");
        return;
    }

    // Same item again: take it off the button and bring the previous item back.
    if (g_swap.active && g_swap.command == command) {
        if (command == Command::IronBoots && link->checkEquipHeavyBoots()) {
            press_button(g_swap.button, 0);  // take the boots off with the normal animation
            g_swap.waitingForBootsOff = true;
            g_swap.waitFrames = 0;
        } else {
            restore_swap();  // hand items are put away by the game once off the button
        }
        if (get_bool(g_cvarFeedback, true)) {
            notify(name + (pt() ? ": item anterior de volta" : ": previous item restored"));
        }
        return;
    }

    const u8 slot = find_inventory_slot(command);
    if (slot == kEmptySlot) {
        notify((pt() ? "Você ainda não tem: " : "You don't have: ") + name);
        return;
    }

    // A different voice item is out: put its previous item back first.
    if (g_swap.active) {
        restore_swap();
    }

    if (get_bool(g_cvarFeedback, true)) {
        notify(name);
    }

    // Already on X or Y: just press that button.
    const int existing = button_with_slot(slot);
    if (existing != kNoButton) {
        press_button(existing, 0);
        return;
    }

    const int button = choose_button();
    g_swap = Swap{};
    g_swap.active = true;
    g_swap.command = command;
    g_swap.button = button;
    g_swap.placedSlot = slot;
    g_swap.previousSlot = dComIfGs_getSelectItemIndex(button);
    g_swap.previousMix = dComIfGs_getMixItemIndex(button);
    dComIfGs_setMixItemIndex(button, kEmptySlot);
    dComIfGs_setSelectItemIndex(button, slot);
    press_button(button, 1);
}

void tick_swap() {
    if (!g_swap.active) {
        return;
    }
    // The player re-assigned the button from the item menu: forget the swap.
    if (dComIfGs_getSelectItemIndex(g_swap.button) != g_swap.placedSlot) {
        g_swap = Swap{};
        return;
    }
    if (g_swap.waitingForBootsOff && game_accepts_input()) {
        daAlink_c* link = daAlink_getAlinkActorClass();
        if (link == nullptr || !link->checkEquipHeavyBoots() ||
            ++g_swap.waitFrames > kBootsRemoveTimeoutFrames)
        {
            restore_swap();
        }
    }
}

// =============================================================================================
// Speech models: copied from the mod bundle (res/models/<lang>/) to the data folder on first use,
// because Vosk needs a real directory on disk.
// =============================================================================================

const char* lang_code(Language l) {
    return l == Language::Portuguese ? "pt" : "en";
}

std::filesystem::path utf8_path(const std::string& s) {
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

struct ModelInstall {
    enum class State { Idle, Copying, Ready, Failed } state = State::Idle;
    Language language = Language::English;
    std::string targetDir;
    std::vector<std::string> files;
    size_t next = 0;
};
ModelInstall g_install;
bool g_wantVoice = false;
Language g_activeLanguage = Language::English;

std::string read_resource(const std::string& path, bool& ok) {
    ResourceBuffer buf = RESOURCE_BUFFER_INIT;
    ok = svc_resource->load(mod_ctx, path.c_str(), &buf) == MOD_OK;
    std::string out;
    if (ok) {
        out.assign(static_cast<const char*>(buf.data), buf.size);
        svc_resource->free(mod_ctx, &buf);
    }
    return out;
}

// Begins installing (or reusing) the model for a language. Returns false if the bundle has none.
bool begin_model_install(Language l) {
    g_install = ModelInstall{};
    g_install.language = l;
    const std::string base = std::string("models/") + lang_code(l) + "/";
    bool ok = false;
    const std::string manifest = read_resource(base + "manifest.txt", ok);
    if (!ok) {
        g_install.state = ModelInstall::State::Failed;
        return false;
    }
    // manifest.txt: first line is the model name, then one relative file path per line.
    std::string modelName;
    size_t pos = 0;
    while (pos < manifest.size()) {
        size_t end = manifest.find('\n', pos);
        if (end == std::string::npos) {
            end = manifest.size();
        }
        std::string line = manifest.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty()) {
            if (modelName.empty()) {
                modelName = line;
            } else {
                g_install.files.push_back(line);
            }
        }
        pos = end + 1;
    }
    const char* dataDir = nullptr;
    if (modelName.empty() || svc_host->data_dir(mod_ctx, &dataDir) != MOD_OK || dataDir == nullptr) {
        g_install.state = ModelInstall::State::Failed;
        return false;
    }
    g_install.targetDir = std::string(dataDir) + "/models/" + modelName;
    std::error_code ec;
    if (std::filesystem::exists(utf8_path(g_install.targetDir + "/.complete"), ec)) {
        g_install.state = ModelInstall::State::Ready;
    } else {
        g_install.state = ModelInstall::State::Copying;
        svc_log->info(mod_ctx, "installing speech model to the mod data folder");
    }
    return true;
}

// Copies one model file per frame so the one-time install never freezes the game.
void tick_model_install() {
    if (g_install.state != ModelInstall::State::Copying) {
        return;
    }
    if (g_install.next >= g_install.files.size()) {
        std::ofstream(utf8_path(g_install.targetDir + "/.complete")) << "ok\n";
        g_install.state = ModelInstall::State::Ready;
        return;
    }
    const std::string& rel = g_install.files[g_install.next++];
    bool ok = false;
    const std::string data =
        read_resource(std::string("models/") + lang_code(g_install.language) + "/" + rel, ok);
    const std::filesystem::path out = utf8_path(g_install.targetDir + "/" + rel);
    std::error_code ec;
    std::filesystem::create_directories(out.parent_path(), ec);
    std::ofstream file(out, std::ios::binary);
    if (!ok || !file) {
        g_install.state = ModelInstall::State::Failed;
        svc_log->error(mod_ctx, ("could not install speech model file " + rel).c_str());
        return;
    }
    file.write(data.data(), static_cast<std::streamsize>(data.size()));
}

void update_voice() {
    const bool want = get_bool(g_cvarVoice, true) && g_engine.library_loaded();
    const Language l = language();

    if (!want) {
        if (g_wantVoice) {
            g_engine.stop();
            g_wantVoice = false;
        }
        return;
    }
    if (!g_wantVoice || l != g_activeLanguage) {
        g_wantVoice = true;
        g_activeLanguage = l;
        g_engine.stop();
        begin_model_install(l);
    }
    if (g_install.state == ModelInstall::State::Copying) {
        tick_model_install();
        if (g_install.state == ModelInstall::State::Ready) {
            g_engine.start(g_install.targetDir, l);
        }
    } else if (g_install.state == ModelInstall::State::Ready &&
               g_engine.status() == voice::EngineStatus::Off)
    {
        g_engine.start(g_install.targetDir, l);
    }

    std::string heard;
    while (g_engine.poll(heard)) {
        Command command;
        if (voice::match(heard, l, command)) {
            execute(command);
        }
    }
}

// =============================================================================================
// UI
// =============================================================================================

std::string status_line() {
    if (!get_bool(g_cvarVoice, true)) {
        return pt() ? "Status: desligado" : "Status: off";
    }
    if (!g_engine.library_loaded()) {
        return std::string("Status: ") + voice::status_text(voice::EngineStatus::NoLibrary, language());
    }
    if (g_install.state == ModelInstall::State::Copying) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), pt() ? "Status: instalando modelo de voz (%d%%)" :
                                               "Status: installing voice model (%d%%)",
            g_install.files.empty() ? 0 : static_cast<int>(100 * g_install.next / g_install.files.size()));
        return buf;
    }
    if (g_install.state == ModelInstall::State::Failed) {
        return std::string("Status: ") + voice::status_text(voice::EngineStatus::NoModel, language());
    }
    std::string s = std::string("Status: ") + voice::status_text(g_engine.status(), language());
    if (g_engine.status() == voice::EngineStatus::Listening) {
        s += pt() ? " (português)" : " (English)";
    }
    return s;
}

void add_control(UiElementHandle pane, const UiControlDesc& d) {
    svc_ui->pane_add_control(mod_ctx, pane, &d, nullptr);
}

void add_toggle(UiElementHandle pane, const char* label, ConfigVarHandle cvar, const char* help) {
    UiControlDesc c = UI_CONTROL_DESC_INIT;
    c.kind = UI_CONTROL_TOGGLE;
    c.label = label;
    c.help_rml = help;
    c.binding = UI_BINDING_CONFIG_VAR;
    c.config_var = cvar;
    add_control(pane, c);
}

void add_select(UiElementHandle pane, const char* label, ConfigVarHandle cvar,
    const char* const* options, size_t count, const char* help) {
    UiControlDesc c = UI_CONTROL_DESC_INIT;
    c.kind = UI_CONTROL_SELECT;
    c.label = label;
    c.help_rml = help;
    c.binding = UI_BINDING_CONFIG_VAR;
    c.config_var = cvar;
    c.options = options;
    c.option_count = count;
    add_control(pane, c);
}

constexpr const char* kLanguages[] = {"English", "Português"};
constexpr const char* kButtons[] = {"Auto", "X", "Y"};

void on_test_pressed(ModContext*, void* userData) {
    execute(static_cast<Command>(reinterpret_cast<uintptr_t>(userData)));
}

ModResult build_settings_tab(
    ModContext*, UiWindowHandle, UiElementHandle left, UiElementHandle right, void*, ModError*) {
    svc_ui->pane_add_section(mod_ctx, left, "Voice");
    add_toggle(left, "Voice Commands", g_cvarVoice, "Listen to the microphone for item names.");
    add_select(left, "Language", g_cvarLanguage, kLanguages, 2,
        "Language you will speak. Each language has its own offline speech model.");
    add_select(left, "Item Button", g_cvarButton, kButtons, 3,
        "Button that receives items called by voice. Auto uses an empty button if there is one, "
        "otherwise X. Saying the item again puts the previous item back.");
    add_toggle(left, "Show What Was Understood", g_cvarFeedback,
        "Shows a short notification with the item that was recognized.");

    svc_ui->pane_add_section(mod_ctx, left, "Test Without Microphone");
    for (const voice::CommandInfo& info : voice::command_table()) {
        UiControlDesc c = UI_CONTROL_DESC_INIT;
        c.kind = UI_CONTROL_BUTTON;
        c.label = voice::display_name(info.command, language());
        c.on_pressed = on_test_pressed;
        c.user_data = reinterpret_cast<void*>(static_cast<uintptr_t>(info.command));
        add_control(left, c);
    }

    svc_ui->pane_add_section(mod_ctx, right, pt() ? "O que falar" : "What to say");
    for (const voice::CommandInfo& info : voice::command_table()) {
        std::string line = std::string(voice::display_name(info.command, language())) + ":  ";
        const auto& list = voice::phrases(info, language());
        for (size_t i = 0; i < list.size(); ++i) {
            line += (i ? ", " : "") + ("\"" + list[i] + "\"");
        }
        svc_ui->pane_add_text(mod_ctx, right, line.c_str(), nullptr);
    }
    return MOD_OK;
}

void on_window_closed(ModContext*, UiWindowHandle, void*) {
    g_window = 0;
}

void on_open_settings(ModContext*, void*) {
    if (g_window != 0) {
        return;
    }
    UiTabDesc tabs[1] = {UI_TAB_DESC_INIT};
    tabs[0].title = "Voice Commands";
    tabs[0].build = build_settings_tab;
    UiWindowDesc desc = UI_WINDOW_DESC_INIT;
    desc.tabs = tabs;
    desc.tab_count = 1;
    desc.on_closed = on_window_closed;
    svc_ui->window_push(mod_ctx, &desc, &g_window);
}

ModResult build_panel(ModContext*, UiElementHandle panel, void*, ModError*) {
    add_toggle(panel, "Voice Commands", g_cvarVoice, nullptr);
    add_select(panel, "Language", g_cvarLanguage, kLanguages, 2, nullptr);
    g_statusText = 0;
    svc_ui->pane_add_text(mod_ctx, panel, status_line().c_str(), &g_statusText);
    UiControlDesc c = UI_CONTROL_DESC_INIT;
    c.kind = UI_CONTROL_BUTTON;
    c.label = "Settings and Commands";
    c.on_pressed = on_open_settings;
    add_control(panel, c);
    return MOD_OK;
}

ModResult update_panel(ModContext*, void*, ModError*) {
    if (g_statusText != 0) {
        svc_ui->elem_set_text(mod_ctx, g_statusText, status_line().c_str());
    }
    return MOD_OK;
}

ModResult register_var(const char* name, ConfigVarType type, int64_t def, ConfigVarHandle& out,
    ModError* error) {
    ConfigVarDesc d = CONFIG_VAR_DESC_INIT;
    d.name = name;
    d.type = type;
    if (type == CONFIG_VAR_BOOL) {
        d.default_bool = def != 0;
    } else {
        d.default_int = def;
    }
    if (svc_config->register_var(mod_ctx, &d, &out) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register voice command option");
    }
    return MOD_OK;
}

}  // namespace

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* error) {
    ModResult r = MOD_OK;
    if ((r = register_var("voiceEnabled", CONFIG_VAR_BOOL, 1, g_cvarVoice, error)) != MOD_OK ||
        (r = register_var("language", CONFIG_VAR_INT, 0, g_cvarLanguage, error)) != MOD_OK ||
        (r = register_var("itemButton", CONFIG_VAR_INT, 0, g_cvarButton, error)) != MOD_OK ||
        (r = register_var("showFeedback", CONFIG_VAR_BOOL, 1, g_cvarFeedback, error)) != MOD_OK)
    {
        return r;
    }

    if (mods::hook::add_post<LinkSetStickData>(on_set_stick_data_post) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to hook Link's input");
    }

    const char* nativeDir = svc_host->native_dir(mod_ctx);
    if (nativeDir == nullptr || !g_engine.load_library(nativeDir)) {
        svc_log->warn(mod_ctx,
            "speech library unavailable; voice is off (the test buttons in settings still work)");
    }

    UiModsPanelDesc panel = UI_MODS_PANEL_DESC_INIT;
    panel.build = build_panel;
    panel.update = update_panel;
    svc_ui->register_mods_panel(mod_ctx, &panel);
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    update_voice();
    tick_press();
    tick_swap();
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    g_engine.shutdown();
    if (g_swap.active) {
        restore_swap();
    }
    g_press = PendingPress{};
    g_install = ModelInstall{};
    g_wantVoice = false;
    g_window = 0;
    g_statusText = 0;
    g_cvarVoice = g_cvarLanguage = g_cvarButton = g_cvarFeedback = 0;
    return MOD_OK;
}
}
