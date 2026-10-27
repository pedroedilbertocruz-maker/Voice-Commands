#include "voice_engine.hpp"

#include "miniaudio.h"

#include <algorithm>
#include <chrono>
#include <type_traits>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace voice {
namespace {

constexpr float kSampleRate = 16000.0f;

#if defined(_WIN32)
constexpr const char* kLibraryName = "libvosk.dll";
#elif defined(__APPLE__)
constexpr const char* kLibraryName = "libvosk.dylib";
#else
constexpr const char* kLibraryName = "libvosk.so";
#endif

void* open_library(const std::string& path) {
#if defined(_WIN32)
    const int size = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring wide(static_cast<size_t>(size > 0 ? size : 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wide.data(), size);
    // Resolve libvosk's own dependencies (libstdc++, libgcc, winpthread) from the same folder.
    return LoadLibraryExW(wide.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
#else
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void* find_symbol(void* lib, const char* name) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib), name));
#else
    return dlsym(lib, name);
#endif
}

void close_library(void* lib) {
    if (lib == nullptr) {
        return;
    }
#if defined(_WIN32)
    FreeLibrary(static_cast<HMODULE>(lib));
#else
    dlclose(lib);
#endif
}

void on_capture(ma_device* device, void*, const void* input, ma_uint32 frameCount) {
    auto* engine = static_cast<Engine*>(device->pUserData);
    if (engine != nullptr && input != nullptr) {
        engine->on_audio(static_cast<const int16_t*>(input), frameCount);
    }
}

}  // namespace

// Subset of the Vosk C API (vosk_api.h), resolved at runtime.
struct Engine::Api {
    struct VoskModel;
    struct VoskRecognizer;
    VoskModel* (*model_new)(const char*);
    void (*model_free)(VoskModel*);
    int (*model_find_word)(VoskModel*, const char*);
    VoskRecognizer* (*recognizer_new_grm)(VoskModel*, float, const char*);
    int (*recognizer_accept_waveform_s)(VoskRecognizer*, const short*, int);
    const char* (*recognizer_result)(VoskRecognizer*);
    void (*recognizer_free)(VoskRecognizer*);
    void (*set_log_level)(int);
};

Engine::Engine() = default;

Engine::~Engine() {
    shutdown();
}

bool Engine::load_library(const std::string& nativeDir) {
    if (m_api != nullptr) {
        return true;
    }
    std::string path = nativeDir;
    if (!path.empty() && path.back() != '/' && path.back() != '\\') {
        path.push_back('/');
    }
    path += kLibraryName;
    void* lib = open_library(path);
    if (lib == nullptr) {
        m_status = EngineStatus::NoLibrary;
        return false;
    }

    auto* api = new Api{};
    bool ok = true;
    auto bind = [&](auto& fn, const char* name) {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(find_symbol(lib, name));
        ok = ok && fn != nullptr;
    };
    bind(api->model_new, "vosk_model_new");
    bind(api->model_free, "vosk_model_free");
    bind(api->model_find_word, "vosk_model_find_word");
    bind(api->recognizer_new_grm, "vosk_recognizer_new_grm");
    bind(api->recognizer_accept_waveform_s, "vosk_recognizer_accept_waveform_s");
    bind(api->recognizer_result, "vosk_recognizer_result");
    bind(api->recognizer_free, "vosk_recognizer_free");
    bind(api->set_log_level, "vosk_set_log_level");
    if (!ok) {
        delete api;
        close_library(lib);
        m_status = EngineStatus::NoLibrary;
        return false;
    }
    api->set_log_level(-1);  // keep Kaldi's logging out of the game console
    m_api = api;
    m_lib = lib;
    m_quit = false;
    m_worker = std::thread([this] { worker_main(); });
    return true;
}

bool Engine::library_loaded() const {
    return m_api != nullptr;
}

void Engine::start(const std::string& modelDir, Language language, const std::string& device) {
    if (m_api == nullptr) {
        return;
    }
    std::lock_guard lock(m_mutex);
    m_request.run = true;
    m_request.modelDir = modelDir;
    m_request.language = language;
    m_request.device = device;
    ++m_request.generation;
    m_status = EngineStatus::LoadingModel;
    m_cv.notify_all();
}

