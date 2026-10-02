# Apple Music Presence

A Python desktop MVP that shares the current track from the **native Windows Apple Music app** as a Discord **Listening** activity. Start/Stop controls and a live track preview are included. Media detection is isolated behind a small interface for a future C++ backend.

## What works

- Title, artist, and album from Windows media sessions; other players and browser sessions are ignored by default.
- Discord's member list follows the artist: **Listening to The Weeknd**, for example. Opening the profile shows **Listening to Apple Music**, the song and artist, and an Apple Music logo badge beside the album cover. The album name is the cover's hover text. Missing artist metadata falls back to **Apple Music**.
- Playback progress derived from Windows' position, duration, and last-update timestamp. Seeking and track changes update Discord's timestamps.
- Pause, stop, missing metadata, and disappearing sessions clear the activity. Resume publishes the current track again.
- Discord and Apple Music can be closed/reopened. The bridge rediscovers media sessions and retries Discord with exponential backoff up to 30 seconds.
- Optional album art and a **Listen on Apple Music** button from Apple's public catalog. Art lookup is off by default.
- Automatically prefers motion covers for matched Apple Music albums when enabled. Normal artwork appears while a new animation is prepared, and remains when no motion cover is available. Converted WebPs are hosted in your chosen public GitHub repository and cached by album across songs and app restarts.
- Bundled/personal album mappings can supply an existing animated WebP/GIF/AVIF immediately, without downloading or uploading another cover.
- Fast artwork lookups (including covers cached in memory) join the first track update: the app allows up to 0.75 seconds for a cover before publishing text alone. Slow lookups continue in the background, and completed covers wake the polling loop early. Later artwork updates still respect the five-second send interval; Discord controls image loading on profiles.
- Failed artwork lookups retry while the same song plays, with waits of 10, 20, 40, then 60 seconds. Temporary network failures are cached for only 10 seconds; confirmed catalog misses retain their 10-minute cache to avoid repeated searches for unavailable recordings. Retries pause when playback is paused, and changing tracks resets the retry delay.
- Handles Apple Music's combined `Artist — Album` Windows metadata when its album field is empty. The desktop reports whether artwork is off, being looked up, unmatched, or sent to Discord.
- An offline preview and a read-only media diagnostic command.

This is a local desktop bridge. Discord desktop, Apple Music, and this app must be running on the same Windows machine. It does not monitor an iPhone, link Apple accounts, or provide Spotify's Listen Along feature.

## Run the Windows executable

Download **AppleMusicPresence.exe** from this repository's Releases page, or use the executable included in the release ZIP. Python is bundled, so no Python installation is needed. Create a Discord application as described below, paste its Application ID into the window, and choose **Start sharing**. This is an unsigned Windows x64 MVP.

