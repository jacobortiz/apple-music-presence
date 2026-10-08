# Validation

The native Windows app was checked on Windows 11 x64 with Visual Studio 2022, MSVC 14.44, and Windows SDK 10.0.22621.0.

| Check | Result |
| --- | --- |
| Build | Clean x64 C++20 Release build with a static runtime. No private source paths in the executable; test assertions remain enabled. |
| Automated tests | Eleven groups passed, covering media/IPC, exact song/album/page matching, duplicate JPEGs, bounded HLS/WebP, credentials and hosting, same-song upgrades, reconnects, and cancellation. |
| Album fallback | Fixtures cover unmatched titles, album caching, strict artist/edition matching, ambiguity, temporary failures, and cancellation. A live public Apple lookup found the exact album after an intentionally unmatched song title. |
| Motion | Live Apple motion discovery, metadata-free WebP conversion, public GitHub publication, and immutable image verification passed. |
| Public cover reuse | Offline fixtures verify unauthenticated reuse, missing/rate-limited branches, malformed/oversized images, and cancellation. Current live discovery succeeded; reuse returned a verified miss because the public `motion-artwork` branch is absent. |
| Portable package | Six public files only; executable matches the tested build, without private profile/PDB paths. Optional FFmpeg 9.0.2 setup passed download, hash/encoder checks, reinstall, and corrupt-archive cleanup on Windows PowerShell 5.1. Live conversion produced a valid 5,508,920-byte animated WebP. |
| Tray lifecycle | Offline window appeared, Save hid it to the tray, and timed exit succeeded. Preferences, motion cache, and startup registration were unchanged. |
| Media | Read-only detection found a paused Apple Music track with title, artist, album, position, and duration present. |
| Discord | Live IPC handshake and explicit clear were acknowledged. A playing native profile card has not yet been visually verified. |

A nine-second offline, windowless preview used 10.78 MiB working memory and 1.62 MiB private memory. CPU time stayed below timer resolution during the four-second sample. This is a short preview measurement, not a long-running live/artwork benchmark.

Discord controls final profile rendering, image caching, and animation playback. Verify those visually on your profile. See [setup](README.md#setup) and [build/test instructions](native/README.md).

The cleanup build passed all eleven test groups, the tray check without periodic refreshes, and exact-content/private-path checks of the extracted portable package. Startup save now reconciles the current executable with the registry; this was reviewed without changing the user's startup registration.
