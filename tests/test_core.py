import unittest
from types import SimpleNamespace
from unittest.mock import patch

from apple_music_presence.models import MediaSnapshot, PlaybackState, Track
from apple_music_presence.presence import build_presence, materially_changed
from apple_music_presence.service import PresenceService


def snapshot(state=PlaybackState.PLAYING, position=10, observed_at=1000):
    return MediaSnapshot(Track("Song", "Artist", "Album"), state, position, 200, observed_at)


class PresenceTests(unittest.TestCase):
    def test_progress_advances_only_when_playing_and_clamps(self):
        self.assertEqual(snapshot().position_at(1010), 20)
        self.assertEqual(snapshot(PlaybackState.PAUSED).position_at(1010), 10)
        self.assertEqual(snapshot().position_at(5000), 200)
        self.assertEqual(snapshot().position_at(900), 10)

    def test_no_fabricated_timeline(self):
        unknown = MediaSnapshot(Track("X", "Y", ""), PlaybackState.PLAYING)
        payload = build_presence(unknown, 1000)
        self.assertNotIn("start", payload)
        self.assertGreaterEqual(len(payload["details"]), 2)

    def test_pause_clears_and_track_fields_and_timestamps(self):
        self.assertIsNone(build_presence(snapshot(PlaybackState.PAUSED), 1000))
        payload = build_presence(snapshot(), 1000)
        self.assertEqual(payload["name"], "Apple Music")
        self.assertEqual(payload["status_display_type"], 1)
        self.assertEqual(payload["details"], "Song")
        self.assertIn("Artist", payload["state"])
        self.assertEqual(payload["state"], "Artist")
        self.assertNotIn("small_image", payload)
        self.assertNotIn("small_text", payload)
        self.assertEqual((payload["start"], payload["end"]), (990, 1190))

    def test_member_list_uses_only_artist_while_profile_retains_app_name(self):
        for artist, expected in (("  The   Weeknd\n", "The Weeknd"),
                                 (" \t", "Apple Music"),
                                 ("X", "X\u200b"),
                                 ("A" * 200, "A" * 128)):
            with self.subTest(artist=artist):
                media = MediaSnapshot(Track("Song", artist, "Album"), PlaybackState.PLAYING)
                payload = build_presence(media, 1000)
                self.assertEqual(payload["name"], "Apple Music")
                self.assertEqual(payload["state"], expected)
                self.assertEqual(payload["status_display_type"], 1)

    def test_timer_jitter_does_not_flood_but_seek_does_update(self):
        old = build_presence(snapshot(), 1000)
        self.assertFalse(materially_changed(old, {**old, "start": 991, "end": 1191}))
        self.assertTrue(materially_changed(old, {**old, "start": 960, "end": 1160}))
        self.assertTrue(materially_changed(old, {"details": "Song", "state": "Artist · Album"}))


class FakeBackend:
    def __init__(self):
        self.value = snapshot()
        self.fail = False
        self.closed = False

    async def read(self):
        if self.fail:
            raise OSError("session disappeared")
        return self.value

    async def close(self):
        self.closed = True


class FakeRpc:
    def __init__(self):
        self.connected = False
        self.calls = []
        self.fail = False

    async def connect(self):
        self.calls.append("connect")
        if self.fail:
            raise OSError("Discord closed")
        self.connected = True

    async def update(self, payload):
        if self.fail:
            raise OSError("Discord closed")
        self.calls.append(payload)

    async def clear(self):
        self.calls.append("clear")

    async def close(self):
        self.calls.append("close")
        self.connected = False


