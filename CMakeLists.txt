cmake_minimum_required(VERSION 3.16)
project(torrent-dl CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── FTXUI (retro BBS TUI) ────────────────────────────────────────────────────
include(FetchContent)
FetchContent_Declare(
    ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
    GIT_TAG        v5.0.0
    GIT_SHALLOW    TRUE
)
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(ftxui)

# ── nlohmann/json (header-only JSON parsing) ─────────────────────────────────
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install    OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nlohmann_json)

# ── Find libtorrent ──────────────────────────────────────────────────────────
# Supports libtorrent-rasterbar installed via vcpkg, system package manager,
# or a manual prefix passed with -DCMAKE_PREFIX_PATH=<path>.

find_package(LibtorrentRasterbar QUIET)

# Verify the imported target actually resolved a library on disk.
# vcpkg's LibtorrentRasterbarConfig.cmake can find the config file yet leave
# IMPORTED_LOCATION empty when its Boost find_dependency triggers a warning,
# causing silent link failures.  Fall back to find_library in that case.
if(LibtorrentRasterbar_FOUND AND TARGET LibtorrentRasterbar::torrent-rasterbar)
    get_target_property(_lt_loc LibtorrentRasterbar::torrent-rasterbar
                        IMPORTED_LOCATION_RELEASE)
    if(NOT _lt_loc)
        get_target_property(_lt_loc LibtorrentRasterbar::torrent-rasterbar
                            IMPORTED_LOCATION)
    endif()
    if(NOT _lt_loc)
        message(STATUS "LibtorrentRasterbar target has no IMPORTED_LOCATION — falling back to find_library")
        set(LibtorrentRasterbar_FOUND FALSE)
    endif()
endif()

if(NOT LibtorrentRasterbar_FOUND)
    # Direct library search — works reliably with vcpkg static triplets
    find_library(LIBTORRENT_LIB
        NAMES torrent-rasterbar
        PATH_SUFFIXES lib
    )
    find_path(LIBTORRENT_INCLUDE_DIR
        NAMES libtorrent/session.hpp
        PATH_SUFFIXES include
    )
    if(LIBTORRENT_LIB AND LIBTORRENT_INCLUDE_DIR)
        message(STATUS "Found libtorrent via find_library: ${LIBTORRENT_LIB}")
        set(LIBTORRENT_FOUND TRUE)
    else()
        # Last resort: try pkg-config (Linux / MSYS2 / Homebrew)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(LIBTORRENT libtorrent-rasterbar)
        endif()
        if(NOT LIBTORRENT_FOUND)
            message(FATAL_ERROR
                "libtorrent-rasterbar not found.\n"
                "Install it via one of:\n"
                "  vcpkg:  vcpkg install libtorrent:x64-windows-static\n"
                "  MSYS2 (ucrt64): pacman -S mingw-w64-ucrt-x86_64-libtorrent-rasterbar\n"
                "  MSYS2 (mingw64): pacman -S mingw-w64-x86_64-libtorrent-rasterbar\n"
                "  Ubuntu: apt install libtorrent-rasterbar-dev\n"
                "  macOS:  brew install libtorrent-rasterbar\n"
                "Then re-run cmake with -DCMAKE_PREFIX_PATH pointing to the install root.\n"
            )
        endif()
    endif()
endif()

# ── Executable ───────────────────────────────────────────────────────────────
add_executable(torrent-dl src/main.cpp)

target_compile_options(torrent-dl PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra>
)

target_compile_definitions(torrent-dl PRIVATE TORRENT_ABI_VERSION=1)

if(LibtorrentRasterbar_FOUND)
    target_link_libraries(torrent-dl PRIVATE
        LibtorrentRasterbar::torrent-rasterbar
        ftxui::screen ftxui::dom ftxui::component
        nlohmann_json::nlohmann_json
        $<$<PLATFORM_ID:Windows>:winhttp>)
elseif(LIBTORRENT_FOUND AND LIBTORRENT_LIB)
    target_include_directories(torrent-dl PRIVATE ${LIBTORRENT_INCLUDE_DIR})
    target_link_libraries(torrent-dl PRIVATE
        ${LIBTORRENT_LIB}
        ftxui::screen ftxui::dom ftxui::component
        nlohmann_json::nlohmann_json
        $<$<PLATFORM_ID:Windows>:winhttp>)
elseif(LIBTORRENT_FOUND)
    target_include_directories(torrent-dl PRIVATE ${LIBTORRENT_INCLUDE_DIRS})
    target_link_directories(torrent-dl PRIVATE ${LIBTORRENT_LIBRARY_DIRS})
    target_link_libraries(torrent-dl PRIVATE
        ${LIBTORRENT_LIBRARIES}
        ftxui::screen ftxui::dom ftxui::component
        nlohmann_json::nlohmann_json
        $<$<PLATFORM_ID:Windows>:winhttp>)
    target_compile_options(torrent-dl PRIVATE ${LIBTORRENT_CFLAGS_OTHER})
endif()

install(TARGETS torrent-dl RUNTIME DESTINATION bin)
