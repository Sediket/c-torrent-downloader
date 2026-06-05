# torrent-dl — Codebase Map for Claude Code

## What this project is

A single-binary C++17 CLI torrent downloader with a retro-BBS TUI. It wraps
**libtorrent-rasterbar** (same library as qBittorrent) and draws its UI with
**FTXUI**. No Qt, no GUI framework.

Two modes:
1. **Direct mode** — pass a `.torrent` file or `magnet:` URI on the CLI; jumps
   straight to the download screen.
2. **Interactive mode** — run with no arguments; shows a search screen that
   queries the Pirate Bay API (`apibay.org/q.php`), lets the user pick a
   result, then transitions to the download screen.

## File layout

```
c-torrent-downloader/
├── src/
│   ├── main.cpp        # Orchestration: globals, CLI parsing, event handlers, screen.Loop (~280 lines)
│   ├── state.h         # All shared types (DownloadEntry, SearchResult, SearchState) and extern globals
│   ├── formatters.h    # Pure helpers: fmt_rate, fmt_bytes, url_encode
│   ├── search.h        # winhttp_get (Win32), trigger_search (detached thread + JSON parsing)
│   ├── torrent.h       # make_session_settings, run_lt_dispatch, TRACKERS constant
│   └── ui.h            # FTXUI builders: render_dl_row, build_dl_list_ui, render_result_row, build_search_ui
├── CMakeLists.txt      # Build system (single TU — headers only, no CMake changes needed)
└── BUILD.md            # Install/build instructions
```

`build/` is the out-of-source CMake build tree (gitignored). `build/_deps/`
holds FetchContent downloads (ftxui, nlohmann/json).

## Dependencies

| Dep | How acquired | Purpose |
|-----|-------------|---------|
| **libtorrent-rasterbar** | System package / vcpkg (must be pre-installed) | BitTorrent session, add_torrent, alerts |
| **FTXUI v5.0.0** | CMake FetchContent (auto-downloaded) | Terminal UI components |
| **nlohmann/json v3.11.3** | CMake FetchContent (auto-downloaded) | Parse apibay.org JSON responses |
| **WinHTTP** (Windows only) | OS SDK | HTTPS GET for search API |

libtorrent is the only dep that must be installed manually before cmake
configure succeeds. FTXUI and nlohmann/json are fetched by CMake.

## Architecture

Still a single translation unit — `main.cpp` `#include`s all headers, so the
compiler sees one TU. The headers split the code by concern so each file is
focused and easy to navigate:

### `state.h`
All shared types (`DownloadEntry`, `SearchResult`, `SearchState`, `SearchStatus`)
and `extern` declarations for every global (`g_downloads`, `g_search`,
`g_handles`, `g_ses`, `g_tab_index`, etc.). Definitions live in `main.cpp`.
Read this file first to understand the full data model.

### `formatters.h`
Pure, stateless utilities: `fmt_rate`, `fmt_bytes`, `url_encode`. No deps beyond
standard headers. Safe to `#include` anywhere.

### `search.h`
- `winhttp_get(host, path)` — synchronous HTTPS GET (`#ifdef _WIN32` only).
- `trigger_search(screen, query)` — fires a detached thread that calls the API,
  parses JSON, and posts results back via `screen.Post()`.

### `torrent.h`
- `TRACKERS[]` / `TRACKER_COUNT` — public tracker list used in magnet URIs.
- `make_session_settings(port)` — shared libtorrent settings factory (used by
  both CLI and interactive modes, eliminating the previous duplication).
- `run_lt_dispatch(screen, ses)` — alert polling loop (500 ms sleep); called from
  a dedicated thread. Posts `g_downloads` updates via `screen.Post()`.

### `ui.h`
FTXUI Element builders — all read-only access to state, never mutate globals:
- `render_dl_row`, `build_dl_list_ui` — downloads tab.
- `render_result_row`, `build_search_ui` — search tab.
- `SPINNER_FRAMES[]` / `SPINNER_COUNT` — braille spinner animation constants.

### `main.cpp`
Global definitions, signal handler, `print_usage`/`is_magnet` CLI helpers, and
`main` itself. Two paths in `main`:
1. **CLI source provided**: creates session via `make_session_settings`, adds
   torrent, wires FTXUI renderer + key handler, joins dispatch thread.
2. **No source (interactive)**: creates session, `Input` component,
   `start_download` lambda, search/download components, tab layout, spinner
   thread, dispatch thread, then `screen.Loop`.

## Coding and Troubleshooting Guidance
- Use Official sites and documentation as examples of working code.
- Use other GitHub projects as references as examples of working code.


## Build

```powershell
# Windows — vcpkg path
cmake -B build -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
# Output: build\Release\torrent-dl.exe

# Windows — MSYS2 (run inside MSYS2 shell)
cmake -B build -DCMAKE_BUILD_TYPE=Release -G "MinGW Makefiles"
cmake --build build

# Compile script
build.ps1
```

See `BUILD.md` for all platforms.

## Key design decisions / non-obvious things

- **Single translation unit** — `main.cpp` `#include`s all headers; it's still
  one TU. CMakeLists.txt doesn't change. Keep it this way; only split into
  separate `.cpp` files if the build time becomes a problem.
- **screen.Post() for all state writes** — libtorrent and search threads never
  write to `DownloadEntry` / `SearchState` directly; they always go through
  `screen.Post(lambda)` which serialises onto the FTXUI event thread.
- **`g_ses` is heap-allocated** in search mode because the session must outlive
  the lambda that creates it; in direct mode it's stack-allocated.
- **WinHTTP only** — search calls are `#ifdef _WIN32` guarded in `search.h`. On
  Linux/macOS the search always returns an empty result set. To add curl support,
  implement the `#else` branch in `trigger_search`.
- **Magnet URI construction** — built in `main.cpp` from `info_hash` + `TRACKERS`
  array defined in `torrent.h`.
- **FTXUI v5** — API differs from v4 in several places (e.g. `InputOption`
  struct, `CatchEvent` signature). Don't upgrade without checking.
