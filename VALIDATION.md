# Validation

Current source checked on Windows 11 x64 with Python 3.12.14.

| Check | Result |
| --- | --- |
| Automated suite | 136 offline tests passed, including higher-resolution artwork, motion size fallback, cache upgrades, cancellation, and concurrent settings saves. |
| Dependencies | `pip check` found no broken requirements. |
| Offline preview | Headless preview worked with invalid live preferences and left saved settings unchanged. |
| Live image checks | Apple's public catalog/CDN returned genuine 1024×1024 JPEG covers for After Hours, DATA, and the playing Fancy Some More? album. |
| Motion conversion | After Hours converted to a valid 768×768, 300-frame looping WebP at quality 85; 5,484,884 bytes, within the 8 MB upload limit. |

Earlier live checks verified native Apple Music metadata/progress, catalog artwork, and Discord acknowledgements for the Apple Music heading, artist status, and cover without a logo badge. Discord controls final image rendering; test that visually on your profile.

The released v0.1.1 executable passed build, preview, and media checks during its development. It predates the current source features and fixes; a new build is needed to include them.
