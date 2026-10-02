# Validation

Current source checked on Windows 11 x64 with Python 3.12.14.

| Check | Result |
| --- | --- |
| Automated suite | 130 offline tests passed, including artwork failure recovery, IPC error matching, cancellation, and concurrent settings saves. |
| Dependencies | `pip check` found no broken requirements. |
| Offline preview | Headless preview worked with invalid live preferences and left saved settings unchanged. |

Earlier live checks verified native Apple Music metadata/progress, catalog artwork, and Discord acknowledgements for the Apple Music heading, artist status, and cover without a logo badge. Discord controls final image rendering; test that visually on your profile.

The released v0.1.1 executable passed build, preview, and media checks during its development. It predates the current source features and fixes; a new build is needed to include them.
