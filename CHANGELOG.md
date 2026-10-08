# Changelog

## Unreleased

- Replace the Python app with the C++20 tray app; remove Python source, tests, dependencies, and build files.
- Add event-driven media detection, Discord IPC, reconnects, pause/stop clearing, and optional Windows startup.
- Show the current artist in the member list while retaining the Apple Music profile heading; send changed activities at most every five seconds.
- Match exact songs, artists, and album editions, with album/public-page fallback and strict duplicate-cover comparison.
- Prepare and host animated covers in the background, updating the current song while preserving normal art on failure.
- Request 1024px normal artwork and 768px motion covers, with bounded conversion, caching, downloads, and cancellation.
- Package a portable x64 ZIP with a verified optional FFmpeg installer, focused notices, and private-path checks.

## 0.1.1

Historical Python release; replaced by the C++ app above.

- Read the current native Apple Music track on Windows and display a Discord Listening activity with title, artist, album, and playback timestamps.
- Normalize Apple's combined `Artist — Album` metadata when the album field is empty, fixing artwork matching for affected tracks.
- Resolve optional public catalog artwork and add a Listen on Apple Music button.
- Show artwork lookup and publication status in the desktop window.
- Clear presence on pause, stop, missing media, and shutdown; reconnect after Discord or Apple Music restarts.
- Include an offline preview, media diagnostics, a Windows executable build script, and 59 automated tests.

Album-art lookup is opt-in. Windows desktop Apple Music and Discord must run on the same machine. A Discord Application ID is required for live sharing.
