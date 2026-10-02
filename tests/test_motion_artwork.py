import asyncio
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import AsyncMock, patch
from urllib.error import HTTPError

from apple_music_presence.artwork import Artwork
from apple_music_presence.config import Settings, load_settings, save_settings
from apple_music_presence.motion_artwork import (
    AutomaticArtworkResolver, GithubArtworkHost, album_page, animated_webp,
    download_video, find_motion,
)

PAGE = "https://music.apple.com/us/album/album/123"
STREAM = "https://mvod.itunes.apple.com/itunes-assets/motion/master.m3u8"
STATIC = Artwork("https://is1-ssl.mzstatic.com/image/cover.jpg", PAGE + "?i=456")
PUBLIC = ("https://raw.githubusercontent.com/owner/repo/" + "a" * 40
          + "/artwork/motion/123-abcdef012345.webp")


def page_html(stream=STREAM, album_id="123", album="Album"):
    item = {"title": album, "subtitleLinks": [{"title": "Album Artist"}],
            "contentDescriptor": {"kind": "album", "identifiers": {"storeAdamID": album_id}},
            "videoArtwork": {"dictionary": {"motionDetailSquare": {"video": stream}}} if stream else None}
    return '<script type="application/json">' + json.dumps({"sections": [{"items": [item]}]}) + '</script>'


class MotionFormatTests(unittest.TestCase):
    def test_album_url_uses_verified_catalog_id_and_removes_track_query(self):
        self.assertEqual(album_page(PAGE + "?i=456"), PAGE)
        for invalid in ("https://evil.test/us/album/x/123", "http://music.apple.com/us/album/x/123",
                        "https://music.apple.com/us/song/123", "https://music.apple.com/us/album/x/bad"):
            self.assertIsNone(album_page(invalid))

    def test_only_matching_album_header_supplies_motion(self):
        self.assertEqual(find_motion(page_html(), "123", "Track Artist feat. Guest", "Album"), STREAM)
        # Recommendations and other album editions must never supply the cover.
        for html in (page_html(album_id="999"), page_html(album="Album (Deluxe)"), "<html>Unknown format</html>"):
            with self.assertRaises(ValueError):
                find_motion(html, "123", "Artist", "Album")

    def test_verified_album_with_no_motion_returns_none(self):
        self.assertIsNone(find_motion(page_html(None), "123", "Artist", "Album"))

    def test_non_apple_motion_url_is_never_downloaded(self):
        self.assertIsNone(find_motion(page_html("https://evil.test/cover.m3u8"), "123", "Artist", "Album"))

    def test_byte_range_hls_downloads_shared_media_only_once(self):
        master = '#EXTM3U\n#EXT-X-STREAM-INF:CODECS="avc1.64001f",RESOLUTION=408x408\nvariant.m3u8\n'
        variant = ('#EXTM3U\n#EXT-X-MAP:URI="clip.mp4",BYTERANGE="20@0"\n'
                   '#EXTINF:4.0,\n#EXT-X-BYTERANGE:20@20\nclip.mp4\n'
                   '#EXTINF:4.0,\n#EXT-X-BYTERANGE:20@40\nclip.mp4\n#EXT-X-ENDLIST\n')
        with tempfile.TemporaryDirectory() as folder:
            directory = Path(folder)
            with patch("apple_music_presence.motion_artwork._download", side_effect=[master.encode(), variant.encode(), b"media"]) as fetch:
                result = download_video(STREAM, directory)
                local = result.read_text()
                self.assertEqual(fetch.call_count, 3)
                self.assertNotIn("https:", local)
                self.assertEqual(local.count("segment-0.mp4"), 3)
                self.assertIn("#EXT-X-BYTERANGE:20@40", local)

    def test_hls_foreign_host_encryption_and_long_loops_are_rejected(self):
        master = '#EXTM3U\n#EXT-X-STREAM-INF:CODECS="avc1.64001f",RESOLUTION=408x408\nvariant.m3u8\n'
        variants = ('#EXTINF:4,\nhttps://evil.test/file.mp4\n#EXT-X-ENDLIST',
                    '#EXT-X-KEY:METHOD=AES-128,URI="key"\n#EXT-X-ENDLIST',
                    '#EXTINF:61,\nclip.mp4\n#EXT-X-ENDLIST',
                    '#EXTINF:4,\nclip.mp4')
        with tempfile.TemporaryDirectory() as folder:
            for variant in variants:
                with self.subTest(variant=variant):
                    with patch("apple_music_presence.motion_artwork._download", side_effect=[master.encode(), variant.encode()]) as fetch:
                        with self.assertRaises(ValueError):
                            download_video(STREAM, Path(folder))
                        self.assertEqual(fetch.call_count, 2)

    def test_real_cover_has_animation_and_truncated_or_static_data_is_rejected(self):
        content = (Path(__file__).resolve().parents[1] / "artwork" / "after-hours.webp").read_bytes()
        self.assertTrue(animated_webp(content))
        self.assertFalse(animated_webp(content[:-10]))
        self.assertFalse(animated_webp(b"RIFF" + (12).to_bytes(4, "little") + b"WEBPVP8 " + b"0000"))


class MotionResolverTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.cache = Path(self.folder.name) / "motion.json"
        self.catalog = AsyncMock()
        self.catalog.resolve.return_value = STATIC
        self.catalog.resolve_album.return_value = STATIC
        self.host = GithubArtworkHost("owner/repo")
        self.resolver = AutomaticArtworkResolver(self.catalog, self.host, self.cache)

    async def asyncTearDown(self):
        await self.resolver.close()
        self.folder.cleanup()

    async def finish_job(self):
        await asyncio.gather(*list(self.resolver._jobs.values()))

    async def test_static_is_immediate_then_motion_upgrades_same_song_and_persists(self):
        gate = asyncio.Event()

        async def convert(stream):
            await gate.wait()
            return b"converted fixture"

        with patch("apple_music_presence.motion_artwork.discover_motion", return_value=STREAM) as discover:
            with patch("apple_music_presence.motion_artwork.convert_motion", side_effect=convert):
                with patch.object(self.host, "publish", return_value=PUBLIC) as publish:
                    first = await self.resolver.resolve("Song", "Artist", "Album")
                    self.assertFalse(first.animated)
                    self.assertEqual(first.url, STATIC.url)
                    self.assertEqual(first.track_url, PAGE)
                    await self.resolver.resolve("Next", "Artist", "Album")
                    self.assertEqual(len(self.resolver._jobs), 1)
                    gate.set()
                    await self.finish_job()
                    upgraded = await self.resolver.refresh("Next", "Artist", "Album")
                    self.assertEqual(upgraded, Artwork(PUBLIC, PAGE, True))
                    discover.assert_called_once()
                    publish.assert_called_once()
        restarted = AutomaticArtworkResolver(self.catalog, self.host, self.cache)
        self.catalog.resolve.reset_mock()
        self.assertEqual(await restarted.resolve("Third", "Artist", "Album"), upgraded)
        self.catalog.resolve.assert_not_awaited()
        await restarted.close()

    async def test_no_motion_keeps_static_and_caches_album_miss(self):
        with patch("apple_music_presence.motion_artwork.discover_motion", return_value=None) as discover:
            await self.resolver.resolve("Song", "Artist", "Album")
            await self.finish_job()
            second = await self.resolver.resolve("Next", "Artist", "Album")
            self.assertFalse(second.animated)
            self.assertFalse(self.resolver._jobs)
            discover.assert_called_once()

    async def test_upload_failure_keeps_static_and_retries_after_short_delay(self):
        with patch("apple_music_presence.motion_artwork.time.time", return_value=100) as clock:
            with patch("apple_music_presence.motion_artwork.discover_motion", return_value=STREAM) as discover:
                with patch("apple_music_presence.motion_artwork.convert_motion", new_callable=AsyncMock, return_value=b"bytes"):
                    with patch.object(self.host, "publish", side_effect=HTTPError("https://api.github.com", 403, "denied", {}, None)):
                        await self.resolver.resolve("Song", "Artist", "Album")
                        await self.finish_job()
                        self.assertFalse((await self.resolver.refresh("Song", "Artist", "Album")).animated)
                        self.assertIn("GitHub access", self.resolver.status)
                        clock.return_value = 159
                        await self.resolver.refresh("Song", "Artist", "Album")
                        self.assertFalse(self.resolver._jobs)
                        clock.return_value = 161
                        await self.resolver.refresh("Song", "Artist", "Album")
                        await self.finish_job()
                        self.assertEqual(discover.call_count, 2)

    async def test_missing_catalog_match_never_downloads_or_uploads(self):
        self.catalog.resolve.return_value = None
        self.catalog.resolve_album.return_value = None
        with patch("apple_music_presence.motion_artwork.discover_motion") as discover:
            self.assertIsNone(await self.resolver.resolve("Song", "Artist", "Album"))
            discover.assert_not_called()
            self.assertFalse(self.resolver._jobs)

    async def test_close_cancels_conversion_without_publishing(self):
        entered = asyncio.Event()

        async def convert(stream):
            entered.set()
            await asyncio.Event().wait()

        with patch("apple_music_presence.motion_artwork.discover_motion", return_value=STREAM):
            with patch("apple_music_presence.motion_artwork.convert_motion", side_effect=convert):
                with patch.object(self.host, "publish") as publish:
                    await self.resolver.resolve("Song", "Artist", "Album")
                    await asyncio.wait_for(entered.wait(), 1)
                    await self.resolver.close()
                    self.assertFalse(self.resolver._jobs)
                    publish.assert_not_called()

    async def test_cache_from_different_host_is_ignored(self):
        self.cache.write_text(json.dumps({"repository": "other/repo", "albums": {}}))
        self.assertFalse(AutomaticArtworkResolver(self.catalog, self.host, self.cache)._cache)


