# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

SMTCLyrics is a Windows desktop lyrics overlay application. It reads current playback info via the SMTC (System Media Transport Controls) WinRT API, fetches synchronized lyrics from online sources (QQ Music, Kugou, Kuwo, Netease) or local `.lrc` files, and renders them in a transparent topmost window with karaoke-style per-word highlighting.

This is a C++ rewrite of an earlier E-language (易语言) application. The original source lives in `../e/`.

## Build Commands

Requires CMake 3.22+, a C++20 compiler (MinGW or MSVC), and Windows 10+.

```bash
# Configure (from cpp/ directory)
cmake --preset mingw-debug      # or mingw-release

# Build
cmake --build build/mingw-debug

# Run tests
./build/mingw-debug/SMTCLyricsTests.exe
```

The test executable is a hand-rolled harness (no Google Test/Catch2). It requires `config.ini`, `cache.json`, and `lyrics/` at runtime — these are copied from `../e/` via CMake POST_BUILD commands.

## Architecture

All code lives under `src/` in the `smtc::` namespace with sub-namespaces matching directory names (`smtc::app`, `smtc::lyrics`, `smtc::config`, etc.).

**Entry point**: `main.cpp` → `smtc::app::Application::run()` — standard Win32 message loop with two timers:
- SMTC timer (500–2000ms): polls `SmtcProvider` for current media session state
- Render timer (16ms): redraws lyrics at ~60fps for smooth karaoke animation

**Module responsibilities**:
- `app/` — `Application` owns all components, routes `WM_APP` messages from background threads
- `lyrics/` — `LyricRepository` orchestrates local-first then online loading; `LrcParser` handles LRC/KRC/QRC formats with per-word timing; `OnlineLyrics` fetches from 4 sources; `QrcDecrypter` does Triple-DES decryption for QQ Music QRC
- `smtc/` — `SmtcProvider` reads WinRT `Windows.Media.Control` sessions
- `ui/` — `DesktopLyricsWindow` is a `WS_EX_LAYERED | WS_EX_TOPMOST` transparent overlay using GDI+; `ControlWindow` is the settings GUI
- `config/` — INI config with automatic migration from legacy Chinese section/key names (from the original E-language app)
- `cache/` — JSON-based per-song lyric source and offset cache (`cache.json`)
- `http/` — WinHTTP wrapper for GET/POST
- `util/` — `Encoding` (UTF-8/16/ANSI, URL encode), `Inflate` (pure C++ zlib decompressor, no external zlib), `Base64`, `Path`

**Key patterns**:
- Async lyric loading: background `std::thread` fetches lyrics, posts results back via `PostMessageW(WM_APP_*)`. A `lyricLoadRequestId_` counter discards stale responses.
- Per-word karaoke: `LrcParser::frameAt()` returns a `highlightPercent` (0–100) based on individual word segment timing in QRC/KRC formats.
- Dual SMTC modes: Mode 1 polls position with local extrapolation + 1500ms calibration; Mode 2 only updates on play/pause/seek events and extrapolates continuously.

## Dependencies

**System libs** (linked via CMake): `gdiplus`, `winhttp`, `crypt32`, `shlwapi`, `runtimeobject`, `windowsapp`, `ole32`, `shell32`, `comdlg32`, `Shcore`

**Third-party**:
- `nlohmann/json` (header-only) — located at `../e-packager-master/thirdparty/json.hpp`, included via CMake include path
- `Microsoft.Windows.CppWinRT` NuGet package — for WinRT projections; pre-generated headers in `generated/winrt/`

## Runtime Files

These files live alongside the built executable:
- `config.ini` — font, colors, SMTC mode, display mode, source priority, window position
- `cache.json` — per-song lyric source preference and offset memory (base64-encoded keys)
- `lyrics/` — local `.lrc` files, named `歌手 歌名.lrc`

## Related Directories

- `../e/` — original E-language project and resources (test fixtures are copied from here)
- `../e-packager-master/` — provides `thirdparty/json.hpp`
- `../qrckit-master/` — Kotlin reference implementation for QRC decryption
