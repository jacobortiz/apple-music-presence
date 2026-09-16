# MVP validation

Validated on Windows 11 x64 with Python 3.12.14.

| Check | Result |
| --- | --- |
| Automated suite | 59 tests passed, including the native Apple Music combined artist/album metadata regression and asynchronous artwork publication/status. |
| Installed package | Editable installation and command-line entry point worked with the pinned runtime dependencies. |
| Native desktop | Tk window layout, offline preview worker, and graceful window shutdown passed. |
| Windows media API | Read the actual native Apple Music session and its metadata/position/duration; ignored an unrelated browser media session. Playback was not modified. |
| Public catalog | Live HTTPS lookup returned a matching public artwork URL and Apple Music track link. |
| Windows executable | PyInstaller 6.22.3 build succeeded. Executable passed headless demo, desktop demo, and live read-only media diagnostics. |
| Live Discord profile | Live profile display was verified during development. After the artwork fix, a real SET_ACTIVITY command was acknowledged by Discord, which returned the album image as a proxied `mp:external/...` asset. The image's final visual rendering was not independently verified. |

The executable is a local unsigned x64 build. The README documents setup and the remaining manual Discord profile checks.

Version 0.1.1 handles the Windows metadata shape `artist="Artist — Album", album_title=""`. It separates the artist and album before matching, preserves explicit album metadata and other players, and exposes artwork lookup status in the desktop. Artwork lookup defaults to off and can be enabled in the desktop settings.