void Engine::stop() {
    if (m_api == nullptr) {
        return;
    }
    std::lock_guard lock(m_mutex);
    m_request.run = false;
    ++m_request.generation;
    m_cv.notify_all();
}

void Engine::shutdown() {
    if (m_worker.joinable()) {
        {
            std::lock_guard lock(m_mutex);
            m_quit = true;
            m_request.run = false;
            ++m_request.generation;
        }
        m_cv.notify_all();
        m_worker.join();
    }
    delete m_api;
    m_api = nullptr;
    close_library(m_lib);
    m_lib = nullptr;
    m_status = EngineStatus::Off;
    std::lock_guard lock(m_resultMutex);
    m_results.clear();
}

bool Engine::poll(std::string& text) {
    std::lock_guard lock(m_resultMutex);
    if (m_results.empty()) {
        return false;
    }
    text = std::move(m_results.front());
    m_results.pop_front();
    return true;
}

void Engine::on_audio(const int16_t* samples, uint32_t count) {
    int peak = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const int v = samples[i] < 0 ? -static_cast<int>(samples[i]) : samples[i];
        peak = v > peak ? v : peak;
    }
    m_level = static_cast<float>(peak) / 32768.0f;
    {
        std::lock_guard lock(m_audioMutex);
        if (!m_capturing) {
            return;
        }
        // Cap the backlog at ~2 s so a stalled recognizer never grows memory without bound.
        constexpr size_t kMaxBacklog = 32000;
        if (m_audio.size() + count > kMaxBacklog) {
            m_audio.clear();
        }
        m_audio.insert(m_audio.end(), samples, samples + count);
    }
    m_cv.notify_all();
}

void Engine::worker_main() {
    uint64_t handled = 0;
    while (true) {
        Request request;
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock, [&] { return m_quit || m_request.generation != handled; });
            if (m_quit) {
                lock.unlock();
                release_model();
                return;
            }
            request = m_request;
            handled = request.generation;
        }
        if (request.run) {
            run_session(request);
        } else {
            release_model();
            m_status = EngineStatus::Off;
        }
        m_level = 0.0f;
    }
}

void Engine::release_model() {
    if (m_model != nullptr) {
        m_api->model_free(static_cast<Api::VoskModel*>(m_model));
        m_model = nullptr;
    }
    m_modelDir.clear();
}

std::vector<std::string> Engine::list_microphones() {
    std::vector<std::string> names;
    ma_context context;
    if (ma_context_init(nullptr, 0, nullptr, &context) != MA_SUCCESS) {
        return names;
    }
    ma_device_info* capture = nullptr;
    ma_uint32 captureCount = 0;
    if (ma_context_get_devices(&context, nullptr, nullptr, &capture, &captureCount) == MA_SUCCESS) {
        for (ma_uint32 i = 0; i < captureCount; ++i) {
            std::string name = capture[i].name;
            if (!name.empty() && std::find(names.begin(), names.end(), name) == names.end()) {
                names.push_back(std::move(name));
            }
        }
    }
    ma_context_uninit(&context);
    return names;
}

