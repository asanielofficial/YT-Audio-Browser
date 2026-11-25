cmake_minimum_required(VERSION 3.20)
project(YtAudioBrowser VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static MSVC runtime so the .clap has no VC++ redistributable dependency.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
FetchContent_Declare(clap
    GIT_REPOSITORY https://github.com/free-audio/clap.git
    GIT_TAG 1.2.10
    GIT_SHALLOW TRUE)
FetchContent_Declare(clap-helpers
    GIT_REPOSITORY https://github.com/free-audio/clap-helpers.git
    GIT_TAG a380c978f429103e08afc1d17e9efbf3e3c96ee2)
FetchContent_MakeAvailable(clap clap-helpers)

add_library(YtAudioBrowser MODULE
    src/entry.cpp
    src/plugin.cpp
    src/gui.cpp
    src/ytdlp.cpp
    src/proc.cpp
    src/http.cpp
    src/providers.cpp
    src/search.cpp)

target_link_libraries(YtAudioBrowser PRIVATE clap clap-helpers
    user32 gdi32 comctl32 ole32 shell32 uuid winhttp advapi32)

target_compile_definitions(YtAudioBrowser PRIVATE
    UNICODE _UNICODE NOMINMAX)

if(MSVC)
    target_compile_options(YtAudioBrowser PRIVATE /W4 /utf-8)
else()
    target_compile_options(YtAudioBrowser PRIVATE -Wall -Wextra)
    target_link_options(YtAudioBrowser PRIVATE -static -static-libgcc -static-libstdc++)
endif()

set_target_properties(YtAudioBrowser PROPERTIES
    PREFIX ""
    SUFFIX ".clap"
    OUTPUT_NAME "YtAudioBrowser")
