# Native tray app

A small C++ Windows app that shares Apple Music with Discord from the notification area. It runs without Python and keeps its settings window closed during normal use.

[Download the portable Windows preview](https://github.com/jacobortiz/apple-music-presence/releases/tag/native-v0.1.0-preview.1), extract all files to a permanent folder, and open **AppleMusicPresenceNative.exe**.

## Build and run

Use Windows 10 1809 or newer, x64, and Visual Studio 2022 / Build Tools with **Desktop development with C++** and a Windows 10/11 SDK.

```powershell
powershell -ExecutionPolicy Bypass -File .\native\build.ps1 -Test
.\build\native\Release\AppleMusicPresenceNative.exe
```

Open the music-note tray icon for settings. Enter your Discord Application ID; Apple Music and Discord desktop must be running. Right-click the icon to pause/resume sharing or exit. Closing settings keeps sharing. The icon may be in Windows’ hidden-icons menu.

**Start with Windows** is optional and takes effect when you save. Disable it before moving the executable. Run one presence app per Discord Application ID.

## Artwork

Enable normal artwork to look up the exact song and album edition through Apple’s public catalog. If the song is missing, the app looks for its exact artist and full album title, then uses Apple Music pages as fallback. Covers request 1024px. Duplicate releases share a normal cover only when the display bytes match; an uncertain release omits the album link and motion lookup.

For automatic animations, enable **Prefer animated covers when available**, enter a public repository as `owner/repository`, and configure write access using either your existing Git for Windows credential-helper sign-in or `APPLE_MUSIC_PRESENCE_GITHUB_TOKEN` with **Contents: read and write**. Tokens never belong in settings or Git.

Close the app, then double-click **Install-MotionSupport.cmd** in the downloadable ZIP to download and verify an optional FFmpeg 9.0.2 converter directly from [Gyan’s public release](https://github.com/GyanD/codexffmpeg/releases/tag/9.0.2). Restart the app afterwards. The installer retains the provider’s license and build notices. The converter runs only for new motion covers; normal artwork and existing animations work without it. You can also place a Windows **ffmpeg.exe** supporting `libwebp_anim` beside the app or in an absolute directory on PATH; [FFmpeg’s download page](https://ffmpeg.org/download.html) lists Windows builds.

Normal artwork stays visible while the app prepares a looping WebP and publishes it to the repository’s **motion-artwork** branch. The same song then upgrades to the animation. Missing motion, unsupported streams, conversion failures, or upload failures preserve normal artwork. Animated results cache for seven days, no-motion results for one day, and temporary failures retry after a minute. A verified Apple album ID shares covers across songs and guest artists.

Motion covers use Lanczos scaling at 768px, quality 85, with smaller frame rate/size only when needed for the 8 MB limit. Downloads and conversion are bounded; quitting cancels background work. Discord controls image caching and animation playback.

Existing Python `album_artwork.json` maps and verified `motion_cache.json` entries are compatible. See the main [artwork instructions](../README.md#animated-covers).

## Preferences and privacy

On first launch, the app imports non-secret Python preferences. It saves its own `native_settings.json` in `%LOCALAPPDATA%\AppleMusicPresence`, leaving Python settings intact. Maps and the bounded album cache stay in that folder.

Artwork opt-in sends track tags to Apple. Motion opt-in uploads only metadata-free public cover images, with album IDs and a GitHub no-reply commit identity. Public artwork commits reveal prepared albums and upload times. Credentials remain in memory and reach GitHub’s API only; they never reach public image downloads or the converter. The app collects no local music files, browser cookies, Apple passwords, Discord tokens, telemetry, or per-song history.

Media detection uses Windows events. Separate background workers handle artwork and motion, so lookups, encoding, and uploads do not delay playback detection or the settings window. Pause/stop/exit clear Discord activity; reconnects use backoff and an idle health check.

## Verification

```powershell
.\build\native\Release\AppleMusicPresenceNative.exe --diagnose
.\build\native\Release\AppleMusicPresenceNative.exe --headless --demo --seconds 5
```

`--diagnose` prints only local media availability flags. `--demo` uses an offline sample and never writes preferences, contacts Apple/Discord, or changes startup. `--headless` requires a duration.

Tests use fake Discord pipes, public-catalog/hosting fixtures, and isolated child processes. They cover album identity, duplicate JPEGs, HLS limits, image metadata rejection, credentials, same-song upgrades, stale jobs, reconnects, and cancellation. Live profile rendering still depends on Discord.

## Dependencies

Windows SDK [C++/WinRT media sessions](https://learn.microsoft.com/en-us/uwp/api/windows.media.control.globalsystemmediatransportcontrolssession), Win32, WinHTTP, BCrypt, and [Discord’s documented local RPC](https://docs.discord.com/developers/topics/rpc). JSON uses vendored [nlohmann/json 3.12.0](https://github.com/nlohmann/json/releases/tag/v3.12.0) under [MIT](vendor/nlohmann/LICENSE.MIT). FFmpeg is an optional separate executable; Git credential lookup runs only for motion publication.

## Packaging

Download the [native preview ZIP](https://github.com/jacobortiz/apple-music-presence/releases/tag/native-v0.1.0-preview.1), extract it into a permanent folder, and open **AppleMusicPresenceNative.exe**. No Python, compiler, or admin install is needed. For new animated covers, double-click **Install-MotionSupport.cmd** once, then configure animated artwork in the app.

To build the downloadable preview after tests pass:

```powershell
powershell -ExecutionPolicy Bypass -File .\native\package.ps1 -Version native-v0.1.0-preview.1
```

The ZIP and `SHA256SUMS.txt` appear in `dist/native`. Packaging checks the app’s x64 GUI header and rejects private profile paths in its binary. An explicit file list includes only the app, concise setup, optional converter installer, and third-party notices; settings, caches, tests, debug files, and FFmpeg stay outside the ZIP. The installer uses a fixed public release URL, validates archive and executable SHA256 hashes, and extracts only the converter and its notices. It does not sign into GitHub or touch app settings/startup.
