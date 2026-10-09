# Apple Music Presence

A lightweight C++ tray app for Windows that shares the current Apple Music track on Discord.

- **Member list:** the current artist.
- **Profile:** “Listening to Apple Music,” song, artist, album cover, and playback progress. Hover the cover for the album name.
- Optional animated covers, with normal artwork as fallback.
- Clears on pause/stop and reconnects when Apple Music or Discord restarts.

## Setup

1. [Download the Windows x64 ZIP](https://github.com/jacobortiz/apple-music-presence/releases/tag/native-v0.1.0-preview.1) and extract it to a permanent folder. Requires Windows 10 1809 or newer; no compiler or runtime installation is needed.
2. Open Apple Music and Discord desktop. Create an **Apple Music** application in the [Discord Developer Portal](https://discord.com/developers/applications) and copy its **Application ID**. No bot or Discord token is needed.
3. Run **AppleMusicPresenceNative.exe**, enter the ID, optionally enable artwork, and choose **Save**. Enable activity sharing in Discord's Activity Privacy settings.

Closing settings keeps sharing. Open the music-note tray icon to change settings; right-click it to pause/resume or exit. **Start with Windows** is optional; disable it before moving the extracted folder. Run one presence app per Application ID.

Apple Music, Discord desktop, and this app must run on the same computer. iPhone-only playback and Spotify's Listen Along are unsupported. Discord controls the profile layout and heading icon; the app adds no logo badge over the cover.

## Animated covers

Enable **Prefer animated covers when available** and enter a public GitHub repository as `owner/repository`. To prepare new covers, close the app and double-click **Install-MotionSupport.cmd** in the ZIP once, then restart. It downloads and verifies an optional FFmpeg converter directly from its Windows build provider.

Configure write access through either:

- Your existing Git for Windows credential helper sign-in.
- A fine-grained token with **Contents: read and write**, supplied through `APPLE_MUSIC_PRESENCE_GITHUB_TOKEN`. Keep it out of settings and Git.

Normal art stays visible while an animation is found or prepared. The app first checks for the current motion cover on the repository's **motion-artwork** branch; verified public covers need neither FFmpeg nor GitHub sign-in. New covers are converted to looping WebP, published using your write access, and applied to the current song. Missing motion or preparation/upload failures keep normal art. FFmpeg runs only during conversion.

New songs are verified before reusing covers. Exact artist and full album titles keep editions separate; verified album IDs share animations across songs and guest artists. Normal covers request 1024px images; animations use 768px at quality 85, reducing frame rate or size only to fit the 8 MB limit.

Animations cache for seven days, no-motion results for one day, and temporary motion failures retry after a minute. Public artwork commits reveal album IDs, upload times, and the publishing account, using GitHub's no-reply email.

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

URLs must be public HTTPS without queries or fragments; images must end in `.webp`, `.gif`, or `.avif`. Restart after editing. See [bundled artwork provenance](artwork/README.md).

## Troubleshooting and privacy

- Windows media events trigger updates up to once per second when the rate budget allows. The app permits five attempts per twenty seconds, reserving one for clearing activity. Rapid changes replace pending state; Discord can take longer to display them. Progress advances from timestamps without per-second pushes.
- Artwork checks the exact song, then the exact artist and album edition, then public Apple Music pages. Uncertain releases omit the album link and animation. Failed lookups retry during playback.
- Missing timeline data means no progress bar. Discord controls animation playback and button visibility; check buttons from another account.
- Artwork opt-in sends track tags to Apple's public services. Motion opt-in uploads metadata-free public covers to GitHub. No local music files, browser cookies, Apple passwords, Discord tokens, telemetry, or per-song history are collected.
- Settings, custom mappings, and the bounded motion cache stay in `%LOCALAPPDATA%\AppleMusicPresence`. Cached albums and public artwork commits can reveal prepared albums. Credentials stay in memory and reach only GitHub's API.
- Existing settings and verified artwork caches from earlier versions remain compatible.

## Development

The app uses C++20, C++/WinRT media sessions, and Discord's documented local RPC. Artwork and motion work run off the UI thread. JSON is vendored under MIT; FFmpeg is an optional separate executable.

See [native build, tests, and packaging](native/README.md), [third-party notices](THIRD_PARTY_NOTICES.txt), [validation](VALIDATION.md), and [changelog](CHANGELOG.md).
