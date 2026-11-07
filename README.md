# UltraZIP

![Version](https://img.shields.io/badge/version-3.0.0-blue.svg)
![C++](https://img.shields.io/badge/C%2B%2B-20-blue.svg)
![Qt](https://img.shields.io/badge/Qt-6-green.svg)
![License](https://img.shields.io/badge/license-MIT-yellow.svg)

**UltraZIP** is a high-performance archiver with a custom adaptive compression engine. It features a modern GUI (built with Qt6) and a powerful CLI, supporting over 25 archive formats, including its own proprietary `.uzip` format.

<p align="center">
  <img src="assets/logo.jpeg" alt="UltraZIP Logo" width="200"/>
</p>

## Key Features

- **Adaptive Compression Engine:** Analyzes a sample of your data to automatically select the best compression method (Zstd or LZMA2) and fine-tunes entropy parameters (`lc`, `lp`, `pb`, BCJ, Delta) to achieve maximum compression.
- **Proprietary `.uzip` Format:** Supports solid compression, streaming, and custom metadata headers.
- **Extensive Format Support:** Read and write support for 20+ formats including 7z, ZIP, TAR, GZ, XZ, Zstd, LZMA, CAB, LHA, ISO, and more via `libarchive`.
- **Native CAB Support:** Built-in native Windows FCI API support for creating `.cab` files without external dependencies.
- **Dual Interface:** A sleek Qt6 GUI with drag-and-drop, file trees, and progress tracking, plus a fully-featured CLI for automation.
- **Cross-Platform:** Builds and runs on Windows, Linux, and macOS.

## Benchmarks

*UltraZIP vs Standard 7-Zip*

| Engine              | Archive Size | Ratio  | Speed |
|---------------------|--------------|--------|-------|
| 7-Zip (Default)     | 21.8 MB      | 72.5%  | Fast  |
| **UltraZIP (Auto)** | **21.7 MB**  | **71.9%** | Fast  |
| 7-Zip (-mx=9)       | 21.7 MB      | 71.9%  | Slow  |

*Tested on a mixed dataset (source code, binaries, and assets). UltraZIP achieves maximum compression levels by default without manual parameter tuning.*

## Building from Source

### Requirements

- C++20 compiler (GCC 10+, Clang 12+, MSVC 19.29+)
- CMake 3.20+
- Qt6 (Base & Widgets)
- libarchive, libzstd, liblzma, libbz2

### MSYS2 (Windows)

```bash
pacman -S mingw-w64-x86_64-{gcc,cmake,ninja,qt6-base,libarchive,zstd,xz,bzip2}
git clone https://github.com/Hiss-Software/UltraZIP.git
cd UltraZIP
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Linux (Debian/Ubuntu)

```bash
sudo apt install build-essential cmake qt6-base-dev libarchive-dev \
    libzstd-dev liblzma-dev libbz2-dev ninja-build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The compiled binary will be located in `build/bin/uzip`.

## Command Line Usage

If no arguments are provided, UltraZIP launches the GUI. To use the CLI:

```bash
# Compress files using auto-adaptive settings
uzip c archive.uzip file1.txt file2.txt

# Compress a whole directory using Zstd
uzip c -m zstd -l 22 archive.uzip ./my_project/

# Extract an archive
uzip x archive.uzip -o ./output/

# List archive contents (verbose)
uzip l -v archive.uzip

# Test archive integrity
uzip t archive.uzip
```

## License

Distributed under the MIT License. See `LICENSE` for more information.

---

*Created by [Hiss-Software](https://github.com/Hiss-Software)*