The existing v0.1.1 release predates automatic motion covers and the faster artwork updates. Use the current source or build a new executable for these features.

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
5. Optionally check **Use album artwork (animated covers where available)** before starting. Stop the bridge to change settings. Closing the window stops sharing and exits; minimizing keeps it running.
6. For automatic motion artwork, check **Prefer animated covers whenever available**, enter a public GitHub host as `owner/repository`, and configure GitHub write access as described under [Animated album covers](#animated-album-covers). Normal artwork remains available if motion preparation or upload fails.

After installation, `launch.cmd` in this folder opens the desktop window. The installed `apple-music-presence-desktop` launcher does the same. The app does not automatically start at sign-in.

Use `--start` to open the desktop and immediately start sharing with the saved settings. Normal launches wait for the Start button.

The Application ID is public, not a credential. The desktop saves it and your preferences in `%LOCALAPPDATA%\AppleMusicPresence\settings.json`. Headless options override saved preferences for that run. No per-song listening history is saved. The album cache and public artwork commits identify albums the app prepared, as described below.

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

- Detection polls once per second. Changed tracks, seeks, resume, and completed artwork lookups are sent at most once every **5 seconds**; rapid changes are coalesced to the latest track. This replaces the previous 15-second app delay. The first track is sent immediately after detection/connection. Discord may still buffer visible profile updates, so five seconds is our send interval, not a guaranteed display delay. Pypresence's quickstart retains 15-second guidance; the faster interval was checked against local Discord acknowledgements. Clear-on-pause/stop is immediate after detection. Updates/clears every 30 seconds probe idle connection health.
- IPC operations time out after 5 seconds. Media requests time out after 5 seconds and get one immediate rediscovery attempt. Shutdown waits for an in-flight request to finish or time out before releasing resources.
- Windows must receive the metadata from Apple Music. Local files, radio, streams, and some app versions may omit album, duration, or position. Missing timelines show no fabricated Discord timer. Nonstandard playback rates also omit the Discord timer. Artist and album share a Discord text field and may be shortened to its 128-character limit.
- Album art is a best-effort exact catalog match. If iTunes search omits a recording, the app searches Apple's public Music page and verifies the exact title, artist, and album against the album's own track list. This also caches the album's other tracks, including guest artists. Automatic motion caches use the verified Apple album ID, so collaborations on the same album share one animation. Ambiguous, incomplete, region-specific, or mismatched results are omitted; edition names are preserved. Artwork failure never blocks playback detection or sharing.
- Enabling artwork sends metadata to `itunes.apple.com` and, when needed, the public `music.apple.com` search and album pages. This fallback reads public page data without cookies or account access; Apple page changes can temporarily prevent matching. Automatic motion also fetches the matched album's Apple-hosted video. It converts public video to a WebP and uploads it to the configured public GitHub repository. It never uploads your local music or accesses your Apple account. Requests to each catalog service are spaced at least 3.2 seconds apart; ordinary covers are typically 100×100 pixels from iTunes or 300×300 from the public Music page. [Apple's Search API documentation and artwork terms](https://performance-partners.apple.com/search-api) describe permitted uses.
- Discord's profile layout determines whether timestamps appear as a progress bar or other time display. The profile activity name is **Apple Music**; the member list uses the artist from `state` with `status_display_type: 1`. The application icon beside the profile header comes from **General Information → App Icon** in the Discord Developer Portal; the bridge also supplies Apple's public app icon as the small artwork badge. This remains a third-party Rich Presence card; Spotify's special layout and Listen Along controls are not reproduced. Buttons may not be clickable when viewing your own activity; inspect from another account to check their appearance.
- An invalid Application ID or disabled Discord activity sharing cannot be repaired by reconnecting. Correct the ID or setting. Discord's browser version alone does not provide the desktop IPC connection used here.

## Architecture

```text
Tk desktop / CLI
       │
PresenceService ───── optional MappedArtworkResolver
       │                        └── AutomaticArtworkResolver → ItunesArtworkResolver
       │                              ├── public Apple album page → local FFmpeg
       │                              └── GithubArtworkHost → public animated WebP
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
| Artist status / app branding | Discord supports [separate member-list status text](https://docs.discord.com/developers/discord-social-sdk/development-guides/setting-rich-presence#configuring-status-text). The bridge sends `name: Apple Music`, `state: artist`, and `status_display_type: 1` (State), plus a small Apple Music image badge. Pypresence 4.6.2 serializes all these fields. |
| Official SDK option | Discord's [Social SDK Rich Presence guide](https://docs.discord.com/developers/discord-social-sdk/development-guides/setting-rich-presence) documents unauthenticated desktop presence and is the official native SDK option for a future native integration. Python uses local IPC directly. |
| Image URLs / cadence | [pypresence field reference](https://qwertyquerty.github.io/pypresence/html/doc/presence.html), [update guidance](https://qwertyquerty.github.io/pypresence/html/info/quickstart.html), and Discord's [ActivityAssets reference](https://discord.com/developers/docs/social-sdk/classdiscordpp_1_1ActivityAssets.html). |
| Artwork lookup | Apple's [Search API](https://performance-partners.apple.com/search-api), tested against its live HTTPS endpoint. No MusicKit token is required for this public catalog search. |

`discord_rpc.py` contains narrowly scoped compatibility fixes for the pinned pypresence version: complete IPC frame reads, PING replies, asynchronous Windows pipe discovery, explicit `activity: null` clearing, and closing the transport without closing the application's asyncio loop. Tests exercise the real dependency's payload serialization. Review these fixes when upgrading pypresence.

## Animated album covers

Enable **Prefer animated covers whenever available** and enter a public GitHub repository as `owner/repository`. The app checks each catalog-matched album's public Apple Music page for square motion artwork. It keeps normal artwork visible while preparing a 384×384, 15 fps looping WebP, then upgrades the same song's presence without waiting for the next song. Albums with no motion cover keep normal artwork.

New covers are uploaded to the **motion-artwork** branch in your chosen repository, under `artwork/motion/`. This branch is created automatically; app source on the main branch is unaffected. Published URLs include their commit SHA to avoid stale negative image caching. Uploaded album identifiers and covers are public. Conversion runs locally, one album at a time. No-motion results are cached for a day, animations for seven days, and temporary failures retry after a minute. The local cache is `%LOCALAPPDATA%\AppleMusicPresence\motion_cache.json`; it holds artwork results, not song history.

Public artwork commits also show upload times and the GitHub account that published them. This can reveal albums the app prepared; it is not a private hosting service. New automatic uploads explicitly use GitHub's no-reply commit email. Older commits keep their original author metadata unless repository history is separately rewritten.

For GitHub access, use either:

- An existing Git for Windows sign-in with repository **Contents: write** permission. The app retrieves it through Git's credential helper without opening prompts or saving the token in its settings.
- A fine-grained GitHub token restricted to your artwork repository with **Contents: read and write**, supplied only through the `APPLE_MUSIC_PRESENCE_GITHUB_TOKEN` environment variable. Do not paste it into the Application ID field or commit it to the repository.

GitHub access is needed only to host new animations. If it is missing or expires, the app keeps normal covers and reports the upload problem. The converter accepts finite, unencrypted square SDR Apple streams of at most 60 seconds, limits video downloads to 24 MB, and limits WebPs to 8 MB. Apple page format changes, unavailable catalog entries, and unsupported streams safely fall back to normal artwork. Discord controls visible image loading and animation playback.

Headless example (after configuring GitHub access):

```powershell
.\.venv\Scripts\python.exe -m apple_music_presence --headless --motion-artwork --artwork-repository owner/repository
```

The ordinary artwork option also uses bundled mappings in `src/apple_music_presence/album_artwork.json`. Mapped albums send their hosted animation URL directly to Discord without fetching or uploading again. The UI reports **Animated album art: sent to Discord** when an animated cover has been published.

The first bundled cover is **The Weeknd — After Hours**: a [20-second looping WebP](https://raw.githubusercontent.com/jacobortiz/apple-music-presence/main/artwork/after-hours.webp), hosted in this repository. Play any song whose Windows metadata reports that artist and album to use it. Other editions go through automatic discovery when enabled. See [artwork provenance and conversion details](artwork/README.md).

For a personal mapping without editing the source, create `%LOCALAPPDATA%\AppleMusicPresence\album_artwork.json` with this structure and replace the example values:

```json
{
  "version": 1,
  "albums": [
    {
      "artist": "Exact artist name",
      "album": "Exact album title",
      "image_url": "https://your-public-host.example/album.webp",
      "album_url": "https://music.apple.com/us/album/album-name/123456789"
    }
  ]
}
```

Use a direct HTTPS image URL that works without signing in, ending in `.webp`, `.gif`, or `.avif`. Image and album URLs in personal mappings must have no query string or fragment, preventing signed links or embedded access tokens from being shared with Discord. The Apple Music album URL supplies the **Listen on Apple Music** button for every song in that album. Matching normalizes punctuation, whitespace, and case but preserves edition names, featured artists, and other words. Conflicting entries are ignored rather than choosing an arbitrary cover. Restart the bridge after changing the map.

Network downloads are restricted to the required Apple and GitHub HTTPS hosts. Redirects are blocked, and GitHub authorization headers are rejected outside `api.github.com`. The converter receives a minimal operating-system environment, not account tokens; it strips metadata, and uploads reject EXIF/XMP or other non-image chunks. Settings, caches, credentials, private keys, working logs, and build outputs are excluded from Git. The app does not collect browser cookies, Apple passwords, Discord tokens, local music files, or telemetry.

Discord supports [animated external image URLs](https://docs.discord.com/developers/events/gateway-events#activity-object-activity-asset-image); video streams and MP4 URLs cannot be used as cover images. Automatic discovery reads public album pages without a MusicKit token. [imageio-ffmpeg 0.6.0](https://pypi.org/project/imageio-ffmpeg/0.6.0/) supplies the local converter; [GitHub's Contents API](https://docs.github.com/en/rest/repos/contents) publishes the converted file.

## Development and testing

```powershell
.\.venv\Scripts\python.exe -m pip install -e .
.\.venv\Scripts\python.exe -m unittest discover -s tests -v
```

Tests cover matching and Windows session selection, timeline normalization, pause/stop, unavailable sessions, progress/seek changes, update coalescing, reconnect/backoff, cancellation, fragmented IPC frames, clearing, and optional artwork failures. They use fixtures for Discord and catalog responses; no test publishes a profile activity.

Motion tests cover album matching, same-song upgrades, cross-song/restart caching, static fallback, upload failures and retries, cancellation, HLS byte ranges, host restrictions, image validation, and keeping credentials off public image requests. Live automatic conversion/upload was verified for After Hours, and the no-motion fallback for GHOST DATA's Magical Metamorphosis. Visible animated profile rendering still needs to be checked in Discord.

Live Windows detection, catalog matching, and profile display were verified during development. Discord acknowledged an activity update containing the cover as a proxied external image. To verify your setup: play a song, inspect your profile, pause/resume, seek, skip tracks, close/reopen Apple Music, close/reopen Discord, and finally stop the bridge. Allow the documented update interval after resume/skip/seek.

## Build a Windows executable

On Windows, after creating the project virtual environment:

```powershell
.\.venv\Scripts\python.exe -m pip install '.[build]'
.\.venv\Scripts\python.exe build_windows.py
```

The executable is written to `dist/AppleMusicPresence.exe`. [PyInstaller 6.22.3](https://pyinstaller.org/en/stable/usage.html) bundles Python, Tcl/Tk, and the WinRT modules. Its console is hidden when launched independently, while command-line output remains available when invoked from a terminal. See `THIRD_PARTY_NOTICES.txt` for runtime and build-tool license notices.
