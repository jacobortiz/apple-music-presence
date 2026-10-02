"""Offline tests for conservative matching, privacy and request limits."""

import asyncio
import io
import struct
import threading
import unittest
from unittest.mock import AsyncMock, MagicMock, patch
from urllib.parse import parse_qs, urlsplit

from apple_music_presence.artwork import (
    Artwork, ItunesArtworkResolver, _fetch_cover, _full_size_artwork,
    _jpeg_display_fingerprint, _normalize_exif_comment, _shared_cover,
)


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


def exif_fixture(comment=b"ASCII\0\0\0" + b"a" * 26, *, orientation=None, endian=">"):
    pack = lambda fmt, *values: struct.pack(endian + fmt, *values)
    root_count = 1 + (orientation is not None)
    child_offset = 8 + 2 + 12 * root_count + 4
    comment_offset = child_offset + 2 + 4 * 12 + 4
    root = pack("HHII", 34665, 4, 1, child_offset)
    if orientation is not None:
        root += pack("HHI", 274, 3, 1) + pack("H", orientation) + b"\0\0"
    child = (pack("HHII", 37510, 7, len(comment), comment_offset)
             + pack("HHI", 40961, 3, 1) + pack("H", 1) + b"\0\0"
             + pack("HHII", 40962, 4, 1, 1024)
             + pack("HHII", 40963, 4, 1, 1024))
    return (b"Exif\0\0" + (b"MM" if endian == ">" else b"II") + pack("HI", 42, 8)
            + pack("H", root_count) + root + pack("I", 0)
            + pack("H", 4) + child + pack("I", 0) + comment)


