import json
import unittest
from unittest.mock import patch
from urllib.error import HTTPError

from apple_music_presence.motion_artwork import _album_redirect, discover_motion


ALIAS = "https://music.apple.com/us/album/song-slug/123"
PAGE = "https://music.apple.com/us/album/album/123"
STREAM = "https://mvod.itunes.apple.com/itunes-assets/motion/master.m3u8"


def album_html():
    header = {
        "title": "Album",
        "contentDescriptor": {"kind": "album", "identifiers": {"storeAdamID": "123"}},
        "videoArtwork": {"dictionary": {"motionDetailSquare": {"video": STREAM}}},
    }
    return ('<script type="application/json">' + json.dumps(header) + '</script>').encode()


def redirect(source, target, code=301):
    return HTTPError(source, code, "Moved", {"Location": target} if target is not None else {}, None)


class MotionRedirectTests(unittest.TestCase):
    def test_motion_follows_only_same_album_canonical_redirect_without_headers(self):
        with patch("apple_music_presence.motion_artwork._download",
                   side_effect=[redirect(ALIAS, PAGE), album_html()]) as download:
            self.assertEqual(discover_motion(ALIAS, "Artist", "Album"), STREAM)
        self.assertEqual(download.call_args_list[0].args, (ALIAS, 3 * 1024 * 1024))
        self.assertEqual(download.call_args_list[1].args, (PAGE, 3 * 1024 * 1024))
        self.assertFalse(any(call.kwargs for call in download.call_args_list))

    def test_relative_redirects_and_supported_statuses_keep_the_verified_identity(self):
        for code in (301, 302, 307, 308):
            with self.subTest(code=code):
                self.assertEqual(_album_redirect(ALIAS, redirect(ALIAS, "/us/album/album/123", code)), PAGE)

    def test_redirect_cannot_change_storefront_album_host_or_credentials(self):
        targets = (
            "https://music.apple.com/gb/album/album/123",
            "https://music.apple.com/us/album/album/999",
            "https://evil.test/us/album/album/123",
            "https://user:secret@music.apple.com/us/album/album/123",
            "http://music.apple.com/us/album/album/123",
            "https://music.apple.com:444/us/album/album/123",
            "https://music.apple.com/us/artist/artist/123",
            "https://music.apple.com:[bad/us/album/album/123",
            None,
        )
        for target in targets:
            with self.subTest(target=target):
                with patch("apple_music_presence.motion_artwork._download",
                           side_effect=redirect(ALIAS, target)) as download:
                    with self.assertRaises(HTTPError):
                        discover_motion(ALIAS, "Artist", "Album")
                download.assert_called_once()

    def test_unrelated_status_and_search_redirect_are_rejected(self):
        self.assertIsNone(_album_redirect(ALIAS, redirect(ALIAS, PAGE, 303)))
        search = "https://music.apple.com/us/search?term=Album"
        self.assertIsNone(_album_redirect(search, redirect(search, PAGE)))
        with patch("apple_music_presence.motion_artwork._download") as download:
            with self.assertRaises(ValueError):
                discover_motion(search, "Artist", "Album")
            download.assert_not_called()

    def test_discovery_allows_two_redirect_hops_and_bounds_loops(self):
        second = "https://music.apple.com/us/album/second-slug/123"
        with patch("apple_music_presence.motion_artwork._download", side_effect=[
                redirect(ALIAS, second), redirect(second, PAGE, 308), album_html()]) as download:
            self.assertEqual(discover_motion(ALIAS, "Artist", "Album"), STREAM)
            self.assertEqual(download.call_count, 3)
        with patch("apple_music_presence.motion_artwork._download",
                   side_effect=redirect(ALIAS, ALIAS)) as download:
            with self.assertRaises(HTTPError):
                discover_motion(ALIAS, "Artist", "Album")
            self.assertEqual(download.call_count, 3)

    def test_queries_are_removed_before_any_album_request(self):
        with patch("apple_music_presence.motion_artwork._download", side_effect=[
                redirect(ALIAS, PAGE + "?i=456&token=fixture"), album_html()]) as download:
            self.assertEqual(discover_motion(ALIAS + "?i=456", "Artist", "Album"), STREAM)
        self.assertEqual(download.call_args_list[0].args[0], ALIAS)
        self.assertEqual(download.call_args_list[1].args[0], PAGE)


if __name__ == "__main__":
    unittest.main()