void Engine::run_session(const Request& request) {
    if (m_model == nullptr || m_modelDir != request.modelDir) {
        m_status = EngineStatus::LoadingModel;
        release_model();
        m_model = m_api->model_new(request.modelDir.c_str());
        if (m_model == nullptr) {
            m_status = EngineStatus::NoModel;
            return;
        }
        m_modelDir = request.modelDir;
    }
    Api::VoskModel* model = static_cast<Api::VoskModel*>(m_model);

    const std::vector<std::string> phrases = active_phrases(request.language,
        [&](const std::string& word) { return m_api->model_find_word(model, word.c_str()) >= 0; });
    m_phraseCount = static_cast<int>(phrases.size());
    const std::string grammar = grammar_json(phrases);
    Api::VoskRecognizer* recognizer = m_api->recognizer_new_grm(model, kSampleRate, grammar.c_str());
    if (recognizer == nullptr) {
        release_model();
        m_status = EngineStatus::NoModel;
        return;
    }

    // Find the requested microphone by name; fall back to the system default if it is gone.
    ma_context context;
    const bool contextOk = ma_context_init(nullptr, 0, nullptr, &context) == MA_SUCCESS;
    ma_device_id deviceId{};
    bool haveDeviceId = false;
    if (contextOk && !request.device.empty()) {
        ma_device_info* capture = nullptr;
        ma_uint32 captureCount = 0;
        if (ma_context_get_devices(&context, nullptr, nullptr, &capture, &captureCount) == MA_SUCCESS) {
            for (ma_uint32 i = 0; i < captureCount; ++i) {
                if (request.device == capture[i].name) {
                    deviceId = capture[i].id;
                    haveDeviceId = true;
                    break;
                }
            }
        }
    }
    m_fallback = !request.device.empty() && !haveDeviceId;

    ma_device_config config = ma_device_config_init(ma_device_type_capture);
    config.capture.pDeviceID = haveDeviceId ? &deviceId : nullptr;
    config.capture.format = ma_format_s16;
    config.capture.channels = 1;
    config.sampleRate = static_cast<ma_uint32>(kSampleRate);
    config.dataCallback = on_capture;
    config.pUserData = this;
    ma_device device;
    const bool deviceOk =
        ma_device_init(contextOk ? &context : nullptr, &config, &device) == MA_SUCCESS;
    if (deviceOk) {
        {
            std::lock_guard lock(m_audioMutex);
            m_audio.clear();
            m_capturing = true;
        }
        if (ma_device_start(&device) != MA_SUCCESS) {
            std::lock_guard lock(m_audioMutex);
            m_capturing = false;
        }
    }
    bool capturing = false;
    {
        std::lock_guard lock(m_audioMutex);
        capturing = m_capturing;
    }
    if (!capturing) {
        if (deviceOk) {
            ma_device_uninit(&device);
        }
        if (contextOk) {
            ma_context_uninit(&context);
        }
        m_api->recognizer_free(recognizer);
        m_status = EngineStatus::NoMicrophone;
        return;
    }

    m_status = EngineStatus::Listening;
    std::vector<int16_t> chunk;
    while (true) {
        {
            std::unique_lock lock(m_mutex);
            if (m_quit || m_request.generation != request.generation) {
                break;
            }
        }
        {
            std::unique_lock lock(m_audioMutex);
            chunk.swap(m_audio);
            m_audio.clear();
        }
        if (chunk.empty()) {
            std::unique_lock lock(m_mutex);
            m_cv.wait_for(lock, std::chrono::milliseconds(50), [&] {
                return m_quit || m_request.generation != request.generation;
            });
            continue;
        }
        if (m_api->recognizer_accept_waveform_s(
                recognizer, chunk.data(), static_cast<int>(chunk.size())) != 0)
        {
            std::string text = result_text(m_api->recognizer_result(recognizer));
            if (!text.empty()) {
                std::lock_guard lock(m_resultMutex);
                m_results.push_back(std::move(text));
            }
        }
        chunk.clear();
    }

    {
        std::lock_guard lock(m_audioMutex);
        m_capturing = false;
        m_audio.clear();
    }
    ma_device_uninit(&device);
    if (contextOk) {
        ma_context_uninit(&context);
    }
    m_api->recognizer_free(recognizer);
    m_status = EngineStatus::Off;
}

const char* status_text(EngineStatus status, Language language) {
    const bool pt = language == Language::Portuguese;
    switch (status) {
    case EngineStatus::Off:
        return pt ? "Desligado" : "Off";
    case EngineStatus::NoLibrary:
        return pt ? "Biblioteca de voz indisponível nesta plataforma" :
                    "Voice library not available on this platform";
    case EngineStatus::LoadingModel:
        return pt ? "Carregando modelo de voz..." : "Loading voice model...";
    case EngineStatus::NoModel:
        return pt ? "Modelo de voz não encontrado" : "Voice model not found";
    case EngineStatus::NoMicrophone:
        return pt ? "Microfone não encontrado" : "No microphone found";
    case EngineStatus::Listening:
        return pt ? "Ouvindo" : "Listening";
    }
    return "";
}

}  // namespace voice
