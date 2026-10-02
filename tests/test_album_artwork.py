import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import AsyncMock

from apple_music_presence.album_artwork import MappedArtworkResolver, _entries
from apple_music_presence.artwork import Artwork
from apple_music_presence.models import MediaSnapshot, PlaybackState, Track
from apple_music_presence.presence import build_presence


def mapping(**changes):
    return {"artist": "Artist", "album": "Album",
            "image_url": "https://example.org/covers/album.webp",
            "album_url": "https://music.apple.com/us/album/album/123", **changes}


class AlbumArtworkTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.static = Artwork("https://is1-ssl.mzstatic.com/static.jpg",
                              "https://music.apple.com/us/song/123")
        self.catalog = AsyncMock()
        self.catalog.resolve.return_value = self.static

    async def test_all_songs_on_exact_album_share_animation_without_catalog_calls(self):
        resolver = MappedArtworkResolver(self.catalog, [mapping()])
        first = await resolver.resolve("First", "Artist", "Album")
        second = await resolver.resolve("Second", " ARTIST ", " ALBUM ")
        self.assertEqual(first, second)
        self.assertTrue(first.animated)
        self.catalog.resolve.assert_not_awaited()
        payload = build_presence(MediaSnapshot(Track("Second", "Artist", "Album"),
                                              PlaybackState.PLAYING), 1000, second)
        self.assertEqual(payload["large_image"], mapping()["image_url"])
        self.assertEqual(payload["buttons"][0]["url"], mapping()["album_url"])
        self.assertEqual(payload["details"], "Second")

    async def test_unmapped_artist_or_edition_uses_normal_catalog(self):
        resolver = MappedArtworkResolver(self.catalog, [mapping()])
        for artist, album in (("Another Artist", "Album"), ("Artist", "Album (Deluxe)"),
                              ("", "Album"), ("Artist", "")):
            with self.subTest(artist=artist, album=album):
                self.assertEqual(await resolver.resolve("Song", artist, album), self.static)
                self.catalog.resolve.assert_awaited_with("Song", artist, album)

    async def test_invalid_urls_are_ignored_without_weakening_catalog_url_rules(self):
        invalid = ("http://example.org/cover.webp", "file:///cover.webp",
                   "https://user:password@example.org/cover.webp", "https://example.org/cover.mp4",
                   "https://example.org:bad/cover.webp", "https://example.org/cover.webp#fragment",
                   "https://example.org/cover.webp?token=secret-fixture",
                   "https://example.org/cover.webp\n")
        for url in invalid:
            with self.subTest(url=url):
                resolver = MappedArtworkResolver(self.catalog, [mapping(image_url=url)])
                self.assertEqual(await resolver.resolve("Song", "Artist", "Album"), self.static)
        resolver = MappedArtworkResolver(self.catalog, [mapping(album_url="https://example.org/album")])
        self.assertEqual(await resolver.resolve("Song", "Artist", "Album"), self.static)
        resolver = MappedArtworkResolver(self.catalog, [mapping(album_url="https://music.apple.com/us/album/album/123?token=secret-fixture")])
        self.assertEqual(await resolver.resolve("Song", "Artist", "Album"), self.static)

    async def test_conflicting_album_mappings_fall_back_instead_of_guessing(self):
        resolver = MappedArtworkResolver(self.catalog, [mapping(),
            mapping(image_url="https://example.org/another.webp")])
        self.assertEqual(await resolver.resolve("Song", "Artist", "Album"), self.static)

    async def test_duplicate_identical_mappings_are_accepted(self):
        resolver = MappedArtworkResolver(self.catalog, [mapping(), mapping()])
        self.assertTrue((await resolver.resolve("Song", "Artist", "Album")).animated)

    async def test_malformed_entries_are_ignored(self):
        resolver = MappedArtworkResolver(self.catalog, [None, 12, {}, mapping(artist=1),
                                                       mapping(album=""), mapping(artist="!"),
                                                       mapping(album="A" * 513)])
        self.assertEqual(await resolver.resolve("Song", "Artist", "Album"), self.static)

    async def test_local_map_loads_and_invalid_json_falls_back(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "album_artwork.json"
            path.write_text(json.dumps({"version": 1, "albums": [mapping()]}), encoding="utf-8")
            resolver = MappedArtworkResolver.load(self.catalog, path)
            self.assertTrue((await resolver.resolve("Song", "Artist", "Album")).animated)
            path.write_text("broken json", encoding="utf-8")
            resolver = MappedArtworkResolver.load(self.catalog, path)
            self.assertEqual(await resolver.resolve("Song", "Artist", "Album"), self.static)

    async def test_bundled_cover_reaches_discord_without_catalog_lookup(self):
        resolver = MappedArtworkResolver.load(self.catalog)
        cover = await resolver.resolve("Blinding Lights", "The Weeknd", "After Hours")
        self.assertTrue(cover.animated)
        self.catalog.resolve.assert_not_awaited()
        payload = build_presence(MediaSnapshot(Track("Blinding Lights", "The Weeknd", "After Hours"),
                                              PlaybackState.PLAYING), 1000, cover)
        self.assertEqual(payload["large_image"],
            "https://raw.githubusercontent.com/jacobortiz/apple-music-presence/main/artwork/after-hours-hq.webp")
        self.assertEqual(payload["buttons"][0]["url"],
            "https://music.apple.com/us/album/after-hours/1499378108")

    def test_map_schema_and_size_are_checked(self):
        for value in ("[]", '{"version": 2, "albums": []}', '{"version": 1, "albums": {}}',
                      json.dumps({"version": 1, "albums": [{}] * 257}), " " * (128 * 1024 + 1)):
            with self.subTest(value=value[:80]):
                with self.assertRaises(ValueError):
                    _entries(value)
