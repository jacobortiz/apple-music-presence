"""Offline tests for conservative matching, privacy and request limits."""

import asyncio
import unittest
from unittest.mock import AsyncMock, patch
from urllib.parse import parse_qs, urlsplit

from apple_music_presence.artwork import Artwork, ItunesArtworkResolver, _full_size_artwork


THUMB_URL = "https://is1-ssl.mzstatic.com/image/thumb/example/100x100bb.jpg"
ART_URL = "https://is1-ssl.mzstatic.com/image/thumb/example/1024x1024bb.jpg"
TRACK_URL = "https://music.apple.com/us/album/song/123?i=456"


def catalog_result(**changes):
    result = {
        "kind": "song",
        "trackName": "Song",
        "artistName": "Artist",
        "collectionName": "Album",
        "artworkUrl100": THUMB_URL,
        "trackViewUrl": TRACK_URL,
    }
    result.update(changes)
    return result


class ArtworkTests(unittest.IsolatedAsyncioTestCase):
    def test_thumbnail_upgrade_preserves_asset_and_removes_low_quality_suffix(self):
        for suffix in ("100x100bb.jpg", "100x100bb-75.jpg", "60x60bb.jpg"):
            with self.subTest(suffix=suffix):
                self.assertEqual(_full_size_artwork(THUMB_URL.rsplit("/", 1)[0] + "/" + suffix), ART_URL)
        png = THUMB_URL.replace(".jpg", ".png")
        self.assertEqual(_full_size_artwork(png), ART_URL.replace(".jpg", ".png"))

    def test_thumbnail_upgrade_leaves_unrecognized_and_larger_urls_unchanged(self):
        for url in (THUMB_URL.replace("/image/thumb/", "/image/"),
                    THUMB_URL.replace("100x100bb", "1400x1400bb"),
                    THUMB_URL.replace("100x100bb", "100x200bb"),
                    THUMB_URL.replace("100x100bb", "original"),
                    THUMB_URL.replace("is1-ssl.mzstatic.com", "mvod.itunes.apple.com"),
                    THUMB_URL + "?version=1", THUMB_URL + "#cover"):
            with self.subTest(url=url):
                self.assertEqual(_full_size_artwork(url), url)
        for url in (THUMB_URL.replace("https:", "http:"),
                    THUMB_URL.replace("mzstatic.com", "mzstatic.com.attacker.test"),
                    THUMB_URL.replace("https://", "https://user:password@"), None):
            self.assertIsNone(_full_size_artwork(url))

    def test_catalog_downloads_reject_other_destinations_and_redirects(self):
        from apple_music_presence.artwork import _fetch_json, _NoCatalogRedirect
        from urllib.request import Request
        for url in ("https://evil.test/search", "http://itunes.apple.com/search",
                    "https://user:password@itunes.apple.com/search"):
            with patch("apple_music_presence.artwork.build_opener") as opener:
                with self.assertRaises(ValueError):
                    _fetch_json(url, 5)
                opener.assert_not_called()
        self.assertIsNone(_NoCatalogRedirect().redirect_request(Request("https://itunes.apple.com/search"),
            None, 302, "Found", {}, "https://evil.test/search"))

    async def test_album_lookup_reuses_cover_across_songs_and_preserves_editions(self):
        resolver = ItunesArtworkResolver()
        result = {"collectionType": "Album", "artistName": "Artist", "collectionName": "Album",
                  "artworkUrl100": THUMB_URL, "collectionViewUrl": "https://music.apple.com/us/album/album/123"}
        with patch("apple_music_presence.artwork._fetch_json", return_value={"results": [result]}) as fetch:
            first = await resolver.resolve_album("Artist", "Album")
            self.assertEqual(first.url, ART_URL)
            self.assertEqual(first, await resolver.resolve_album("ARTIST", " Album "))
            fetch.assert_called_once()
            self.assertEqual(parse_qs(urlsplit(fetch.call_args.args[0]).query)["entity"], ["album"])
            self.assertIsNone(await resolver.resolve_album("Artist", "Album (Deluxe)"))

    async def test_album_lookup_omits_ambiguous_art_and_wrong_artists(self):
        result = {"collectionType": "Album", "artistName": "Artist", "collectionName": "Album",
                  "artworkUrl100": THUMB_URL, "collectionViewUrl": "https://music.apple.com/us/album/album/123"}
        for results in ([result, {**result, "collectionViewUrl": "https://music.apple.com/us/album/album/999"}],
                        [{**result, "artistName": "Other"}], [{**result, "collectionName": "Album (Live)"}]):
            with patch("apple_music_presence.artwork._fetch_json", return_value={"results": results}):
                self.assertIsNone(await ItunesArtworkResolver().resolve_album("Artist", "Album"))

    async def test_exact_match_and_cache_avoid_duplicate_network_requests(self):
        resolver = ItunesArtworkResolver(country="gb")
        with patch("apple_music_presence.artwork._fetch_json", return_value={"results": [catalog_result()]}) as fetch:
            expected = Artwork(ART_URL, TRACK_URL)
            self.assertEqual(await resolver.resolve("Song", "Artist", "Album"), expected)
            self.assertEqual(await resolver.resolve(" SONG ", "artist", "Album"), expected)
            fetch.assert_called_once()
            query = parse_qs(urlsplit(fetch.call_args.args[0]).query)
            self.assertEqual(query["country"], ["GB"])
            self.assertEqual(query["entity"], ["song"])
            self.assertEqual(query["term"], ["Song Artist Album"])

    async def test_remix_live_and_wrong_album_never_use_original_art(self):
        variants = [
            catalog_result(trackName="Song (Remix)"),
            catalog_result(trackName="Song - Live"),
            catalog_result(collectionName="Album (Deluxe)"),
            catalog_result(artistName="Different Artist"),
        ]
        for variant in variants:
            with self.subTest(variant=variant):
                resolver = ItunesArtworkResolver()
                with patch("apple_music_presence.artwork._fetch_json", return_value={"results": [variant]}):
                    self.assertIsNone(await resolver.resolve("Song", "Artist", "Album"))

    async def test_ambiguous_recordings_are_omitted(self):
        resolver = ItunesArtworkResolver()
        payload = {"results": [catalog_result(), catalog_result(trackViewUrl="https://music.apple.com/us/album/song/999?i=888")]}
        with patch("apple_music_presence.artwork._fetch_json", return_value=payload):
            self.assertIsNone(await resolver.resolve("Song", "Artist", "Album"))

    async def test_duplicate_identical_results_are_accepted(self):
        resolver = ItunesArtworkResolver()
        with patch("apple_music_presence.artwork._fetch_json", return_value={"results": [catalog_result(), catalog_result()]}):
            self.assertEqual(await resolver.resolve("Song", "Artist", "Album"), Artwork(ART_URL, TRACK_URL))

    async def test_missing_metadata_never_triggers_network(self):
        resolver = ItunesArtworkResolver()
        with patch("apple_music_presence.artwork._fetch_json") as fetch:
            for metadata in [("Song", "Artist", ""), ("", "Artist", "Album"), ("Song", "", "Album"), ("!", "Artist", "Album"), ("S" * 513, "Artist", "Album")]:
                self.assertIsNone(await resolver.resolve(*metadata))
            fetch.assert_not_called()

    async def test_failure_is_nonfatal_and_negatively_cached(self):
        resolver = ItunesArtworkResolver()
        with patch("apple_music_presence.artwork._fetch_json", side_effect=TimeoutError) as fetch:
            self.assertIsNone(await resolver.resolve("Song", "Artist", "Album"))
            self.assertIsNone(await resolver.resolve("Song", "Artist", "Album"))
            fetch.assert_called_once()

    async def test_failed_lookup_retries_after_ttl(self):
        resolver = ItunesArtworkResolver()
        with patch("apple_music_presence.artwork.monotonic", return_value=100) as clock:
            with patch("apple_music_presence.artwork._fetch_json", side_effect=[TimeoutError(), {"results": [catalog_result()]}]) as fetch:
                self.assertIsNone(await resolver.resolve("Song", "Artist", "Album"))
                clock.return_value = 109.99
                self.assertIsNone(await resolver.resolve("Song", "Artist", "Album"))
                fetch.assert_called_once()
                clock.return_value = 110
                self.assertEqual(await resolver.resolve("Song", "Artist", "Album"), Artwork(ART_URL, TRACK_URL))
                self.assertEqual(fetch.call_count, 2)

    async def test_rate_limit_wait_and_lru_capacity(self):
        resolver = ItunesArtworkResolver(cache_size=1)
        with patch("apple_music_presence.artwork.monotonic", return_value=100):
            with patch("apple_music_presence.artwork.asyncio.sleep", new_callable=AsyncMock) as sleep:
                with patch("apple_music_presence.artwork._fetch_json", return_value={"results": []}) as fetch:
                    await resolver.resolve("First", "Artist", "Album")
                    await resolver.resolve("Second", "Artist", "Album")
                    await resolver.resolve("First", "Artist", "Album")
                    self.assertEqual(fetch.call_count, 3)
                    self.assertEqual(sleep.await_count, 2)
                    self.assertAlmostEqual(sleep.await_args.args[0], 3.2)
                    self.assertEqual(len(resolver._cache), 1)

    async def test_simultaneous_same_track_only_fetches_once(self):
        resolver = ItunesArtworkResolver()
        with patch("apple_music_presence.artwork._fetch_json", return_value={"results": [catalog_result()]}) as fetch:
            results = await asyncio.gather(*(resolver.resolve("Song", "Artist", "Album") for _ in range(5)))
            self.assertEqual(results, [Artwork(ART_URL, TRACK_URL)] * 5)
            fetch.assert_called_once()

    async def test_untrusted_urls_and_malformed_results_are_omitted(self):
        payloads = [
            None,
            {"results": None},
            {"results": [None, 42, {"kind": "song"}]},
            {"results": [catalog_result(artworkUrl100="https://is1.mzstatic.com.attacker.test/art.jpg")]},
            {"results": [catalog_result(artworkUrl100="http://is1.mzstatic.com/art.jpg")]},
            {"results": [catalog_result(trackViewUrl="https://attacker.test/song")]},
            {"results": [catalog_result(trackViewUrl="https://user:pass@music.apple.com/song")]},
            {"results": [catalog_result(trackViewUrl="https://music.apple.com:bad/song")]},
        ]
        for payload in payloads:
            with self.subTest(payload=payload):
                with patch("apple_music_presence.artwork._fetch_json", return_value=payload):
                    self.assertIsNone(await ItunesArtworkResolver().resolve("Song", "Artist", "Album"))

    async def test_cancellation_propagates_without_poisoning_cache(self):
        resolver = ItunesArtworkResolver()
        with patch("apple_music_presence.artwork.asyncio.to_thread", side_effect=asyncio.CancelledError):
            with self.assertRaises(asyncio.CancelledError):
                await resolver.resolve("Song", "Artist", "Album")
        self.assertEqual(len(resolver._cache), 0)


if __name__ == "__main__":
    unittest.main()
