cmake_minimum_required(VERSION 3.20)
project(YtAudioBrowser VERSION 1.0.0 LANGUAGES CXX)

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

set(YTB_COMMON src/entry.cpp src/plugin.cpp src/providers.cpp src/search.cpp)

if(WIN32)
    add_library(YtAudioBrowser MODULE ${YTB_COMMON}
        src/gui.cpp src/ytdlp.cpp src/proc.cpp src/http.cpp)

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
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0 gdk-x11-3.0)
    pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
    find_package(Threads REQUIRED)

    add_library(YtAudioBrowser MODULE ${YTB_COMMON}
        src/linux/gui_gtk.cpp src/linux/ytdlp_linux.cpp
        src/linux/proc_linux.cpp src/linux/http_linux.cpp)

    target_link_libraries(YtAudioBrowser PRIVATE clap clap-helpers
        PkgConfig::GTK3 PkgConfig::CURL Threads::Threads ${CMAKE_DL_LIBS})

    target_compile_options(YtAudioBrowser PRIVATE -Wall -Wextra -fvisibility=hidden)
    # Export only clap_entry; keep our C++ runtime private so the host's does not clash.
    target_link_options(YtAudioBrowser PRIVATE -static-libstdc++ -static-libgcc
        -Wl,--exclude-libs,ALL -Wl,--no-undefined
        -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/linux/exports.map)
endif()

set_target_properties(YtAudioBrowser PROPERTIES
    PREFIX ""
    SUFFIX ".clap"
    OUTPUT_NAME "YtAudioBrowser")
