import asyncio
import json
import unittest
from unittest.mock import AsyncMock, patch
from urllib.error import HTTPError

from apple_music_presence.apple_catalog import AppleMusicArtworkResolver, _album_tracks, _candidate_pages

PAGE = "https://music.apple.com/us/album/data/123"
ART = "https://is1-ssl.mzstatic.com/image/cover/{w}x{h}bb.{f}"


def html(items):
    return '<script type="application/json">' + json.dumps({"items": items}) + '</script>'


def descriptor(kind, page=PAGE, album_id="123"):
    return {"kind": kind, "url": page, "identifiers": {"storeAdamID": album_id}}


def header(**changes):
    return {"title": "DATA", "contentDescriptor": descriptor("album"), "videoArtwork": None,
            "artwork": {"dictionary": {"url": ART}}, **changes}


def song(title="First", artist="Tainy & Guest", **changes):
    return {"title": title, "artistName": artist, "trackNumber": 1,
            "contentDescriptor": descriptor("song", PAGE + "?i=456"), **changes}


class ApplePageTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.catalog = AsyncMock()
        self.catalog.resolve.return_value = None
        self.resolver = AppleMusicArtworkResolver(self.catalog)

    async def test_missing_itunes_recording_uses_verified_album_and_indexes_guests(self):
        search = html([song()])
        album = html([header(), song(), song("Second", "Tainy & Another")])
        with patch.object(self.resolver, "_page", new_callable=AsyncMock, side_effect=[search, album]) as fetch:
            first = await self.resolver.resolve("First", "Tainy & Guest", "DATA")
            self.assertEqual(first.url, ART.replace("{w}", "300").replace("{h}", "300").replace("{f}", "jpg"))
            second = await self.resolver.resolve("Second", "Tainy & Another", "DATA")
            self.assertEqual(first, second)
            self.assertEqual(fetch.await_count, 2)
            self.catalog.resolve.assert_awaited_once()
            self.assertEqual(first.track_url, PAGE)
            self.assertEqual(await self.resolver.resolve_album("Tainy & Another", "DATA"), first)
            self.catalog.resolve_album.assert_not_awaited()

    async def test_catalog_success_never_fetches_web_pages(self):
        self.catalog.resolve.return_value = "cover fixture"
        with patch.object(self.resolver, "_page", new_callable=AsyncMock) as fetch:
            self.assertEqual(await self.resolver.resolve("First", "Tainy", "DATA"), "cover fixture")
            fetch.assert_not_awaited()

    async def test_wrong_artist_album_edition_or_recommendation_is_rejected(self):
        for album in (html([header(), song(artist="Other")]),
                      html([header(title="DATA (Instrumental)"), song()]),
                      html([header(contentDescriptor=descriptor("album", album_id="999")), song()]),
                      html([header(), song(contentDescriptor=descriptor("song", "https://music.apple.com/us/album/x/999"))]),
                      html([header(artwork={"dictionary": {"url": "https://evil.test/cover.jpg"}}), song()])):
            resolver = AppleMusicArtworkResolver(self.catalog)
            with patch.object(resolver, "_page", new_callable=AsyncMock, side_effect=[html([song()]), album]):
                self.assertIsNone(await resolver.resolve("First", "Tainy & Guest", "DATA"))

    async def test_ambiguous_album_ids_do_not_guess_or_seed_cache(self):
        other = "https://music.apple.com/us/album/data/999"
        search = html([song(), song(contentDescriptor=descriptor("song", other))])
        second_album = html([header(contentDescriptor=descriptor("album", other, "999")),
                            song(contentDescriptor=descriptor("song", other))])
        with patch.object(self.resolver, "_page", new_callable=AsyncMock,
                          side_effect=[search, html([header(), song()]), second_album]):
            self.assertIsNone(await self.resolver.resolve("First", "Tainy & Guest", "DATA"))
            self.assertEqual(len(self.resolver._tracks), 1)

    async def test_network_failure_retries_and_cancellation_propagates(self):
        with patch("apple_music_presence.apple_catalog.monotonic", return_value=100) as clock:
            with patch.object(self.resolver, "_page", new_callable=AsyncMock, side_effect=TimeoutError) as fetch:
                self.assertIsNone(await self.resolver.resolve("First", "Tainy & Guest", "DATA"))
                clock.return_value = 109
                await self.resolver.resolve("First", "Tainy & Guest", "DATA")
                fetch.assert_awaited_once()
                clock.return_value = 110
                fetch.side_effect = asyncio.CancelledError
                with self.assertRaises(asyncio.CancelledError):
                    await self.resolver.resolve("First", "Tainy & Guest", "DATA")

    async def test_missing_or_excessive_metadata_never_searches(self):
        for metadata in (("First", "", "DATA"), ("!", "Tainy", "DATA"), ("x" * 513, "Tainy", "DATA")):
            self.assertIsNone(await self.resolver.resolve(*metadata))
        self.catalog.resolve.assert_not_awaited()

    def test_search_candidates_are_exact_bounded_and_apple_only(self):
        self.assertEqual(_candidate_pages(html([song("First (Remix)")]), "First"), [])
        self.assertEqual(_candidate_pages(html([song(contentDescriptor=descriptor("song", "https://evil.test/123"))]), "First"), [])
        self.assertEqual(_candidate_pages(html([song(contentDescriptor=descriptor("song", f"https://music.apple.com/us/album/x/{n}")) for n in range(4)]), "First"), [])

    def test_album_header_artwork_must_be_unique(self):
        self.assertEqual(_album_tracks(html([header(), header(artwork={"dictionary": {"url": ART + "other"}}), song()]), PAGE, "DATA"), {})

    async def test_page_requests_are_spaced_and_limited(self):
        with patch("apple_music_presence.apple_catalog.monotonic", return_value=100):
            with patch("apple_music_presence.apple_catalog._download", return_value=b"page") as fetch:
                with patch("apple_music_presence.apple_catalog.asyncio.sleep", new_callable=AsyncMock) as sleep:
                    await self.resolver._page(PAGE)
                    await self.resolver._page(PAGE)
                    sleep.assert_awaited_once()
                    self.assertAlmostEqual(sleep.await_args.args[0], 3.2)
                    fetch.assert_called_with(PAGE, 3 * 1024 * 1024)

    async def test_album_redirect_only_follows_same_public_album_and_storefront(self):
        alias = "https://music.apple.com/us/album/song-slug/123"
        with patch("apple_music_presence.apple_catalog._download", side_effect=[
                HTTPError(alias, 301, "Moved", {"Location": PAGE}, None), b"page"]):
            with patch("apple_music_presence.apple_catalog.asyncio.sleep", new_callable=AsyncMock):
                self.assertEqual(await self.resolver._page(alias), "page")
        for target in ("https://evil.test/us/album/data/123", "https://music.apple.com/us/album/data/999",
                       "https://music.apple.com/gb/album/data/123", "https://user:secret@music.apple.com/us/album/data/123"):
            with patch("apple_music_presence.apple_catalog._download", side_effect=
                       HTTPError(alias, 301, "Moved", {"Location": target}, None)) as fetch:
                with patch("apple_music_presence.apple_catalog.asyncio.sleep", new_callable=AsyncMock):
                    with self.assertRaises(HTTPError):
                        await self.resolver._page(alias)
                fetch.assert_called_once()
