# Apple Music Presence

A Python desktop MVP that shares the current track from the **native Windows Apple Music app** as a Discord **Listening** activity. Start/Stop controls and a live track preview are included. Media detection is isolated behind a small interface for a future C++ backend.

## What works

- Title, artist, and album from Windows media sessions; other players and browser sessions are ignored by default.
- Playback progress derived from Windows' position, duration, and last-update timestamp. Seeking and track changes update Discord's timestamps.
- Pause, stop, missing metadata, and disappearing sessions clear the activity. Resume publishes the current track again.
- Discord and Apple Music can be closed/reopened. The bridge rediscovers media sessions and retries Discord with exponential backoff up to 30 seconds.
- Optional album art and a **Listen on Apple Music** button from Apple's public catalog. Art lookup is off by default.
- Handles Apple Music's combined `Artist — Album` Windows metadata when its album field is empty. The desktop reports whether artwork is off, being looked up, unmatched, or sent to Discord.
- An offline preview and a read-only media diagnostic command.

This is a local desktop bridge. Discord desktop, Apple Music, and this app must be running on the same Windows machine. It does not monitor an iPhone, link Apple accounts, or provide Spotify's Listen Along feature.

## Run the Windows executable

Download **AppleMusicPresence.exe** from this repository's Releases page, or use the executable included in the release ZIP. Python is bundled, so no Python installation is needed. Create a Discord application as described below, paste its Application ID into the window, and choose **Start sharing**. This is an unsigned Windows x64 MVP.

For a preview without any account setup, run `AppleMusicPresence.exe --demo`. The executable also accepts the diagnostic and headless flags documented below.

## Setup from source

Use **Windows 10 version 1809 or newer / Windows 11** and **64-bit Python 3.12** with Tcl/Tk installed (the standard python.org installer includes it). The package permits Python 3.11–3.14; development and live Windows checks used 3.12. The Apple Music app itself may require a newer Windows version than the media API minimum.