def jpeg_fixture(comment=b"ASCII\0\0\0" + b"a" * 26, *, orientation=None, scan=b"image-data", icc=b"ICC_PROFILE\0"):
    def segment(marker, payload):
        return bytes((255, marker)) + (len(payload) + 2).to_bytes(2, "big") + payload

    frame = b"\x08\x04\x00\x04\x00\x03\x01\x11\x00\x02\x11\x00\x03\x11\x00"
    start = b"\x03\x01\x00\x02\x00\x03\x00\x00\x3f\x00"
    return (b"\xff\xd8" + segment(225, exif_fixture(comment, orientation=orientation))
            + segment(226, icc) + segment(192, frame) + segment(218, start) + scan + b"\xff\xd9")


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

    async def test_album_lookup_shares_certain_cover_without_choosing_ambiguous_release(self):
        result = {"collectionType": "Album", "artistName": "Artist", "collectionName": "Album",
                  "artworkUrl100": THUMB_URL, "collectionViewUrl": "https://music.apple.com/us/album/album/123"}
        with patch("apple_music_presence.artwork._fetch_json", return_value={"results": [
                result, {**result, "collectionViewUrl": "https://music.apple.com/us/album/album/999"}]}):
            with patch("apple_music_presence.artwork._fetch_cover") as fetch:
                self.assertEqual(await ItunesArtworkResolver().resolve_album("Artist", "Album"), Artwork(ART_URL, ""))
                fetch.assert_not_called()
        for results in ([{**result, "artistName": "Other"}], [{**result, "collectionName": "Album (Live)"}]):
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

    async def test_ambiguous_recordings_with_same_cover_have_no_arbitrary_link(self):
        resolver = ItunesArtworkResolver()
        payload = {"results": [catalog_result(), catalog_result(trackViewUrl="https://music.apple.com/us/album/song/999?i=888")]}
        with patch("apple_music_presence.artwork._fetch_json", return_value=payload):
            with patch("apple_music_presence.artwork._fetch_cover") as fetch:
                self.assertEqual(await resolver.resolve("Song", "Artist", "Album"), Artwork(ART_URL, ""))
                fetch.assert_not_called()

    async def test_exact_releases_with_only_comment_differences_share_static_cover_and_cache(self):
        resolver = ItunesArtworkResolver()
        other = THUMB_URL.replace("/example/", "/other/")
        payload = {"results": [catalog_result(), catalog_result(artworkUrl100=other,
                   trackViewUrl="https://music.apple.com/us/album/song/999?i=888")]}
        with patch("apple_music_presence.artwork._fetch_json", return_value=payload) as catalog:
            with patch("apple_music_presence.artwork._fetch_cover", side_effect=[
                    jpeg_fixture(), jpeg_fixture(b"ASCII\0\0\0" + b"b" * 26)]) as fetch:
                cover = await resolver.resolve("Song", "Artist", "Album")
                self.assertEqual(cover, Artwork(ART_URL, ""))
                self.assertFalse(cover.animated)
                self.assertEqual(await resolver.resolve("Song", "Artist", "Album"), cover)
                catalog.assert_called_once()
                self.assertEqual(fetch.call_count, 2)

    async def test_visible_cover_or_display_metadata_difference_remains_ambiguous(self):
        other = THUMB_URL.replace("/example/", "/other/")
        payload = {"results": [catalog_result(), catalog_result(artworkUrl100=other,
                   trackViewUrl="https://music.apple.com/us/album/song/999?i=888")]}
        for second in (jpeg_fixture(scan=b"different-image"), jpeg_fixture(icc=b"different-ICC"),
                       jpeg_fixture(orientation=6), b"not a JPEG", jpeg_fixture()[:-1]):
            with self.subTest(second=second[:12]):
                with patch("apple_music_presence.artwork._fetch_json", return_value=payload):
                    with patch("apple_music_presence.artwork._fetch_cover", side_effect=[jpeg_fixture(), second]):
                        self.assertIsNone(await ItunesArtworkResolver().resolve("Song", "Artist", "Album"))

    async def test_unmatched_editions_never_trigger_cover_comparison(self):
        payload = {"results": [catalog_result(collectionName="Album (Deluxe)"),
                   catalog_result(collectionName="Album (Live)", artworkUrl100=THUMB_URL.replace("example", "other"))]}
        with patch("apple_music_presence.artwork._fetch_json", return_value=payload):
            with patch("apple_music_presence.artwork._fetch_cover") as fetch:
                self.assertIsNone(await ItunesArtworkResolver().resolve("Song", "Artist", "Album"))
                fetch.assert_not_called()

    async def test_too_many_exact_candidates_skip_comparison(self):
        payload = {"results": [catalog_result(trackViewUrl=f"https://music.apple.com/us/album/song/{index}") for index in range(5)]}
        with patch("apple_music_presence.artwork._fetch_json", return_value=payload):
            with patch("apple_music_presence.artwork._fetch_cover") as fetch:
                self.assertIsNone(await ItunesArtworkResolver().resolve("Song", "Artist", "Album"))
                fetch.assert_not_called()

    async def test_cover_failure_uses_short_retry_cache(self):
        resolver = ItunesArtworkResolver()
        payload = {"results": [catalog_result(), catalog_result(artworkUrl100=THUMB_URL.replace("example", "other"),
                   trackViewUrl="https://music.apple.com/us/album/song/999")]}
        with patch("apple_music_presence.artwork.monotonic", return_value=100) as clock:
            with patch("apple_music_presence.artwork._fetch_json", return_value=payload):
                with patch("apple_music_presence.artwork._fetch_cover", side_effect=TimeoutError()) as fetch:
                    self.assertIsNone(await resolver.resolve("Song", "Artist", "Album"))
                    clock.return_value = 109
                    self.assertIsNone(await resolver.resolve("Song", "Artist", "Album"))
                    fetch.assert_called_once()
                clock.return_value = 110
                with patch("apple_music_presence.artwork._fetch_cover", side_effect=[jpeg_fixture(), jpeg_fixture()]):
                    self.assertEqual(await resolver.resolve("Song", "Artist", "Album"), Artwork(ART_URL, ""))

    async def test_cover_comparison_cancellation_does_not_cache_a_failure(self):
        import functools
        entered, stopped = threading.Event(), threading.Event()
        resolver = ItunesArtworkResolver()
        payload = {"results": [catalog_result(), catalog_result(artworkUrl100=THUMB_URL.replace("example", "other"),
                   trackViewUrl="https://music.apple.com/us/album/song/999")]}

        def comparison(matches, timeout, stop):
            entered.set()
            stop.wait(1)
            stopped.set()

        with patch("apple_music_presence.artwork._fetch_json", return_value=payload):
            with patch("apple_music_presence.artwork._shared_cover", side_effect=comparison):
                task = asyncio.create_task(resolver.resolve("Song", "Artist", "Album"))
                self.assertTrue(await asyncio.to_thread(functools.partial(entered.wait, 1)))
                task.cancel()
                with self.assertRaises(asyncio.CancelledError):
                    await task
                self.assertTrue(await asyncio.to_thread(functools.partial(stopped.wait, 1)))
                self.assertFalse(resolver._cache)

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


