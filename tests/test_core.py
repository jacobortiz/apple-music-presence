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
        self.assertEqual(payload["details"], "Song")
        self.assertIn("Artist", payload["state"])
        self.assertIn("Album", payload["state"])
        self.assertEqual((payload["start"], payload["end"]), (990, 1190))

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
        self.clock_value += 16
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
        self.clock_value += 13
        await self.service.tick()
        self.assertEqual(self.rpc.calls[-1]["details"], "New")

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
        import asyncio
        from unittest.mock import AsyncMock
        from apple_music_presence.artwork import Artwork
        art = Artwork("https://is1-ssl.mzstatic.com/cover.jpg", "https://music.apple.com/us/song/123")
        self.service.artwork = SimpleNamespace(resolve=AsyncMock(return_value=art))
        status = await self.service.tick()
        self.assertIn("looking up", status.artwork_status)
        await asyncio.sleep(0)
        status = await self.service.tick()
        self.assertIn("found", status.artwork_status)
        self.clock_value += 15
        status = await self.service.tick()
        self.assertEqual(self.rpc.calls[-1]["large_image"], art.url)
        self.assertIn("sent to Discord", status.artwork_status)

    async def test_disabled_artwork_is_visible_in_status(self):
        status = await self.service.tick()
        self.assertIn("off", status.artwork_status)


if __name__ == "__main__":
    unittest.main()
