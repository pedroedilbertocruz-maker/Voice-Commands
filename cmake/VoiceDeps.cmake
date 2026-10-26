# Third-party pieces for voice recognition, pinned and verified:
#   - miniaudio (microphone capture, public domain / MIT-0), header only
#   - Vosk 0.3.45 (offline speech recognition, Apache-2.0), prebuilt per platform
#   - Vosk small speech models for English and Portuguese (Apache-2.0)
#
# Outputs: miniaudio_SOURCE_DIR, VOSK_RUNTIME_LIBRARIES, VOSK_LIBRARY_DIR, VOICE_RES_DIR

FetchContent_Declare(miniaudio
        GIT_REPOSITORY https://github.com/mackron/miniaudio.git
        GIT_TAG 350784a9467a79d0fa65802132668e5afbcf3777  # 0.11.22
        GIT_SHALLOW FALSE)
FetchContent_GetProperties(miniaudio)
if (NOT miniaudio_POPULATED)
    FetchContent_Populate(miniaudio)
endif ()

set(_vosk_version "0.3.45")
set(_vosk_base "https://github.com/alphacep/vosk-api/releases/download/v${_vosk_version}")
set(VOSK_RUNTIME_LIBRARIES "")
set(VOSK_LIBRARY_DIR "")

string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _proc)
if (CMAKE_OSX_ARCHITECTURES)
    string(TOLOWER "${CMAKE_OSX_ARCHITECTURES}" _proc)
endif ()
set(_vosk_package "")
if (WIN32 AND _proc MATCHES "amd64|x86_64|x64")
    set(_vosk_package "vosk-win64-${_vosk_version}")
    set(_vosk_hash "f1dcc9cca460630f81ea8f71794f69c80bed6556d2a4e6237b5785e1d2dff34b")
    set(_vosk_files libvosk.dll libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND _proc MATCHES "x86_64|amd64")
    set(_vosk_package "vosk-linux-x86_64-${_vosk_version}")
    set(_vosk_hash "bbdc8ed85c43979f6443142889770ea95cbfbc56cffb5c5dcd73afa875c5fbb2")
    set(_vosk_files libvosk.so)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND _proc MATCHES "aarch64|arm64")
    set(_vosk_package "vosk-linux-aarch64-${_vosk_version}")
    set(_vosk_hash "45e95d37755deb07568e79497d7feba8c03aee5a9e071df29961aa023fd94541")
    set(_vosk_files libvosk.so)
endif ()

if (_vosk_package)
    set(_zip "${CMAKE_BINARY_DIR}/_deps/${_vosk_package}.zip")
    set(VOSK_LIBRARY_DIR "${CMAKE_BINARY_DIR}/_deps/${_vosk_package}")
    if (NOT EXISTS "${VOSK_LIBRARY_DIR}")
        message(STATUS "Voice: downloading ${_vosk_package}")
        file(DOWNLOAD "${_vosk_base}/${_vosk_package}.zip" "${_zip}"
                EXPECTED_HASH SHA256=${_vosk_hash} TLS_VERIFY ON)
        file(ARCHIVE_EXTRACT INPUT "${_zip}" DESTINATION "${CMAKE_BINARY_DIR}/_deps")
    endif ()
    foreach (_f IN LISTS _vosk_files)
        list(APPEND VOSK_RUNTIME_LIBRARIES "${VOSK_LIBRARY_DIR}/${_f}")
    endforeach ()
else ()
    message(WARNING "Voice: no Vosk build for this platform; the mod will load without voice")
endif ()

# Speech models are copied into the mod's res/ with a manifest (model name + file list), which
# the mod uses to install them into its data folder on first use.
set(VOICE_RES_DIR "${CMAKE_BINARY_DIR}/voice_res")
file(MAKE_DIRECTORY "${VOICE_RES_DIR}")
if (VOICE_BUNDLE_MODELS)
    set(_models
            "en|vosk-model-small-en-us-0.15"
            "pt|vosk-model-small-pt-0.3")
    foreach (_entry IN LISTS _models)
        string(REPLACE "|" ";" _parts "${_entry}")
        list(GET _parts 0 _lang)
        list(GET _parts 1 _name)
        set(_dest "${VOICE_RES_DIR}/models/${_lang}")
        if (NOT EXISTS "${_dest}/manifest.txt")
            set(_mzip "${CMAKE_BINARY_DIR}/_deps/${_name}.zip")
            if (NOT EXISTS "${_mzip}")
                message(STATUS "Voice: downloading speech model ${_name}")
                file(DOWNLOAD "https://alphacephei.com/vosk/models/${_name}.zip" "${_mzip}"
                        STATUS _st TLS_VERIFY ON)
                list(GET _st 0 _code)
                if (NOT _code EQUAL 0)
                    file(REMOVE "${_mzip}")
                    message(FATAL_ERROR "Voice: could not download ${_name}: ${_st}. "
                            "Configure with -DVOICE_BUNDLE_MODELS=OFF to build without models.")
                endif ()
            endif ()
            file(ARCHIVE_EXTRACT INPUT "${_mzip}" DESTINATION "${CMAKE_BINARY_DIR}/_deps/models")
            file(REMOVE_RECURSE "${_dest}")
            file(COPY "${CMAKE_BINARY_DIR}/_deps/models/${_name}/" DESTINATION "${_dest}")
            file(GLOB_RECURSE _files RELATIVE "${_dest}" "${_dest}/*")
            list(SORT _files)
            list(JOIN _files "\n" _list)
            file(WRITE "${_dest}/manifest.txt" "${_name}\n${_list}\n")
        endif ()
    endforeach ()
else ()
    file(WRITE "${VOICE_RES_DIR}/.keep" "")
endif ()