class CoverComparisonTests(unittest.TestCase):
    def test_exif_normalization_preserves_every_byte_except_user_comment(self):
        for endian in (">", "<"):
            original = exif_fixture(endian=endian)
            changed = exif_fixture(b"ASCII\0\0\0" + b"b" * 26, endian=endian)
            normalized = _normalize_exif_comment(original)
            self.assertEqual(normalized, _normalize_exif_comment(changed))
            self.assertEqual(normalized[:-34], original[:-34])
            self.assertEqual(normalized[-34:], b"\0" * 34)
        self.assertNotEqual(_jpeg_display_fingerprint(jpeg_fixture(orientation=1)),
                            _jpeg_display_fingerprint(jpeg_fixture(orientation=6)))

    def test_unknown_and_overlapping_exif_structures_fail_closed(self):
        original = exif_fixture()
        changes = ((24, 4, 8),    # Exif IFD overlaps the root directory.
                   (28, 4, 26),  # Unexpected next directory.
                   (38, 4, 5000),  # Oversized UserComment.
                   (42, 4, 26),  # UserComment overlaps its directory.
                   (46, 2, 34853))  # Unknown GPS tag.
        for offset, size, value in changes:
            with self.subTest(offset=offset):
                content = bytearray(original)
                content[offset:offset + size] = value.to_bytes(size, "big")
                self.assertIsNone(_normalize_exif_comment(bytes(content)))
        self.assertIsNone(_normalize_exif_comment(original[:-1]))
        invalid = bytearray(jpeg_fixture())
        invalid[4:6] = b"\0\1"  # Invalid APP1 segment length.
        self.assertIsNone(_jpeg_display_fingerprint(bytes(invalid)))

    def test_cover_download_checks_destination_size_and_timeout_without_credentials(self):
        for url in ("http://is1.mzstatic.com/cover.jpg", "https://evil.test/cover.jpg",
                    "https://is1.mzstatic.com.evil.test/cover.jpg",
                    "https://user:secret@is1.mzstatic.com/cover.jpg",
                    ART_URL + "?token=secret", ART_URL + "#fragment"):
            with self.subTest(url=url):
                with patch("apple_music_presence.artwork.build_opener") as opener:
                    with self.assertRaises(ValueError):
                        _fetch_cover(url, 5, threading.Event())
                    opener.assert_not_called()

        opener = MagicMock()
        response = io.BytesIO(jpeg_fixture())
        opener.open.return_value.__enter__.return_value = response
        with patch("apple_music_presence.artwork.build_opener", return_value=opener):
            self.assertEqual(_fetch_cover(ART_URL, 5, threading.Event()), jpeg_fixture())
        request = opener.open.call_args.args[0]
        self.assertEqual(request.full_url, ART_URL)
        self.assertEqual(set(name.casefold() for name, value in request.header_items()), {"accept", "user-agent"})
        self.assertEqual(opener.open.call_args.kwargs["timeout"], 5)

        opener.open.return_value.__enter__.return_value = io.BytesIO(jpeg_fixture())
        with patch("apple_music_presence.artwork.build_opener", return_value=opener):
            with patch("apple_music_presence.artwork._MAX_COVER_BYTES", 128):
                with self.assertRaisesRegex(ValueError, "size limit"):
                    _fetch_cover(ART_URL, 5, threading.Event())
        opener.open.return_value.__enter__.return_value = io.BytesIO(jpeg_fixture())
        with patch("apple_music_presence.artwork.build_opener", return_value=opener):
            with patch("apple_music_presence.artwork.monotonic", side_effect=[100, 101]):
                with self.assertRaises(TimeoutError):
                    _fetch_cover(ART_URL, 0.5, threading.Event())

    def test_cancelled_comparison_stops_before_any_followup_download(self):
        matches = {Artwork(ART_URL, TRACK_URL), Artwork(ART_URL.replace("example", "other"), TRACK_URL)}
        stop = threading.Event()
        stop.set()
        with patch("apple_music_presence.artwork._fetch_cover") as fetch:
            self.assertIsNone(_shared_cover(matches, 5, stop))
            fetch.assert_not_called()
        stop.clear()

        def first_cover(*args):
            stop.set()
            return jpeg_fixture()

        with patch("apple_music_presence.artwork._fetch_cover", side_effect=first_cover) as fetch:
            self.assertIsNone(_shared_cover(matches, 5, stop))
            fetch.assert_called_once()


if __name__ == "__main__":
    unittest.main()
