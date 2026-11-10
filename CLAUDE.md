# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Drift is an Omarchy desktop app that also ships for Windows (and builds on macOS): endless YouTube Music radio from a playlist link or a song search. C++17 (C++20 on Windows, for C++/WinRT) + Qt 6 Quick (QML, Material style), built with qmake into one binary.

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

`bin/*` are POSIX sh scripts for Linux and macOS (`qmake6`, `make`); on Windows see below.

### Windows

Build with Qt for MSVC from a shell that has the MSVC environment (`vcvars64.bat`); MinGW can't build `smtc.cpp` (C++/WinRT):

```bat
mkdir build\win && cd build\win
qmake CONFIG-=debug_and_release ..\..\drift.pro
set CL=/MP
nmake
```

Qt Creator builds into `build\Desktop_Qt_<version>_MSVC2022_64bit_<Debug|Release>\`, with a `drift.exe` under both `debug\` and `release\`. Its Run button starts `release\drift.exe` even in the Debug kit, because `drift.pro` sets `CONFIG += release`; `nmake debug` or `nmake release` there rebuilds one by hand.

`mpv.exe` and `yt-dlp.exe` aren't installed system-wide on a dev machine. Drift looks for each beside `drift.exe` first, then on `PATH` (`findTool`), and without mpv it shows "mpv isn't installed" on the first play. Put both next to the `drift.exe` that actually runs (the folder is git-ignored), from the same sources the release workflow uses:

```powershell
$dir = "build\win"   # wherever drift.exe is
Invoke-WebRequest https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe -OutFile "$dir\yt-dlp.exe"
$mpv = (Invoke-RestMethod https://api.github.com/repos/shinchiro/mpv-winbuild-cmake/releases/latest).assets |
    Where-Object name -like 'mpv-x86_64-2*.7z' | Select-Object -First 1
Invoke-WebRequest $mpv.browser_download_url -OutFile "$env:TEMP\mpv.7z"
& "$env:ProgramFiles\7-Zip\7z.exe" e "$env:TEMP\mpv.7z" "-o$dir" mpv.exe
```

On Windows mpv's IPC is the named pipe `\\.\pipe\drift-mpv-<pid>` rather than a socket file; QLocalSocket reaches both the same way.

### Adding files

Runtime needs `mpv` and `yt-dlp` on every platform. New source files must be added to **both** `drift.pro` and `tests/drift_tests.pro` (the test binary compiles `src/*.cpp` directly); new QML or i18n files must go in `src/resources.qrc`.

## Architecture

`main.cpp` creates three objects and exposes them to `Main.qml` as context properties: `backend`, `theme`, `translator`. All UI is in `src/Main.qml`. Only one Drift runs: a `QLocalServer` named `drift-$USER` takes a second launch's connection as "show the window", and the one line it sends as a `drift://play/<video id>` link to play (`Backend::linkId`, `playId`). The scheme is registered by `drift.iss`, by `main.cpp` itself for the portable zip (HKCU), and by `drift.desktop` on Linux. Closing the window while a track is loaded deletes the QML engine (and `malloc_trim`s) but keeps `Backend` and mpv playing; `show()` rebuilds the engine.

- **Backend** (`backend.cpp`): the radio queue and all networking.
  - Playlist entries come from `yt-dlp --flat-playlist -J` (`m_fetch`).
  - Radio mixes and search call YouTube Music's internal `youtubei/v1/next` and `/search` endpoints directly over `QNetworkAccessManager`.
  - Each track's stream URL comes from `yt-dlp -g` (`m_resolve`), which is then handed to mpv.
  - Queue logic: each playlist song takes a turn as the seed, then a few tracks from its mix follow. `m_seen` stops anything except playlist songs from repeating. Searching a song sets chain mode (`m_chain`), where the radio follows mix after mix from that one seed.
  - `m_history` holds the last 20 tracks for `previous()`, which puts the current one back at the head of the queue. `m_disliked` (video ids, the `disliked` setting) keeps a track out of every radio; picking it by hand in `playSong` takes that back.
  - `playNext` saves the track and mode under `last/` and the destructor its position; `resume()` (called from `main.cpp` unless a `drift://` link came) plays it again at launch and seeks on mpv's first `time-pos`.
  - Loudness is leveled by mpv's `dynaudnorm` filter (`kNormalize`, the `normalize` setting, L key), set over IPC like the volume.
  - The parse and queue functions (`parseEntries`, `parseRadio`, `parseSearch`, `setSeeds`, `nextSeed`, `addRadio`) are public statics or methods, so tests drive them with JSON fixtures and no network.
  - State is kept with QSettings, so tests call `QStandardPaths::setTestModeEnabled(true)`.
- **Mpv** (`mpv.cpp`): a separate `mpv --idle` process controlled over its JSON IPC socket. It starts lazily on the first load and runs with video off and a small demuxer cache (keeping the app around 200 MB is a stated goal). Commands sent before the socket connects are queued in `m_pending`.
- **Mpris** (`mpris.cpp`, Linux only, QtDBus): `org.mpris.MediaPlayer2.drift` with the root and Player interfaces, driving `Backend`. Media keys and Omarchy's bar go through it. `PropertiesChanged` is sent by hand from `Backend` signals; position isn't announced per tick, only `Seeked` on jumps.
- **Smtc** (`smtc.cpp`, Windows only, C++/WinRT from the Windows SDK, linked with `windowsapp`): the same controls through System Media Transport Controls, taken from a `MediaPlayer` with its playback disabled since there's no window handle in the background. WinRT headers stay out of `smtc.h` (pimpl), and button presses arrive on a Windows thread, so they're queued to Qt's. The play/pause and next media keys, and Alt+X / Alt+C for the same and Alt+V / Alt+B for volume down / up (picked to stay clear of CS2's binds), are global hotkeys via `RegisterHotKey`, since SMTC alone didn't deliver the keys. A tray icon (QSystemTrayIcon, so Windows builds use QApplication and Widgets) brings back a closed window, pauses, skips and quits.
- **Discord** (`discord.cpp`, every platform): Rich Presence over Discord's local IPC socket (`\\.\pipe\discord-ipc-N`, or `discord-ipc-N` in the runtime dir), spoken by hand on a QLocalSocket: an 8-byte little-endian opcode and length, then JSON. It never polls: Discord is looked for only when a track starts or resumes, changes are collected for a second into one `SET_ACTIVITY`, and Discord draws the progress bar from the start/end times, so position ticks only compare against what was sent (a seek or slow load resends). Pausing clears the activity. Its two buttons are "Listen in Drift" and "Listen on YouTube Music"; Discord only takes web links, so the first goes to `docs/index.html` (GitHub Pages from master `/docs`, `https://gamawn.github.io/drift/?v=<id>`), which redirects to `drift://play/<id>`. `kClientId` is Drift's application in Discord's developer portal; the D key toggles the `discord` setting.
- **Updater** (`updater.cpp`, every platform): asks GitHub for the latest release five seconds after launch and once a day. A copy put there by the Windows installer (`unins000.exe` beside `drift.exe`) downloads `Drift-setup-x64.exe` to the temp folder, checks it against the asset's SHA-256 `digest` from GitHub's API and offers "Restart and update" in the window and the tray; if that isn't pressed, `installPending` runs it at the next launch, once per version (`update/attempted`), and waits: the installer closes Drift to replace it, and one that gives up (a refused UAC prompt) lets the launch go on. The installer is run with `/SILENT /RELAUNCH=1`, and `drift.iss` starts Drift again on `/RELAUNCH` with `--from-setup`; without that argument Drift exits while the installer's `SetupMutex` (`DriftSetup`) exists. The portable zip and packages only get "Drift X is out" with a link to the release page.
- **process.h**: `findTool` prefers a binary next to `drift.exe` (the Windows bundle) over PATH. `hideConsole` must be applied to every QProcess so Windows doesn't flash a console.
- **Theme** follows Omarchy: the accent comes from `~/.local/state/omarchy/current`, watched with QFileSystemWatcher, and window rounding from `hyprctl -j getoption decoration:rounding`.
- **Translator** is a custom QTranslator that reads flat JSON catalogs in `src/i18n/<omarchy-lang>.json` (English source text → translation; plural strings map to CLDR-category objects). The language comes from `~/.config/omarchy/language` (system locale on Windows). Changing it calls `engine.retranslate()`, so keep plain `qsTr()` in QML and `tr()` in C++, and add new strings to every catalog (`en.json`, `ru.json`).

`theme.cpp`/`translator.cpp` are shared with the sibling app `../dymo` (theme is identical; the translator differs only by the Windows fallback), so keep fixes in sync.

## Release

`.github/workflows/windows.yml` builds `Drift-setup-x64.exe` (Inno Setup, `windows/drift.iss`, version from `pkgver`) and `Drift-windows-x64.zip` (Qt 6.8.3 MSVC, windeployqt, bundled mpv.exe/yt-dlp.exe) and the Arch package. Pushing a `v*` tag attaches all three to a release. Keep `pkgver` in `pkgbuild/PKGBUILD` and `setApplicationVersion` in `main.cpp` in step with the tag.
