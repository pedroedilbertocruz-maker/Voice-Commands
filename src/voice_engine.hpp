// Offline speech recognition: microphone capture (miniaudio) feeding Vosk on a worker thread.
//
// Vosk is loaded at runtime from the mod's native directory, so the mod still loads (without
// voice) on platforms where the library is missing. All public methods are called from the game
// thread; recognition never blocks it.
#pragma once

#include "commands.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace voice {

enum class EngineStatus {
    Off,           // not started
    NoLibrary,     // Vosk library could not be loaded
    LoadingModel,  // model loading on the worker thread
    NoModel,       // model files missing or invalid
    NoMicrophone,  // capture device could not be opened
    Listening,
};

class Engine {
public:
    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Loads the Vosk shared library from `native_dir`. Returns false if unavailable.
    bool load_library(const std::string& native_dir);
    bool library_loaded() const;

    // (Re)starts listening with the model in `model_dir`. Safe to call again to switch language.
    void start(const std::string& model_dir, Language language);
    // Stops listening and releases the model and microphone.
    void stop();
    // Stops everything and unloads the library; call from mod_shutdown.
    void shutdown();

    // Next recognized utterance, if any.
    bool poll(std::string& text);

    EngineStatus status() const { return m_status.load(); }
    // Number of grammar phrases the model accepted (others contain unknown words).
    int phrase_count() const { return m_phraseCount.load(); }

    // Audio callback entry point (miniaudio thread).
    void on_audio(const int16_t* samples, uint32_t count);

private:
    struct Api;
    struct Request {
        bool run = false;
        std::string modelDir;
        Language language = Language::English;
        uint64_t generation = 0;
    };

    void worker_main();
    void run_session(const Request& request);

    Api* m_api = nullptr;
    void* m_lib = nullptr;

    std::thread m_worker;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    Request m_request;
    bool m_quit = false;

    std::mutex m_audioMutex;
    std::vector<int16_t> m_audio;
    bool m_capturing = false;

    std::mutex m_resultMutex;
    std::deque<std::string> m_results;

    std::atomic<EngineStatus> m_status{EngineStatus::Off};
    std::atomic<int> m_phraseCount{0};
};

const char* status_text(EngineStatus status, Language language);

}  // namespace voice
