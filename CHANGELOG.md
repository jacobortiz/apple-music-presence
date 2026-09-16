# Changelog

## 0.1.1

- Read the current native Apple Music track on Windows and display a Discord Listening activity with title, artist, album, and playback timestamps.
- Normalize Apple's combined `Artist — Album` metadata when the album field is empty, fixing artwork matching for affected tracks.
- Resolve optional public catalog artwork and add a Listen on Apple Music button.
- Show artwork lookup and publication status in the desktop window.
- Clear presence on pause, stop, missing media, and shutdown; reconnect after Discord or Apple Music restarts.
- Include an offline preview, media diagnostics, a Windows executable build script, and 59 automated tests.

Album-art lookup is opt-in. Windows desktop Apple Music and Discord must run on the same machine. A Discord Application ID is required for live sharing.
