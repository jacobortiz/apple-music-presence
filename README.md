# Apple Music Presence

A Windows desktop app that shares the current Apple Music track on Discord.

A lightweight [C++ tray app](native/README.md) is also available. It runs without Python and supports sharing, exact artwork matching, and automatic animated covers. FFmpeg runs only when a new animation needs preparation.

- **Member list:** the current artist.
- **Profile:** “Listening to Apple Music,” song, artist, album cover, and playback progress. Hover the cover for the album name.
- Optional animated covers, with normal artwork as fallback.
- Clears on pause/stop and reconnects when Discord or Apple Music restarts.

Apple Music, Discord desktop, and this app must run on the same computer. iPhone-only playback and Spotify's Listen Along are unsupported. Discord controls the profile layout and heading icon; Rich Presence cannot place a custom monochrome Apple logo beside the heading. The app sends no logo badge over the cover.

## Setup

Use Windows 10/11 and 64-bit Python 3.12 with Tcl/Tk (included by python.org).

1. Open the native Apple Music app and Discord desktop.
2. Create an **Apple Music** application in the [Discord Developer Portal](https://discord.com/developers/applications). Copy its **Application ID**; no bot or Discord token is needed.
3. Open PowerShell in this folder:

   ```powershell
   py -3.12 -m venv .venv
   .\.venv\Scripts\python.exe -m pip install -e .
   .\.venv\Scripts\python.exe -m apple_music_presence
   ```

4. Paste the ID, optionally enable artwork, and choose **Start sharing**. Enable activity sharing in Discord's Activity Privacy settings.

After installation, use `launch.cmd`. Add `--start` to the Python command to share immediately with saved settings. Closing the window stops sharing; minimizing keeps it running.

The existing **v0.1.1 executable is older than these features**. Use current source or build a new executable below.

## Animated covers

Enable **Prefer animated covers whenever available** and enter a public GitHub repository as `owner/repository`. Configure write access through either:

- Your existing Git for Windows credential helper sign-in.
- A fine-grained token with **Contents: read and write**, supplied through `APPLE_MUSIC_PRESENCE_GITHUB_TOKEN`. Keep it out of settings and Git.

Normal art stays visible while a new animation is prepared. When Apple provides a supported motion cover, the app converts it to a looping WebP, uploads it to the repository's **motion-artwork** branch, and updates the same song. Missing motion covers or upload failures keep normal art. New songs are checked before reusing covers; verified album IDs share animations across songs and guest artists.

Normal covers request 1024×1024 images. Motion covers use higher-resolution sources and 768×768 WebP at quality 85, reducing frame rate or size only when needed to fit the 8 MB limit. Older cached animations regenerate automatically with new URLs.

Animations are cached for seven days, no-motion results for one day, and temporary motion failures retry after a minute. Public commits reveal album IDs, upload times, and the publishing account. New uploads use GitHub's no-reply email; older commit metadata is unchanged.

For an already-hosted cover, create `%LOCALAPPDATA%\AppleMusicPresence\album_artwork.json`:

```json
{
  "version": 1,
  "albums": [{
    "artist": "Exact artist name",
    "album": "Exact album title",
    "image_url": "https://example.org/cover.webp",
    "album_url": "https://music.apple.com/us/album/album-name/123456789"
  }]
}
```

URLs must be public HTTPS with no query or fragment; images must end in `.webp`, `.gif`, or `.avif`. Matching preserves album editions and artist names. Restart after editing. [Bundled artwork provenance](artwork/README.md).

## Troubleshooting and privacy

- Detection polls every second; changed activities are sent at most every five seconds. Discord may take longer to display them.
- Artwork uses exact Apple catalog matches, with public Music pages as fallback. Duplicate releases can share verified identical normal covers; an uncertain release omits the album link and animation. Missing tags, different covers, network failures, and Apple page changes can prevent artwork. Failed lookups retry during playback.
- No timeline means no progress bar. Discord controls animation playback and button visibility; check buttons from another account.
- Use `--demo` for an offline preview, `--diagnose` to inspect local media metadata, or `--verbose` for troubleshooting. `--source-id` selects an exact media session if needed. Run one instance per Application ID.
- Artwork opt-in sends song metadata to Apple's public services. Motion opt-in uploads public cover images to GitHub. No local music, browser cookies, Apple passwords, Discord tokens, or telemetry are collected.
- Preferences, motion cache, and custom mappings stay in `%LOCALAPPDATA%\AppleMusicPresence`. No per-song history is saved; cached albums and public artwork commits can reveal prepared albums. Credentials, settings, caches, and logs are excluded from Git.
- Downloads are limited to approved Apple/GitHub hosts. Only verified same-album Apple page redirects are allowed. GitHub credentials stay on its API; uploads reject embedded image metadata.

Headless example (uses saved settings):

```powershell
.\.venv\Scripts\python.exe -m apple_music_presence --headless --artwork
```

Use `--client-id YOUR_APPLICATION_ID` to override the saved ID, `--country GB` to change storefront, or `--no-artwork` to disable artwork. Ctrl+C clears the activity and exits.

## Development

The backend-neutral `Track` / `MediaSnapshot` contract separates Windows detection from Discord. A future C++ adapter can implement `MediaBackend.read()` and `close()` and replace the backend in `app.make_service()`. Network and IPC work run off the UI thread.

Detection uses Microsoft's [Windows media sessions](https://learn.microsoft.com/en-us/uwp/api/windows.media.control.globalsystemmediatransportcontrolssessionmanager) via PyWinRT. Discord uses [local RPC](https://docs.discord.com/developers/topics/rpc) through pypresence; [status display type](https://docs.discord.com/developers/discord-social-sdk/development-guides/setting-rich-presence#configuring-status-text) keeps the artist in the member list. Artwork uses [Apple's Search API](https://performance-partners.apple.com/search-api) and public album pages; imageio-ffmpeg converts motion covers and [GitHub's Contents API](https://docs.github.com/en/rest/repos/contents) hosts them. Dependencies are pinned in `pyproject.toml`.

```powershell
.\.venv\Scripts\python.exe -m unittest discover -s tests -v
.\.venv\Scripts\python.exe -m pip install '.[build]'
.\.venv\Scripts\python.exe build_windows.py
```

The build produces `dist/AppleMusicPresence.exe`, bundling Python and Tcl/Tk. See `THIRD_PARTY_NOTICES.txt` for licenses. Tests use offline fixtures; live verification covers Windows detection, Discord acknowledgements, and artwork lookup. Check final profile appearance in Discord.
