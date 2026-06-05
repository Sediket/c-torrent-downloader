# torrent-dl — Build Instructions

A minimal CLI torrent downloader built directly on **libtorrent-rasterbar**
(the same library powering qBittorrent).

## Prerequisites

Install **libtorrent-rasterbar** for your platform:

| Platform | Command |
|----------|---------|
| Windows (vcpkg) | `vcpkg install libtorrent:x64-windows` |
| Windows (MSYS2) | `pacman -S mingw-w64-x86_64-libtorrent-rasterbar` |
| Ubuntu / Debian | `apt install libtorrent-rasterbar-dev` |
| Fedora / RHEL   | `dnf install libtorrent-rasterbar-devel` |
| macOS (Homebrew)| `brew install libtorrent-rasterbar` |

You also need **CMake ≥ 3.16** and a C++17-capable compiler (GCC 9+, Clang 10+, MSVC 2019+).

## Build

```sh
# Configure
cmake -B build -DCMAKE_BUILD_TYPE=Release

# If using vcpkg, add:
# -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake

cmake --build build --config Release
```

The binary is `build/torrent-dl` (or `build/Release/torrent-dl.exe` on Windows).

## Usage

```
torrent-dl [options] <torrent-file-or-magnet-link>

Options:
  -o <dir>   Save directory  (default: current directory)
  -p <port>  Listen port     (default: 6881)
  -h         Show help
```

### Examples

```sh
# Download a .torrent file to ~/Downloads
torrent-dl -o ~/Downloads ubuntu.torrent

# Download via magnet link
torrent-dl -o ~/Downloads "magnet:?xt=urn:btih:..."

# Custom port
torrent-dl -p 50000 -o /tmp/dl some.torrent
```

Press **Ctrl+C** to stop.  
The progress line shows: state · percent · download rate · upload rate · peer count · bytes done / total.
