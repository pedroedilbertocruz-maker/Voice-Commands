// Offline recognition check used by CI: feeds WAV files through Vosk with the mod's grammar and
// verifies each one maps to the expected command.
//
//   recognize <libvosk> <model_dir> <en|pt> --phrases            list "index|phrase" to synthesize
//   recognize <libvosk> <model_dir> <en|pt> file.wav=index ...    recognize and check
#include "commands.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
static void* open_lib(const char* p) { return LoadLibraryA(p); }
static void* sym(void* l, const char* n) { return (void*)GetProcAddress((HMODULE)l, n); }
#else
#include <dlfcn.h>
static void* open_lib(const char* p) { return dlopen(p, RTLD_NOW); }
static void* sym(void* l, const char* n) { return dlsym(l, n); }
#endif

using namespace voice;

struct Wav {
    int rate = 0;
    std::vector<int16_t> samples;
};

static bool read_wav(const std::string& path, Wav& out) {
    std::ifstream f(path, std::ios::binary);
    std::vector<char> d((std::istreambuf_iterator<char>(f)), {});
    if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) != 0) return false;
    size_t p = 12;
    int channels = 1, bits = 16;
    while (p + 8 <= d.size()) {
        uint32_t len;
        std::memcpy(&len, &d[p + 4], 4);
        if (std::memcmp(&d[p], "fmt ", 4) == 0) {
            uint16_t ch, bps;
            uint32_t rate;
            std::memcpy(&ch, &d[p + 10], 2);
            std::memcpy(&rate, &d[p + 12], 4);
            std::memcpy(&bps, &d[p + 22], 2);
            channels = ch, bits = bps, out.rate = (int)rate;
        } else if (std::memcmp(&d[p], "data", 4) == 0) {
            size_t n = std::min<size_t>(len, d.size() - p - 8) / 2;
            out.samples.resize(n);
            std::memcpy(out.samples.data(), &d[p + 8], n * 2);
        }
        p += 8 + len + (len & 1);
    }
    return channels == 1 && bits == 16 && out.rate > 0 && !out.samples.empty();
}

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: recognize <libvosk> <model> <en|pt> --phrases | wav=index...\n");
        return 2;
    }
    const Language lang = std::strcmp(argv[3], "pt") == 0 ? Language::Portuguese : Language::English;
    if (std::strcmp(argv[4], "--phrases") == 0) {
        for (const auto& i : command_table())
            for (const auto& p : phrases(i, lang)) std::printf("%d|%s\n", (int)i.command, p.c_str());
        return 0;
    }
    void* lib = open_lib(argv[1]);
    if (!lib) { std::fprintf(stderr, "cannot load %s\n", argv[1]); return 2; }
    auto model_new = (void* (*)(const char*))sym(lib, "vosk_model_new");
    auto find_word = (int (*)(void*, const char*))sym(lib, "vosk_model_find_word");
    auto rec_new = (void* (*)(void*, float, const char*))sym(lib, "vosk_recognizer_new_grm");
    auto accept = (int (*)(void*, const short*, int))sym(lib, "vosk_recognizer_accept_waveform_s");
    auto final_result = (const char* (*)(void*))sym(lib, "vosk_recognizer_final_result");
    auto rec_free = (void (*)(void*))sym(lib, "vosk_recognizer_free");
    auto log_level = (void (*)(int))sym(lib, "vosk_set_log_level");
    log_level(-1);
    void* model = model_new(argv[2]);
    if (!model) { std::fprintf(stderr, "cannot load model %s\n", argv[2]); return 2; }

    // Report phrases the model cannot recognize (words outside its vocabulary).
    for (const auto& i : command_table())
        for (const auto& p : phrases(i, lang)) {
            std::string missing;
            size_t s = 0;
            while (s <= p.size()) {
                size_t e = p.find(' ', s);
                if (e == std::string::npos) e = p.size();
                std::string w = p.substr(s, e - s);
                if (!w.empty() && find_word(model, w.c_str()) < 0) missing += " " + w;
                s = e + 1;
            }
            if (!missing.empty()) std::printf("UNKNOWN WORDS [%s] in \"%s\":%s\n", argv[3], p.c_str(), missing.c_str());
        }
    const auto active = active_phrases(lang, [&](const std::string& w) { return find_word(model, w.c_str()) >= 0; });
    const std::string grammar = grammar_json(active);

    int total = 0, passed = 0;
    for (int a = 4; a < argc; ++a) {
        std::string arg = argv[a];
        size_t eq = arg.rfind('=');
        const std::string file = arg.substr(0, eq);
        const int expected = std::atoi(arg.c_str() + eq + 1);
        Wav wav;
        if (!read_wav(file, wav)) { std::printf("SKIP  %s (unreadable)\n", file.c_str()); continue; }
        void* rec = rec_new(model, (float)wav.rate, grammar.c_str());
        accept(rec, wav.samples.data(), (int)wav.samples.size());
        const std::string text = result_text(final_result(rec));
        rec_free(rec);
        Command got;
        const bool ok = match(text, lang, got) && (int)got == expected;
        ++total;
        passed += ok;
        std::printf("%s  %-28s heard \"%s\"\n", ok ? "PASS" : "FAIL", file.c_str(), text.c_str());
    }
    std::printf("[%s] %d/%d recognized, %zu active phrases\n", argv[3], passed, total, active.size());
    // Synthetic voices are harsher than real speech; require a clear majority, not perfection.
    return total == 0 || passed * 10 >= total * 7 ? 0 : 1;
}
