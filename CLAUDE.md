# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Drift is an Omarchy desktop app: endless YouTube Music radio from a playlist link or a song search. C++17 + Qt 6 Quick (QML, Material style), built with qmake6 into one binary.

## Commands

```sh
bin/build      # qmake6 + make into build/drift
bin/test       # builds tests/drift_tests.pro into build/tests, runs offscreen
bin/install    # bin/build, then makepkg -fsi in pkgbuild/ (Arch package, app launcher entry)
```

Run a single Qt Test function after `bin/test` has built once:

```sh
QT_QPA_PLATFORM=offscreen build/tests/drift_tests parsesPlaylistEntries
```

Runtime needs `mpv` and `yt-dlp`. New source files must be added to **both** `drift.pro` and `tests/drift_tests.pro` (the test binary compiles `src/*.cpp` directly); new QML or i18n files must go in `src/resources.qrc`.

## Architecture

`main.cpp` creates three objects and exposes them to `Main.qml` as context properties: `backend`, `theme`, `translator`. All UI is in `src/Main.qml`.

- **Backend** (`backend.cpp`): the radio queue and all networking.
  - Playlist entries come from `yt-dlp --flat-playlist -J` (`m_fetch`).
  - Radio mixes and search call YouTube Music's internal `youtubei/v1/next` and `/search` endpoints directly over `QNetworkAccessManager`.
  - Each track's stream URL comes from `yt-dlp -g` (`m_resolve`), which is then handed to mpv.
  - Queue logic: each playlist song takes a turn as the seed, then a few tracks from its mix follow. `m_seen` stops anything except playlist songs from repeating. Searching a song sets chain mode (`m_chain`), where the radio follows mix after mix from that one seed.
  - The parse and queue functions (`parseEntries`, `parseRadio`, `parseSearch`, `setSeeds`, `nextSeed`, `addRadio`) are public statics or methods, so tests drive them with JSON fixtures and no network.
  - State is kept with QSettings, so tests call `QStandardPaths::setTestModeEnabled(true)`.
- **Mpv** (`mpv.cpp`): a separate `mpv --idle` process controlled over its JSON IPC socket. It starts lazily on the first load and runs with video off and a small demuxer cache (keeping the app around 200 MB is a stated goal). Commands sent before the socket connects are queued in `m_pending`.
- **process.h**: `findTool` prefers a binary next to `drift.exe` (the Windows bundle) over PATH. `hideConsole` must be applied to every QProcess so Windows doesn't flash a console.
- **Theme** follows Omarchy: the accent comes from `~/.local/state/omarchy/current`, watched with QFileSystemWatcher, and window rounding from `hyprctl -j getoption decoration:rounding`.
- **Translator** is a custom QTranslator that reads flat JSON catalogs in `src/i18n/<omarchy-lang>.json` (English source text → translation; plural strings map to CLDR-category objects). The language comes from `~/.config/omarchy/language` (system locale on Windows). Changing it calls `engine.retranslate()`, so keep plain `qsTr()` in QML and `tr()` in C++, and add new strings to every catalog (`en.json`, `ru.json`).

`theme.cpp`/`translator.cpp` are shared with the sibling app `../dymo` (theme is identical; the translator differs only by the Windows fallback), so keep fixes in sync.

## Release

`.github/workflows/windows.yml` builds `Drift-windows-x64.zip` (Qt 6.8.3 MSVC, windeployqt, bundled mpv.exe/yt-dlp.exe) and the Arch package. Pushing a `v*` tag attaches both to a release. Keep `pkgver` in `pkgbuild/PKGBUILD` and `setApplicationVersion` in `main.cpp` in step with the tag.
