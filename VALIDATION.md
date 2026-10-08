# Validation

Current source checked on Windows 11 x64 with Python 3.12.14.

| Check | Result |
| --- | --- |
| Automated suite | 165 offline tests passed, including release-safe artwork reuse, canonical album redirects, invalid local-file recovery, and small-screen layouts with long metadata. |
| Dependencies | `pip check` found no broken requirements. |
| Offline preview | Headless preview worked with invalid live preferences and left saved settings unchanged. |
| Live image checks | Apple's public catalog/CDN returned genuine 1024×1024 JPEG covers for After Hours, DATA, and the playing Fancy Some More? album. |
| Motion conversion | After Hours converted to a valid 768×768, 300-frame looping WebP at quality 85; 5,484,884 bytes, within the 8 MB upload limit. |
| Deluxe lookup | After Hours (Deluxe) and Nothing Compares (Bonus Track) resolved to a verified 1024×1024 shared catalog cover. Ambiguous release links and motion uploads were omitted. |

Earlier live checks verified native Apple Music metadata/progress, catalog artwork, and Discord acknowledgements for the Apple Music heading, artist status, and cover without a logo badge. Discord controls final image rendering; test that visually on your profile.

The released v0.1.1 executable passed build, preview, and media checks during its development. It predates the current source features and fixes; a new build is needed to include them.

## Native tray preview

Checked on the same Windows host with Visual Studio 2022, MSVC 14.44, and Windows SDK 10.0.22621.0.

| Check | Result |
| --- | --- |
| Native build | Clean x64 C++20 Release build; executable is 711,680 bytes, with a static C++ runtime and no Python dependency. |
| Native regressions | Core settings/presence, media, fake Discord pipes, artwork, and background-service checks passed. Assertions remain enabled in the test build. |
| Tray lifecycle | Offline window appeared, Save hid it to the tray, and timed exit succeeded. Preferences, motion cache, and startup registration were unchanged. |
| Native media | Read-only detection found the paused Apple Music track with title, artist, album, position, and duration present. |
| Native Discord | Live IPC handshake and explicit clear were acknowledged. A playing native profile card has not yet been visually verified. |
| Python regressions | All 165 existing tests still passed; dependencies remain consistent. |

In a nine-second offline, windowless preview, the native process used 10.78 MiB working memory and 1.62 MiB private memory. The equivalent Python preview used 30.18 MiB working memory and 15.64 MiB private memory across its launcher and interpreter. CPU time for both stayed below timer resolution during the four-second sample. These are short preview measurements, not a long-running live/artwork benchmark.

New motion-cover preparation and public Apple page/duplicate-JPEG fallback are not included in this native preview; see [native setup and limitations](native/README.md).