class ServiceTests(unittest.IsolatedAsyncioTestCase):
    async def test_ready_motion_cover_upgrades_same_song_and_pause_clears_immediately(self):
        from unittest.mock import AsyncMock
        from apple_music_presence.artwork import Artwork
        static = Artwork("https://is1-ssl.mzstatic.com/cover.jpg", "https://music.apple.com/us/album/x/123")
        animated = Artwork("https://example.org/cover.webp", static.track_url, animated=True)
        resolver = SimpleNamespace(resolve=AsyncMock(return_value=static),
                                   refresh=AsyncMock(return_value=static), close=AsyncMock(),
                                   status="Motion cover: preparing animation; using normal artwork")
        self.service.artwork = resolver
        status = await self.service.tick()
        self.assertEqual(self.rpc.calls[-1]["large_image"], static.url)
        self.assertIn("preparing animation", status.artwork_status)
        resolver.refresh.return_value = animated
        self.clock_value += 2
        await self.service.tick()
        self.assertEqual(self.rpc.calls[-1]["large_image"], static.url)
        self.clock_value += 3
        status = await self.service.tick()
        self.assertEqual(self.rpc.calls[-1]["large_image"], animated.url)
        self.assertEqual(status.artwork_status, "Animated album art: sent to Discord")
        self.backend.value = snapshot(PlaybackState.PAUSED)
        await self.service.tick()
        self.assertEqual(self.rpc.calls[-1], "clear")
        await self.service.close()
        resolver.close.assert_awaited_once()
        self.service.artwork = None

    async def asyncSetUp(self):
        self.backend, self.rpc = FakeBackend(), FakeRpc()
        self.service = PresenceService(self.backend, self.rpc)
        # Replace the service's module binding, not Python's shared time module:
        # asyncio's own timeout/debug clock must continue running normally.
        self.clock_value = 100
        self.clock = patch("apple_music_presence.service.time", SimpleNamespace(
            monotonic=lambda: self.clock_value, time=lambda: 1000))
        self.clock.start()

    async def asyncTearDown(self):
        self.clock.stop()
        await self.service.close()

    async def test_pause_clear_and_resume(self):
        await self.service.tick()
        self.backend.value = snapshot(PlaybackState.PAUSED)
        await self.service.tick()
        self.assertEqual(self.rpc.calls[-1], "clear")
        self.backend.value = snapshot()
        self.clock_value += 5
        await self.service.tick()
        self.assertIsInstance(self.rpc.calls[-1], dict)

    async def test_failed_media_read_clears_stale_presence(self):
        await self.service.tick()
        self.backend.fail = True
        status = await self.service.tick()
        self.assertEqual(self.rpc.calls[-1], "clear")
        self.assertFalse(status.sharing)

    async def test_unchanged_payload_deduplicates_and_refresh_probes_connection(self):
        await self.service.tick()
        await self.service.tick()
        self.assertEqual(len(self.rpc.calls), 2)
        self.clock_value += 31
        await self.service.tick()
        self.assertEqual(len(self.rpc.calls), 3)

    async def test_reconnect_backoff_republishes_current_track(self):
        self.rpc.fail = True
        await self.service.tick()
        calls = len(self.rpc.calls)
        await self.service.tick()
        self.assertEqual(len(self.rpc.calls), calls)
        self.rpc.fail = False
        self.clock_value += 2
        status = await self.service.tick()
        self.assertTrue(status.sharing)
        self.assertIsInstance(self.rpc.calls[-1], dict)

    async def test_rapid_track_changes_coalesce_to_latest_within_rate_limit(self):
        await self.service.tick()
        self.backend.value = MediaSnapshot(Track("New", "Artist", "Album"),
                                           PlaybackState.PLAYING, 0, 100, 1000)
        self.clock_value += 2
        status = await self.service.tick()
        self.assertIn("queued", status.message)
        self.assertEqual(len(self.rpc.calls), 2)
        self.backend.value = MediaSnapshot(Track("Latest", "Artist", "Album"),
                                           PlaybackState.PLAYING, 0, 100, 1000)
        self.clock_value = 104.99
        status = await self.service.tick()
        self.assertIn("queued", status.message)
        self.assertEqual(len(self.rpc.calls), 2)
        self.clock_value = 105
        await self.service.tick()
        self.assertEqual(self.rpc.calls[-1]["details"], "Latest")

    async def test_continuous_skips_send_at_most_once_per_five_seconds(self):
        await self.service.tick()
        sent_at = [self.clock_value]
        for second in range(1, 21):
            self.clock_value = 100 + second
            self.backend.value = MediaSnapshot(Track(f"Song {second}", "Artist", "Album"),
                                               PlaybackState.PLAYING, 0, 100, 1000)
            calls_before = len(self.rpc.calls)
            await self.service.tick()
            if len(self.rpc.calls) != calls_before:
                sent_at.append(self.clock_value)
        self.assertEqual(sent_at, [100, 105, 110, 115, 120])
        self.assertEqual(self.rpc.calls[-1]["details"], "Song 20")

    async def test_artist_change_updates_member_status_after_rate_limit(self):
        await self.service.tick()
        self.backend.value = MediaSnapshot(Track("Song", "Another artist", "Album"),
                                           PlaybackState.PLAYING, 10, 200, 1000)
        self.clock_value += 5
        await self.service.tick()
        self.assertEqual(self.rpc.calls[-1]["name"], "Apple Music")
        self.assertEqual(self.rpc.calls[-1]["state"], "Another artist")
        self.assertEqual(self.rpc.calls[-1]["status_display_type"], 1)
        self.assertEqual(self.rpc.calls[-1]["details"], "Song")

    async def test_update_failure_backoff_increases_despite_successful_handshakes(self):
        from unittest.mock import AsyncMock
        self.rpc.update = AsyncMock(side_effect=OSError("rejected"))
        await self.service.tick()
        self.assertEqual(self.service._retry_delay, 2)
        self.clock_value += 2
        await self.service.tick()
        self.assertEqual(self.service._retry_delay, 4)

    async def test_cleanup_clears_activity_and_closes_backend(self):
        await self.service.tick()
        await self.service.close()
        self.assertEqual(self.rpc.calls[-2:], ["clear", "close"])
        self.assertTrue(self.backend.closed)

    async def test_artwork_resolution_is_published_and_status_reports_it(self):
        from unittest.mock import AsyncMock
        from apple_music_presence.artwork import Artwork
        art = Artwork("https://is1-ssl.mzstatic.com/cover.jpg", "https://music.apple.com/us/song/123")
        self.service.artwork = SimpleNamespace(resolve=AsyncMock(return_value=art))
        status = await self.service.tick()
        self.assertEqual(self.rpc.calls[-1]["large_image"], art.url)
        self.assertIn("sent to Discord", status.artwork_status)
        self.assertEqual(len(self.rpc.calls), 2)  # Connect, then one complete update.

    async def test_slow_artwork_does_not_block_track_and_publishes_later(self):
        import asyncio
        from apple_music_presence.artwork import Artwork
        ready = asyncio.Event()
        art = Artwork("https://is1-ssl.mzstatic.com/cover.jpg", "https://music.apple.com/us/song/123")

        async def resolve(*metadata):
            await ready.wait()
            return art

        self.service.artwork = SimpleNamespace(resolve=resolve)
        with patch("apple_music_presence.service.ARTWORK_GRACE_SECONDS", 0.01):
            status = await asyncio.wait_for(self.service.tick(), 0.5)
        self.assertIn("looking up", status.artwork_status)
        self.assertNotIn("large_image", self.rpc.calls[-1])
        self.assertFalse(self.service._art_task.cancelled())
        ready.set()
        await asyncio.sleep(0)
        self.clock_value += 1
        status = await self.service.tick()
        self.assertIn("found", status.artwork_status)
        self.assertEqual(len(self.rpc.calls), 2)
        self.clock_value += 4
        status = await self.service.tick()
        self.assertEqual(self.rpc.calls[-1]["large_image"], art.url)
        self.assertIn("sent to Discord", status.artwork_status)

    async def test_track_change_cancels_slow_old_cover_and_never_publishes_it(self):
        import asyncio
        from apple_music_presence.artwork import Artwork
        old_cover = Artwork("https://is1-ssl.mzstatic.com/old.jpg", "https://music.apple.com/us/song/1")
        new_cover = Artwork("https://is1-ssl.mzstatic.com/new.jpg", "https://music.apple.com/us/song/2")

        async def resolve(title, *metadata):
            if title == "Song":
                await asyncio.Future()
                return old_cover
            return new_cover

        self.service.artwork = SimpleNamespace(resolve=resolve)
        with patch("apple_music_presence.service.ARTWORK_GRACE_SECONDS", 0.01):
            await self.service.tick()
        old_task = self.service._art_task
        self.backend.value = MediaSnapshot(Track("New", "Artist", "Album"), PlaybackState.PLAYING)
        self.clock_value += 5
        await self.service.tick()
        self.assertTrue(old_task.cancelled())
        self.assertEqual(self.rpc.calls[-1]["details"], "New")
        self.assertEqual(self.rpc.calls[-1]["large_image"], new_cover.url)

    async def test_paused_new_track_does_not_wait_for_artwork(self):
        import asyncio

        async def resolve(*metadata):
            await asyncio.Future()

        self.service.artwork = SimpleNamespace(resolve=resolve)
        self.backend.value = snapshot(PlaybackState.PAUSED)
        with patch("apple_music_presence.service.asyncio.wait") as wait:
            await self.service.tick()
        wait.assert_not_called()
        self.assertEqual(self.rpc.calls[-1], "clear")

    async def test_completed_artwork_wakes_worker_before_long_poll_interval(self):
        import asyncio
        import threading
        from apple_music_presence.artwork import Artwork
        ready = asyncio.Event()
        stop = threading.Event()
        art = Artwork("https://is1-ssl.mzstatic.com/cover.jpg", "https://music.apple.com/us/song/123")

        async def resolve(*metadata):
            await ready.wait()
            return art

        def notify(status):
            if "looking up" in status.artwork_status:
                self.clock_value += 5
                ready.set()
            elif "sent to Discord" in status.artwork_status:
                stop.set()

        self.service.artwork = SimpleNamespace(resolve=resolve)
        self.service.notify = notify
        self.service.poll_interval = 10
        with patch("apple_music_presence.service.ARTWORK_GRACE_SECONDS", 0.01):
            await asyncio.wait_for(self.service.run(stop), 2)
        updates = [call for call in self.rpc.calls if isinstance(call, dict)]
        self.assertEqual(len(updates), 2)
        self.assertEqual(updates[-1]["large_image"], art.url)

    async def test_disabled_artwork_is_visible_in_status(self):
        status = await self.service.tick()
        self.assertIn("off", status.artwork_status)

    async def test_transient_artwork_failure_recovers_without_changing_song(self):
        import asyncio
        from unittest.mock import AsyncMock
        from apple_music_presence.artwork import Artwork
        art = Artwork("https://is1-ssl.mzstatic.com/cover.jpg", "https://music.apple.com/us/song/123")
        resolve = AsyncMock(side_effect=[OSError("Temporary failure"), art])
        self.service.artwork = SimpleNamespace(resolve=resolve)
        status = await self.service.tick()
        self.assertIn("retrying automatically", status.artwork_status)
        self.assertNotIn("large_image", self.rpc.calls[-1])
        self.clock_value = 109.99
        await self.service.tick()
        self.assertEqual(resolve.await_count, 1)
        self.clock_value = 110
        await self.service.tick()
        await asyncio.sleep(0)
        status = await self.service.tick()
        self.assertEqual(resolve.await_count, 2)
        self.assertEqual(self.rpc.calls[-1]["details"], "Song")
        self.assertEqual(self.rpc.calls[-1]["large_image"], art.url)
        self.assertIn("sent to Discord", status.artwork_status)

    async def test_refresh_failure_keeps_sharing_and_can_upgrade_later(self):
        from unittest.mock import AsyncMock
        from apple_music_presence.artwork import Artwork
        static = Artwork("https://is1-ssl.mzstatic.com/cover.jpg", "https://music.apple.com/us/album/x/123")
        animated = Artwork("https://example.org/cover.webp", static.track_url, True)
        self.service.artwork = SimpleNamespace(resolve=AsyncMock(return_value=static),
                                              refresh=AsyncMock(side_effect=[OSError("unavailable"), animated]))
        status = await self.service.tick()
        self.assertTrue(status.sharing)
        self.assertEqual(self.rpc.calls[-1]["large_image"], static.url)
        self.clock_value += 5
        await self.service.tick()
        self.assertEqual(self.rpc.calls[-1]["large_image"], animated.url)
        self.backend.value = snapshot(PlaybackState.PAUSED)
        status = await self.service.tick()
        self.assertEqual(self.rpc.calls[-1], "clear")
        self.assertIn("resumes with playback", status.artwork_status)

    async def test_cancelled_optional_lookup_retries_without_stopping_sharing(self):
        import asyncio
        from unittest.mock import AsyncMock
        from apple_music_presence.artwork import Artwork
        art = Artwork("https://is1-ssl.mzstatic.com/cover.jpg", "https://music.apple.com/us/album/x/123")
        resolve = AsyncMock(side_effect=[asyncio.CancelledError(), art])
        self.service.artwork = SimpleNamespace(resolve=resolve)
        status = await self.service.tick()
        self.assertTrue(status.sharing)
        self.assertNotIn("large_image", self.rpc.calls[-1])
        self.clock_value += 10
        await self.service.tick()
        await asyncio.sleep(0)
        await self.service.tick()
        self.assertEqual(self.rpc.calls[-1]["large_image"], art.url)

    async def test_cancelling_bridge_during_refresh_still_propagates(self):
        import asyncio
        from unittest.mock import AsyncMock
        from apple_music_presence.artwork import Artwork
        art = Artwork("https://is1-ssl.mzstatic.com/cover.jpg", "https://music.apple.com/us/album/x/123")
        self.service.artwork = SimpleNamespace(resolve=AsyncMock(return_value=art),
                                              refresh=AsyncMock(side_effect=asyncio.CancelledError))
        with self.assertRaises(asyncio.CancelledError):
            await self.service.tick()

    async def test_artwork_failures_back_off_to_one_attempt_per_minute(self):
        import asyncio
        from unittest.mock import AsyncMock
        resolve = AsyncMock(return_value=None)
        self.service.artwork = SimpleNamespace(resolve=resolve)
        await self.service.tick()
        for expected_attempts, next_attempt in enumerate((110, 130, 170, 230, 290), start=2):
            self.clock_value = next_attempt - 0.01
            await self.service.tick()
            self.assertEqual(resolve.await_count, expected_attempts - 1)
            self.clock_value = next_attempt
            await self.service.tick()
            await asyncio.sleep(0)
            await self.service.tick()
            self.assertEqual(resolve.await_count, expected_attempts)

    async def test_artwork_retry_waits_for_resume(self):
        import asyncio
        from unittest.mock import AsyncMock
        resolve = AsyncMock(return_value=None)
        self.service.artwork = SimpleNamespace(resolve=resolve)
        await self.service.tick()
        self.clock_value = 200
        self.backend.value = snapshot(PlaybackState.PAUSED)
        status = await self.service.tick()
        self.assertEqual(resolve.await_count, 1)
        self.assertIn("resumes with playback", status.artwork_status)
        self.backend.value = snapshot()
        await self.service.tick()
        await asyncio.sleep(0)
        await self.service.tick()
        self.assertEqual(resolve.await_count, 2)

    async def test_new_track_does_not_inherit_artwork_retry_delay(self):
        from unittest.mock import AsyncMock
        from apple_music_presence.artwork import Artwork
        art = Artwork("https://is1-ssl.mzstatic.com/cover.jpg", "https://music.apple.com/us/song/123")
        resolve = AsyncMock(side_effect=[None, art])
        self.service.artwork = SimpleNamespace(resolve=resolve)
        await self.service.tick()
        self.clock_value += 5
        self.backend.value = MediaSnapshot(Track("New", "Artist", "Album"), PlaybackState.PLAYING)
        await self.service.tick()
        self.assertEqual(resolve.await_count, 2)
        self.assertEqual(self.rpc.calls[-1]["details"], "New")
        self.assertEqual(self.rpc.calls[-1]["large_image"], art.url)


if __name__ == "__main__":
    unittest.main()
