cmake_minimum_required(VERSION 3.22)

project(SUDZ VERSION 0.15.0)

# JUCE: use a local checkout if you pass -DJUCE_DIR=/path/to/JUCE, otherwise download JUCE 8.0.4.
if(DEFINED JUCE_DIR)
    add_subdirectory(${JUCE_DIR} juce)
else()
    include(FetchContent)
    FetchContent_Declare(juce
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.4
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(juce)
endif()

if(APPLE)
    set(PW_FORMATS AU VST3 Standalone)
else()
    set(PW_FORMATS VST3 Standalone)
endif()

juce_add_plugin(Sudz
    COMPANY_NAME "Sudz"
    BUNDLE_ID "com.sudz.vocal"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD TRUE          # installs the AU into ~/Library/Audio/Plug-Ins/Components
    PLUGIN_MANUFACTURER_CODE Sdzv
    PLUGIN_CODE Sudz                      # GarageBand needs: first letter upper-case, the rest lower-case
    AU_MAIN_TYPE kAudioUnitType_Effect
    NEEDS_WEB_BROWSER TRUE
    FORMATS ${PW_FORMATS}
    PRODUCT_NAME "Sudz")

juce_generate_juce_header(Sudz)

# the editor page, plus the AI clean-up engine and model (ONNX Runtime Web, MIT; DeepFilterNet3, MIT/Apache-2.0)
juce_add_binary_data(SudzUI SOURCES
    ui/index.html
    ui/ai/ort.wasm.min.js
    ui/ai/ort-wasm-simd-threaded.mjs
    ui/ai/ort-wasm-simd-threaded.wasm
    ui/ai/enc.onnx.b64.txt
    ui/ai/erb_dec.onnx.b64.txt
    ui/ai/df_dec.onnx.b64.txt
    ui/ai/legacy/ort.wasm-core.min.js
    ui/ai/legacy/ort-wasm.wasm)

target_sources(Sudz PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

if(APPLE)
    target_sources(Sudz PRIVATE Source/MacHelpers.mm)
endif()

target_compile_features(Sudz PRIVATE cxx_std_17)

target_compile_definitions(Sudz PUBLIC
    JUCE_WEB_BROWSER=1
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(Sudz
    PRIVATE
        SudzUI
        juce::juce_audio_utils
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
