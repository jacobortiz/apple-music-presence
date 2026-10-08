# Native tray preview

A small C++ Windows app that runs in the notification area. It needs no Python runtime and keeps its settings window closed during normal use.

## Build and run

Use Windows 10 1809 or newer, x64, and Visual Studio 2022 / Build Tools with **Desktop development with C++** and a Windows 10/11 SDK.

From the repository folder:

```powershell
powershell -ExecutionPolicy Bypass -File .\native\build.ps1 -Test
.\build\native\Release\AppleMusicPresenceNative.exe
```

Open the music-note tray icon for settings. Enter your Discord Application ID if needed; Apple Music and Discord desktop must be running. Right-click the icon to pause/resume sharing or exit. Closing the settings window keeps the app running. The icon may be in Windows' hidden-icons menu.

**Start with Windows** is optional and takes effect when you save. It uses a startup entry for your Windows account; no administrator rights are needed. Disable it before moving or deleting the executable. Run one presence app per Discord Application ID.

## Included

- Event-driven Apple Music track detection, title/artist/album, and playback progress.
- The artist in Discord's member list and the Apple Music profile heading.
- Clearing on pause/stop/exit, reconnects, and an idle health check every 30 seconds.
- Optional normal artwork from exact public Apple catalog matches, requested at 1024px.
- Existing hosted animations from custom maps or the Python app's verified motion cache.
- One background service worker; artwork has a separate worker only when enabled. Network requests never run on the settings-window thread.

This is the first native implementation. **It does not discover, convert, or upload new motion covers yet.** Public Apple Music page fallback and comparison of duplicate JPEG covers also remain in the Python app, so some tracks may have artwork there but not here. Keep using Python when you need those features.

## Preferences and privacy

On first launch, the native app imports the existing non-secret Python preferences. It saves its own `native_settings.json` in `%LOCALAPPDATA%\AppleMusicPresence`, leaving the Python preferences intact.

Artwork opt-in sends the current song's tags to Apple's public search service. Hosted animations require the existing repository setting and matching Apple album ID. Custom `album_artwork.json` maps use the same format as the Python app. No credentials, account login, listening history, telemetry, or new GitHub uploads are involved in this native version.

## Diagnostics

```powershell
.\build\native\Release\AppleMusicPresenceNative.exe --diagnose
.\build\native\Release\AppleMusicPresenceNative.exe --headless --demo --seconds 5
```

`--diagnose` reads Windows media metadata and prints only availability flags. `--demo` uses an offline sample and never writes preferences, contacts Apple/Discord, or changes Windows startup. `--headless` requires a duration and is intended for bounded verification.

Tests use fake Discord pipes and offline artwork/media fixtures. They cover framing, cancellation, reconnects, pause/stop mapping, release matching, unsafe URLs, malformed local files, and worker lifecycle. Live profile rendering still depends on Discord.

## Native dependencies

Windows SDK [C++/WinRT media sessions](https://learn.microsoft.com/en-us/uwp/api/windows.media.control.globalsystemmediatransportcontrolssession), Win32 tray/windows, WinHTTP, and [Discord's documented local RPC](https://docs.discord.com/developers/topics/rpc). JSON uses vendored [nlohmann/json 3.12.0](https://github.com/nlohmann/json/releases/tag/v3.12.0) under [MIT](vendor/nlohmann/LICENSE.MIT); its single header's SHA-256 is `aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63`.

Next migration work: Apple page/duplicate-cover fallback, then motion discovery, bounded conversion, and credential-safe publication. FFmpeg should run only when a new animation needs preparation.
