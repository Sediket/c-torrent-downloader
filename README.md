# torrent-dl

A single-binary CLI torrent downloader with a retro BBS-style terminal UI. Built on [libtorrent-rasterbar](https://www.libtorrent.org/) and [FTXUI](https://github.com/ArthurSonzogni/FTXUI).

## Download

Grab the latest binary from the [Releases](https://github.com/Sediket/c-torrent-downloader/releases) page — no install needed.

## Usage

```
torrent-dl [options] [torrent-file-or-magnet-link]

Options:
  -o <dir>   Output directory (default: current directory)
  -p <port>  Listen port     (default: 6881)
  -h         Show help
```

### Interactive mode

Run with no arguments to open the search interface. Type a query and press `Enter` to search, use arrow keys to navigate results, and press `Enter` again to start downloading.

```
torrent-dl
```

### Direct mode

Pass a `.torrent` file or magnet link to skip straight to the download screen.

```
torrent-dl -o ~/Downloads ubuntu.torrent
torrent-dl -o ~/Downloads "magnet:?xt=urn:btih:..."
```

Press `Tab` to switch between the Search and Downloads tabs. Press `Ctrl+C` to quit.

## Building from source

### Prerequisites

Install **libtorrent-rasterbar** and **CMake ≥ 3.16**:

| Platform | Command |
|---|---|
| Windows (vcpkg) | `vcpkg install libtorrent:x64-windows` |
| Windows (MSYS2) | `pacman -S mingw-w64-x86_64-libtorrent-rasterbar` |
| Ubuntu / Debian | `apt install libtorrent-rasterbar-dev` |
| Fedora | `dnf install libtorrent-rasterbar-devel` |
| macOS | `brew install libtorrent-rasterbar` |

FTXUI and nlohmann/json are fetched automatically by CMake.

### Build

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

On Windows with vcpkg, add `-DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake` to the configure step.

Output: `build/torrent-dl` (Linux/macOS) or `build\Release\torrent-dl.exe` (Windows).
