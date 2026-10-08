# C++ development

See the [main README](../README.md) for the portable download, setup, artwork, and privacy.

## Build and test

Use Windows 10 1809 or newer, x64, and Visual Studio 2022 / Build Tools with **Desktop development with C++** and a Windows 10/11 SDK. Run these commands from the repository root:

```powershell
powershell -ExecutionPolicy Bypass -File .\native\build.ps1 -Test
.\build\native\Release\AppleMusicPresenceNative.exe
```

After building, `launch.cmd` also opens the app and forwards diagnostic arguments.

The x64 C++20 Release build uses a static C++ runtime. Test assertions stay enabled; application release assertions are disabled to avoid embedding private build paths.

Tests use fake Discord pipes, public-catalog/hosting fixtures, and isolated child processes. They cover metadata, album identity, duplicate JPEGs, HLS limits, image metadata rejection, credentials, same-song upgrades, stale jobs, reconnects, and cancellation. See [validation](../VALIDATION.md) for live checks.

## Architecture

- `MediaBackend` uses [Windows media sessions](https://learn.microsoft.com/en-us/uwp/api/windows.media.control.globalsystemmediatransportcontrolssession) and wakes the service on playback events.
- `Service` coordinates playback and sharing, with a 30-second health check, reconnect backoff, and a five-second minimum between changed activities.
- `DiscordRpc` uses [Discord's documented local RPC](https://docs.discord.com/developers/topics/rpc). Pause, stop, and exit clear the activity.
- `ArtworkResolver` checks exact song metadata, exact artist/full album edition, then public Apple Music pages. Identical duplicate covers can share display artwork; uncertain releases omit album links and animation.
- Background artwork/motion workers keep lookup, encoding, and upload work off the UI thread. Downloads and conversion are bounded; exit cancels work.

Dependencies are Windows SDK C++/WinRT, Win32, WinHTTP, BCrypt, and vendored [nlohmann/json 3.12.0](https://github.com/nlohmann/json/releases/tag/v3.12.0) under [MIT](vendor/nlohmann/LICENSE.MIT). FFmpeg is optional and runs only for new animations; Git credential lookup runs only for publication. See [third-party notices](../THIRD_PARTY_NOTICES.txt).

Preferences use `%LOCALAPPDATA%\AppleMusicPresence\native_settings.json`. First launch can import non-secret legacy `settings.json`; custom artwork maps and verified motion-cache entries remain compatible. Legacy files are not overwritten.

## Diagnostic options

```powershell
.\build\native\Release\AppleMusicPresenceNative.exe --diagnose
.\build\native\Release\AppleMusicPresenceNative.exe --headless --demo --seconds 5
```

`--diagnose` prints local media availability flags. `--demo` uses an offline sample and never writes preferences, contacts Apple/Discord, or changes startup. `--headless` requires `--seconds` for a bounded run; omit `--demo` to use saved live settings. `--seconds` accepts positive durations up to 86400 seconds.

## Packaging

After a successful build and tests:

```powershell
powershell -ExecutionPolicy Bypass -File .\native\package.ps1 -Version native-v0.1.0-preview.1
```

The ZIP and `SHA256SUMS.txt` appear in `dist/native`. Packaging validates the x64 GUI header and rejects private profile paths in the executable. Its explicit file list includes only the app, concise setup, optional converter installer, and third-party notices. Settings, caches, tests, debug files, and FFmpeg stay outside the ZIP.

`Install-MotionSupport.cmd` launches the optional PowerShell installer. It downloads FFmpeg 9.0.2 directly from [Gyan's public release](https://github.com/GyanD/codexffmpeg/releases/tag/9.0.2), verifies pinned archive/executable SHA256 hashes, and extracts only the converter and provider notices. It does not sign into GitHub or change settings/startup. Alternatively, place an `ffmpeg.exe` supporting `libwebp_anim` beside the app or in an absolute directory on PATH. [FFmpeg's download page](https://ffmpeg.org/download.html) lists Windows providers.
