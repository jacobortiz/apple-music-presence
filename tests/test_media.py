from datetime import datetime, timedelta, timezone
from types import SimpleNamespace
import unittest
from unittest.mock import AsyncMock, patch

from apple_music_presence.media import (
    MediaBackendError,
    WindowsMediaBackend,
    _rate,
    _artist_album,
    _timeline,
    is_apple_music_source,
)
from apple_music_presence.models import PlaybackState


APPLE = "AppleInc.AppleMusicWin_nzyj5cx40ttqa!App"
NOW = 1_700_000_000.0


def make_timeline(position=50, start=10, end=210, updated=NOW - 5):
    return SimpleNamespace(
        position=timedelta(seconds=position), start_time=timedelta(seconds=start),
        end_time=timedelta(seconds=end),
        last_updated_time=datetime.fromtimestamp(updated, timezone.utc),
    )


def make_session(source=APPLE, status=4, title="Song", rate=1.0):
    info = SimpleNamespace(playback_status=status, playback_rate=rate)
    return SimpleNamespace(
        source_app_user_model_id=source,
        get_playback_info=lambda: info,
        try_get_media_properties_async=AsyncMock(return_value=SimpleNamespace(
            title=title, artist=" Artist ", album_title=" Album ")),
        get_timeline_properties=lambda: make_timeline(),
    )


class TimelineTests(unittest.TestCase):
    def test_combined_apple_music_artist_album(self):
        self.assertEqual(_artist_album("The 1975 — The 1975", "", APPLE), ("The 1975", "The 1975"))

    def test_explicit_album_and_other_players_are_not_reinterpreted(self):
        self.assertEqual(_artist_album("Artist — Alias", "Album", APPLE), ("Artist — Alias", "Album"))
        self.assertEqual(_artist_album("Artist — Album", "", "chrome.exe"), ("Artist — Album", ""))

    def test_ambiguous_or_incomplete_combined_fields_are_preserved(self):
        for artist in ("Artist — Album — Live", " — Album", "Artist — ", "Artist - Album"):
            self.assertEqual(_artist_album(artist, "", APPLE), (artist, ""))

    def test_normalizes_offset_and_advances_from_last_updated_at_rate(self):
        position, duration = _timeline(make_timeline(), PlaybackState.PLAYING, 2, NOW)
        self.assertEqual((position, duration), (50, 200))

    def test_paused_position_does_not_advance(self):
        self.assertEqual(_timeline(make_timeline(), PlaybackState.PAUSED, 1, NOW),
                         (40, 200))

    def test_clamps_progress_to_song_bounds(self):
        self.assertEqual(_timeline(make_timeline(position=1000),
                                   PlaybackState.PLAYING, 1, NOW), (200, 200))
        self.assertEqual(_timeline(make_timeline(position=-10),
                                   PlaybackState.PAUSED, 1, NOW), (0, 200))

    def test_naive_timestamp_is_utc_and_future_time_does_not_rewind(self):
        timeline = make_timeline()
        timeline.last_updated_time = datetime.fromtimestamp(NOW - 5, timezone.utc).replace(tzinfo=None)
        self.assertEqual(_timeline(timeline, PlaybackState.PLAYING, 1, NOW)[0], 45)
        timeline.last_updated_time = datetime.fromtimestamp(NOW + 5, timezone.utc)
        self.assertEqual(_timeline(timeline, PlaybackState.PLAYING, 1, NOW)[0], 40)

    def test_uninitialized_timestamp_and_duration_are_not_extrapolated(self):
        timeline = make_timeline(start=0, position=0, end=0)
        timeline.last_updated_time = datetime(1601, 1, 1, tzinfo=timezone.utc)
        self.assertEqual(_timeline(timeline, PlaybackState.PLAYING, 1, NOW), (0, None))

    def test_optional_or_invalid_rate_defaults_to_normal(self):
        for rate in (None, float("nan"), float("inf"), 0, -1):
            self.assertEqual(_rate(rate), 1)
        self.assertEqual(_rate(1.5), 1.5)

    def test_only_native_apple_music_matches(self):
        for source in (APPLE, "AppleMusic.exe", r"C:\Apple Music\AppleMusic.exe"):
            self.assertTrue(is_apple_music_source(source), source)
        for source in ("Spotify.exe", "chrome.exe", "msedge.exe", "AppleMusic",
                       "My.AppleMusic.exe", "AppleInc.AppleMusicWinFake!App"):
            self.assertFalse(is_apple_music_source(source), source)