1. Install/open the native Apple Music app and Discord desktop. Sign in normally in those apps.
2. Open the [Discord Developer Portal](https://discord.com/developers/applications), select **New Application**, and give it a display name such as **Apple Music**. Copy its **Application ID** from General Information. You do not need a bot, token, client secret, OAuth redirect, or account password.
3. Open PowerShell in this project's folder and run:

   ```powershell
   py -3.12 -m venv .venv
   .\.venv\Scripts\python.exe -m pip install .
   .\.venv\Scripts\python.exe -m apple_music_presence
   ```

   No environment activation or PowerShell execution-policy change is required.

4. Paste the Application ID into the window and choose **Start sharing**. Play a track in Apple Music. Enable activity sharing in Discord's Activity Privacy settings if it is disabled.
5. Optionally check **Find album art using Apple's public catalog** before starting. Stop the bridge to change settings. Closing the window stops sharing and exits; minimizing keeps it running.

After installation, `launch.cmd` in this folder opens the desktop window. The installed `apple-music-presence-desktop` launcher does the same. The app does not automatically start at sign-in.

Use `--start` to open the desktop and immediately start sharing with the saved settings. Normal launches wait for the Start button.

The Application ID is public, not a credential. The desktop saves it and your preferences in `%LOCALAPPDATA%\AppleMusicPresence\settings.json`. Headless options override saved preferences for that run. No listening history is saved.

## Preview and diagnostics

Preview without an Application ID, media access, or network requests:

```powershell
.\.venv\Scripts\python.exe -m apple_music_presence --demo
```

The preview simulates playback, pause, and resume. A short command-line smoke test is:

```powershell
.\.venv\Scripts\python.exe -m apple_music_presence --demo --headless --seconds 5
```

Read the actual local media sessions without connecting to Discord:

```powershell
.\.venv\Scripts\python.exe -m apple_music_presence --diagnose
```

Diagnostic output includes current song metadata and session IDs. If Apple's app changes its source ID, copy the full ID from diagnostics and launch with `--source-id 'EXACT_SOURCE_ID'`. An override deliberately selects that exact session; the default only matches native Apple Music. The package family observed during live testing was `AppleInc.AppleMusicWin_nzyj5cx40ttqa!App`.

Headless operation:

```powershell
.\.venv\Scripts\python.exe -m apple_music_presence --headless --client-id YOUR_APPLICATION_ID
.\.venv\Scripts\python.exe -m apple_music_presence --headless --client-id YOUR_APPLICATION_ID --artwork --country GB
```

Use Ctrl+C to stop and clear the activity. `--no-artwork` overrides saved opt-in. `--verbose` enables technical troubleshooting messages. Run one instance per Application ID.

## Behavior and limits

- Detection polls once per second. Discord updates are deduplicated and coalesced to at most one ordinary update every 15 seconds, following pypresence's recommendation; rapid skips, seeks, and resume may take up to 15 seconds to display. Clear-on-pause/stop is immediate after detection. Updates/clears every 30 seconds probe idle connection health.
- IPC operations time out after 5 seconds. Media requests time out after 5 seconds and get one immediate rediscovery attempt. Shutdown waits for an in-flight request to finish or time out before releasing resources.
- Windows must receive the metadata from Apple Music. Local files, radio, streams, and some app versions may omit album, duration, or position. Missing timelines show no fabricated Discord timer. Nonstandard playback rates also omit the Discord timer. Artist and album share a Discord text field and may be shortened to its 128-character limit.
- Album art is a best-effort catalog match using title **and** artist **and** album. Ambiguous, incomplete, region-specific, or mismatched results are omitted; remixes and live versions are not silently substituted. Artwork failure never blocks playback detection or sharing.
- Enabling artwork sends those three metadata fields to `itunes.apple.com`. The app uses returned HTTPS artwork and track URLs; it does not upload local artwork, access your Apple account, or download the image itself. Results are cached in memory with requests spaced at least 3.2 seconds apart. Returned artwork is typically 100×100 pixels. [Apple's Search API documentation and artwork terms](https://performance-partners.apple.com/search-api) describe permitted uses.
- Discord's profile layout determines whether timestamps appear as a progress bar or other time display. The activity name comes from your Discord application. Buttons may not be clickable when viewing your own activity; inspect from another account to check their appearance.
- An invalid Application ID or disabled Discord activity sharing cannot be repaired by reconnecting. Correct the ID or setting. Discord's browser version alone does not provide the desktop IPC connection used here.

## Architecture

```text
Tk desktop / CLI
       │
PresenceService ───── optional ItunesArtworkResolver
       │
       ├── MediaBackend protocol → WindowsMediaBackend → Windows.Media.Control
       └── DiscordRpc → pypresence → Discord desktop named pipe
```

`models.py` defines immutable `Track` / `MediaSnapshot` values and the `MediaBackend` protocol. `media.py` owns Windows COM/WinRT details on one worker thread. `presence.py` maps snapshots into Discord fields without accessing either external system. `service.py` handles polling, scheduling, artwork tasks, clearing, and reconnects. `ui.py` only receives status events through a queue; network/IPC work never runs on the Tk thread.

To add C++, implement `async read() -> MediaSnapshot` and `async close() -> None`, then substitute that adapter in `app.make_service()`. A pybind11 wrapper around C++/WinRT or a small process bridge can implement the same contract. Return position in seconds **as of** the snapshot's Unix `observed_at` time, a positive duration when known, and an explicit playing/paused/stopped state. Return no track when the session disappears. Keep native COM initialization and teardown on the backend thread. The Discord and UI layers need no changes. A C++ backend is an extension point, not included in this MVP.

## Verified API choices

Verified September 15, 2026 against primary documentation, release packages, and the installed Windows app:

| Component | Choice and evidence |
| --- | --- |
| Media detection | Microsoft's [GlobalSystemMediaTransportControlsSessionManager](https://learn.microsoft.com/en-us/uwp/api/windows.media.control.globalsystemmediatransportcontrolssessionmanager?view=winrt-26100) and [timeline properties](https://learn.microsoft.com/en-us/uwp/api/windows.media.control.globalsystemmediatransportcontrolssessiontimelineproperties?view=winrt-26100). |
| Python WinRT | Current modular [PyWinRT](https://pywinrt.readthedocs.io/en/stable/types.html) packages, pinned to [3.2.1](https://pypi.org/project/winrt-Windows.Media.Control/3.2.1/). These replace the older monolithic `winrt` / `winsdk` packages. |
| Discord transport | Discord's documented [RPC over IPC and SET_ACTIVITY](https://docs.discord.com/developers/topics/rpc), including Listening type `2`, with [pypresence 4.6.2](https://pypi.org/project/pypresence/4.6.2/). This community Python client wraps a currently documented protocol. The archived `discord-rpc` C SDK and deprecated WebSocket transport are not used. |
| Official SDK option | Discord's [Social SDK Rich Presence guide](https://docs.discord.com/developers/discord-social-sdk/development-guides/setting-rich-presence) documents unauthenticated desktop presence and is the official native SDK option for a future native integration. Python uses local IPC directly. |
| Image URLs / cadence | [pypresence field reference](https://qwertyquerty.github.io/pypresence/html/doc/presence.html), [update guidance](https://qwertyquerty.github.io/pypresence/html/info/quickstart.html), and Discord's [ActivityAssets reference](https://discord.com/developers/docs/social-sdk/classdiscordpp_1_1ActivityAssets.html). |
| Artwork lookup | Apple's [Search API](https://performance-partners.apple.com/search-api), tested against its live HTTPS endpoint. No MusicKit token is required for this public catalog search. |

`discord_rpc.py` contains narrowly scoped compatibility fixes for the pinned pypresence version: complete IPC frame reads, PING replies, asynchronous Windows pipe discovery, explicit `activity: null` clearing, and closing the transport without closing the application's asyncio loop. Tests exercise the real dependency's payload serialization. Review these fixes when upgrading pypresence.

## Development and testing

```powershell
.\.venv\Scripts\python.exe -m pip install -e .
.\.venv\Scripts\python.exe -m unittest discover -s tests -v
```

Tests cover matching and Windows session selection, timeline normalization, pause/stop, unavailable sessions, progress/seek changes, update coalescing, reconnect/backoff, cancellation, fragmented IPC frames, clearing, and optional artwork failures. They use fixtures for Discord and catalog responses; no test publishes a profile activity.

Live Windows detection, catalog matching, and profile display were verified during development. Discord acknowledged an activity update containing the cover as a proxied external image. To verify your setup: play a song, inspect your profile, pause/resume, seek, skip tracks, close/reopen Apple Music, close/reopen Discord, and finally stop the bridge. Allow the documented update interval after resume/skip/seek.

## Build a Windows executable

On Windows, after creating the project virtual environment:

```powershell
.\.venv\Scripts\python.exe -m pip install '.[build]'
.\.venv\Scripts\python.exe build_windows.py
```

The executable is written to `dist/AppleMusicPresence.exe`. [PyInstaller 6.22.3](https://pyinstaller.org/en/stable/usage.html) bundles Python, Tcl/Tk, and the WinRT modules. Its console is hidden when launched independently, while command-line output remains available when invoked from a terminal. See `THIRD_PARTY_NOTICES.txt` for runtime and build-tool license notices.
