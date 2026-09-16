"""Offline tests for conservative matching, privacy and request limits."""

import asyncio
import unittest
from unittest.mock import AsyncMock, patch
from urllib.parse import parse_qs, urlsplit

from apple_music_presence.artwork import Artwork, ItunesArtworkResolver


ART_URL = "https://is1-ssl.mzstatic.com/image/thumb/example/100x100bb.jpg"
TRACK_URL = "https://music.apple.com/us/album/song/123?i=456"


def catalog_result(**changes):
    result = {
        "kind": "song",
        "trackName": "Song",
        "artistName": "Artist",
        "collectionName": "Album",
        "artworkUrl100": ART_URL,
        "trackViewUrl": TRACK_URL,
    }
    result.update(changes)
    return result


class ArtworkTests(unittest.IsolatedAsyncioTestCase):
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
                clock.return_value = 161
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