class BackendTests(unittest.IsolatedAsyncioTestCase):
    async def test_actual_windows_apple_music_metadata_shape(self):
        session = make_session(title="Robbers")
        session.try_get_media_properties_async.return_value = SimpleNamespace(
            title="Robbers", artist="The 1975 — The 1975", album_title="")
        track = (await self.backend([session]).read()).track
        self.assertEqual((track.title, track.artist, track.album), ("Robbers", "The 1975", "The 1975"))

    def backend(self, sessions, source_id=None):
        backend = WindowsMediaBackend(source_id)
        manager = SimpleNamespace(get_sessions=lambda: sessions)
        backend._ensure_manager = AsyncMock(return_value=manager)
        return backend

    async def test_prefers_playing_apple_session_and_ignores_other_apps(self):
        paused = make_session(status=5, title="Paused")
        playing = make_session(title="Playing")
        backend = self.backend([make_session("Spotify.exe"), paused, playing])
        with patch("apple_music_presence.media.time.time", return_value=NOW):
            snapshot = await backend.read()
        self.assertEqual(snapshot.track.title, "Playing")
        self.assertEqual(snapshot.track.artist, "Artist")
        self.assertEqual(snapshot.track.album, "Album")
        self.assertEqual(snapshot.position, 45)
        self.assertEqual(snapshot.observed_at, NOW)

    async def test_no_apple_music_clears_track(self):
        snapshot = await self.backend([make_session("chrome.exe")]).read()
        self.assertIsNone(snapshot.track)
        self.assertEqual(snapshot.state, PlaybackState.STOPPED)

    async def test_exact_override_requires_entire_source_id(self):
        backend = self.backend([make_session("custom-player")], "custom")
        self.assertIsNone((await backend.read()).track)
        backend.source_id = "custom-player"
        self.assertEqual((await backend.read()).source_id, "custom-player")

    async def test_stop_does_not_request_stale_metadata(self):
        session = make_session(status=3)
        snapshot = await self.backend([session]).read()
        self.assertIsNone(snapshot.track)
        session.try_get_media_properties_async.assert_not_called()

    async def test_blank_title_clears_presence(self):
        snapshot = await self.backend([make_session(title=" ")]).read()
        self.assertIsNone(snapshot.track)
        self.assertEqual(snapshot.state, PlaybackState.STOPPED)

    async def test_reenumerates_sessions_on_each_poll(self):
        sessions = [make_session()]
        backend = self.backend(sessions)
        self.assertIsNotNone((await backend.read()).track)
        sessions.clear()
        self.assertIsNone((await backend.read()).track)
        sessions.append(make_session(title="New song"))
        self.assertEqual((await backend.read()).track.title, "New song")

    async def test_transient_failure_rediscovers_manager(self):
        backend = self.backend([make_session()])
        manager = backend._ensure_manager.return_value
        backend._ensure_manager.side_effect = [OSError("closed session"), manager]
        self.assertIsNotNone((await backend.read()).track)
        self.assertEqual(backend._ensure_manager.await_count, 2)

    async def test_persistent_failure_is_actionable_and_discards_manager(self):
        backend = self.backend([])
        backend._manager = object()
        backend._ensure_manager.side_effect = TimeoutError
        with self.assertRaisesRegex(MediaBackendError, "unavailable"):
            await backend.read()
        self.assertIsNone(backend._manager)

    async def test_list_sessions_returns_unique_source_ids(self):
        backend = self.backend([make_session(), make_session(), make_session("chrome.exe")])
        self.assertEqual(await backend.list_sessions(), [APPLE, "chrome.exe"])


if __name__ == "__main__":
    unittest.main()
