# Hosted motion covers

## The Weeknd — After Hours

- Album source: [Apple Music](https://music.apple.com/us/album/after-hours/1499378108).
- Original square motion stream, linked by Apple's public album page:
  `https://mvod.itunes.apple.com/itunes-assets/HLSMusic126/v4/0e/66/6b/0e666b3f-39d8-0339-0708-782988ac96a5/P359476169_default.m3u8`
- Obtained October 1, 2026. Converted from Apple's 408×408 SDR rendition to a 384×384, 15 fps animated WebP with FFmpeg 7.1 (`libwebp_anim`, quality 65, infinite loop).
- Full loop: 300 frames, approximately 20 seconds; 996,082 bytes.
- SHA-256: `36b1d6198799d3a0abff1b3cb2b8d0df0b92924571f9334f7dc18ccf40f2ffe2`.
- Public image: [after-hours.webp](https://raw.githubusercontent.com/jacobortiz/apple-music-presence/main/artwork/after-hours.webp).

The cover is third-party album artwork; its copyright remains with its original rights holders. It is not covered by this repository's code license.

The app passes the public image URL to Discord. It does not download this file at runtime. Add exact artist/album entries to `src/apple_music_presence/album_artwork.json` to include further covers, and use a new filename when replacing a cover to avoid Discord's image cache retaining an old version.
