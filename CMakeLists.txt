cmake_minimum_required(VERSION 3.22)
project(SampliteExplorer VERSION 1.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.4
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

# Queen Mary DSP library (GPL-2.0-or-later): key detection and beat tracking.
# It has no CMake build of its own, so only the parts we use are compiled here.
FetchContent_Declare(qmdsp
    GIT_REPOSITORY https://github.com/c4dm/qm-dsp.git
    GIT_TAG e34a3cc188332ed7c33cd9257ef164de5b587191)
FetchContent_MakeAvailable(qmdsp)

file(GLOB QM_CHROMA CONFIGURE_DEPENDS ${qmdsp_SOURCE_DIR}/dsp/chromagram/*.cpp)
add_library(qmdsp STATIC
    ${QM_CHROMA}
    ${qmdsp_SOURCE_DIR}/base/Pitch.cpp
    ${qmdsp_SOURCE_DIR}/dsp/keydetection/GetKeyMode.cpp
    ${qmdsp_SOURCE_DIR}/dsp/onsets/DetectionFunction.cpp
    ${qmdsp_SOURCE_DIR}/dsp/phasevocoder/PhaseVocoder.cpp
    ${qmdsp_SOURCE_DIR}/dsp/rateconversion/Decimator.cpp
    ${qmdsp_SOURCE_DIR}/dsp/tempotracking/TempoTrackV2.cpp
    ${qmdsp_SOURCE_DIR}/dsp/transforms/FFT.cpp
    ${qmdsp_SOURCE_DIR}/maths/MathUtilities.cpp
    ${qmdsp_SOURCE_DIR}/ext/kissfft/kiss_fft.c
    ${qmdsp_SOURCE_DIR}/ext/kissfft/tools/kiss_fftr.c)
target_include_directories(qmdsp
    SYSTEM PUBLIC ${qmdsp_SOURCE_DIR}
    PRIVATE ${qmdsp_SOURCE_DIR}/ext/kissfft ${qmdsp_SOURCE_DIR}/ext/kissfft/tools)
target_compile_definitions(qmdsp PUBLIC _USE_MATH_DEFINES PRIVATE kiss_fft_scalar=double NOMINMAX)
target_compile_options(qmdsp PRIVATE $<IF:$<C_COMPILER_ID:MSVC>,/w,-w>) # third-party warnings

set(COMMON_DEFS JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_USE_MP3AUDIOFORMAT=1)

juce_add_gui_app(SampliteExplorer
    PRODUCT_NAME "Samplite Explorer"
    COMPANY_NAME "Samplite"
    BUNDLE_ID com.samplite.explorer)
target_sources(SampliteExplorer PRIVATE Source/Main.cpp Source/Detect.cpp Source/FileActions.cpp)
target_compile_definitions(SampliteExplorer PRIVATE ${COMMON_DEFS} APP_VERSION="${PROJECT_VERSION}")
target_link_libraries(SampliteExplorer
    PRIVATE qmdsp juce::juce_audio_utils juce::juce_dsp
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

juce_add_console_app(ExplorerTests)
target_sources(ExplorerTests PRIVATE Tests/Tests.cpp Source/Detect.cpp Source/FileActions.cpp)
target_compile_definitions(ExplorerTests PRIVATE ${COMMON_DEFS})
target_link_libraries(ExplorerTests
    PRIVATE qmdsp juce::juce_audio_formats juce::juce_dsp
    PUBLIC juce::juce_recommended_config_flags)

enable_testing()
add_test(NAME ExplorerTests COMMAND ExplorerTests)

# Files the installer scripts pick up.
file(WRITE ${CMAKE_BINARY_DIR}/version.txt "${PROJECT_VERSION}")
configure_file(THIRD-PARTY.md ${CMAKE_BINARY_DIR}/licenses/THIRD-PARTY.md COPYONLY)
configure_file(${qmdsp_SOURCE_DIR}/COPYING ${CMAKE_BINARY_DIR}/licenses/QM-DSP-GPL.txt COPYONLY)
configure_file(${qmdsp_SOURCE_DIR}/ext/kissfft/COPYING ${CMAKE_BINARY_DIR}/licenses/KissFFT.txt COPYONLY)
configure_file(${juce_SOURCE_DIR}/LICENSE.md ${CMAKE_BINARY_DIR}/licenses/JUCE.md COPYONLY)