class MotionHostingTests(unittest.TestCase):
    def test_host_auth_stays_on_github_api_and_new_image_uses_immutable_commit(self):
        content = (Path(__file__).resolve().parents[1] / "artwork" / "after-hours.webp").read_bytes()
        calls = []

        def request(url, limit, **kwargs):
            calls.append((url, kwargs))
            if url.startswith("https://raw.githubusercontent.com/"):
                self.assertNotIn("headers", kwargs)
                if "/" + "b" * 40 + "/" in url:
                    raise HTTPError(url, 404, "missing", {}, None)
                return content
            self.assertEqual(kwargs["headers"]["Authorization"], "Bearer secret-fixture")
            if url.endswith("/repos/owner/repo"):
                return json.dumps({"private": False, "permissions": {"push": True}}).encode()
            if url.endswith("/git/ref/heads/motion-artwork"):
                return json.dumps({"object": {"sha": "b" * 40}}).encode()
            self.assertEqual(kwargs["method"], "PUT")
            body = json.loads(kwargs["data"])
            self.assertEqual(body["branch"], "motion-artwork")
            self.assertNotIn("secret-fixture", body["message"])
            return json.dumps({"commit": {"sha": "a" * 40}}).encode()

        with patch("apple_music_presence.motion_artwork.github_token", return_value="secret-fixture"):
            with patch("apple_music_presence.motion_artwork._download", side_effect=request):
                url = GithubArtworkHost("owner/repo").publish("123", STREAM, content)
        self.assertIn("/" + "a" * 40 + "/artwork/motion/123-", url)
        self.assertEqual(sum(call[1].get("method") == "PUT" for call in calls), 1)

    def test_private_host_and_invalid_image_are_rejected(self):
        host = GithubArtworkHost("owner/repo")
        with patch("apple_music_presence.motion_artwork.github_token") as token:
            with self.assertRaises(ValueError):
                host.publish("123", STREAM, b"not an image")
            token.assert_not_called()
        content = (Path(__file__).resolve().parents[1] / "artwork" / "after-hours.webp").read_bytes()
        with patch("apple_music_presence.motion_artwork.github_token", return_value="secret"):
            with patch("apple_music_presence.motion_artwork._download", return_value=b'{"private":true,"permissions":{"push":true}}'):
                with self.assertRaises(ValueError):
                    host.publish("123", STREAM, content)

    def test_settings_round_trip_and_motion_requires_public_host_name(self):
        settings = Settings(client_id="123456789012345678", motion_artwork=True, artwork_repository="owner/repo")
        settings.validate()
        self.assertTrue(settings.artwork)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "settings.json"
            save_settings(settings, path)
            self.assertEqual(load_settings(path), settings)
            self.assertNotIn("token", path.read_text())
        settings.artwork_repository = "https://github.com/owner/repo"
        with self.assertRaises(ValueError):
            settings.validate()
