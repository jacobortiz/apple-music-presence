from __future__ import annotations

import asyncio
import json
import struct
import unittest
from unittest.mock import AsyncMock, patch

from pypresence.exceptions import InvalidPipe

from apple_music_presence.discord_rpc import (
    DiscordRpc,
    DiscordRpcError,
    _WindowsAioPresence,
)


class FakeWriter:
    def __init__(self):
        self.closed = False
        self.frames = []

    def close(self):
        self.closed = True

    def write(self, frame):
        self.frames.append(frame)


class FakePresence:
    def __init__(self, client_id, **kwargs):
        self.client_id = client_id
        self.options = kwargs
        self.sock_writer = FakeWriter()
        self.updates = []
        self.clears = 0
        self.error = None
        self.hang = False

    async def connect(self):
        if self.hang:
            await asyncio.Future()
        if self.error:
            raise self.error

    async def update(self, **payload):
        if self.hang:
            await asyncio.Future()
        if self.error:
            raise self.error
        self.updates.append(payload)

    async def clear(self):
        self.clears += 1

    def close(self):
        raise AssertionError("The adapter must not call pypresence.close().")


def frame(payload, opcode=1):
    body = json.dumps(payload).encode()
    return struct.pack("<II", opcode, len(body)) + body


class DiscordRpcTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.clients = []

        def factory(client_id, **kwargs):
            client = FakePresence(client_id, **kwargs)
            self.clients.append(client)
            return client

        self.rpc = DiscordRpc("123456789012345678", 0.05, client_factory=factory)

    async def asyncTearDown(self):
        await self.rpc.close()

    async def test_update_forces_listening_without_mutating_input(self):
        await self.rpc.connect()
        payload = {"details": "A song", "state": "An artist", "activity_type": 0}
        await self.rpc.update(payload)
        self.assertEqual(self.clients[0].updates[0]["activity_type"], 2)
        self.assertEqual(payload["activity_type"], 0)
        self.assertTrue(self.rpc.connected)

    async def test_connect_is_idempotent_and_passes_deadlines(self):
        await self.rpc.connect()
        await self.rpc.connect()
        self.assertEqual(len(self.clients), 1)
        self.assertIs(self.clients[0].options["loop"], asyncio.get_running_loop())
        self.assertEqual(self.clients[0].options["response_timeout"], 0.05)

    async def test_broken_connection_is_discarded_and_can_reconnect(self):
        await self.rpc.connect()
        self.clients[0].error = BrokenPipeError("Discord exited")
        with self.assertRaisesRegex(DiscordRpcError, "Discord exited"):
            await self.rpc.update({"details": "Track"})
        self.assertFalse(self.rpc.connected)
        self.assertTrue(self.clients[0].sock_writer.closed)
        await self.rpc.connect()
        await self.rpc.update({"details": "Track"})
        self.assertEqual(len(self.clients), 2)

    async def test_hung_request_times_out_and_drops_pipe(self):
        await self.rpc.connect()
        self.clients[0].hang = True
        with self.assertRaises(DiscordRpcError):
            await asyncio.wait_for(self.rpc.update({"details": "Track"}), 0.5)
        self.assertFalse(self.rpc.connected)
        self.assertTrue(self.clients[0].sock_writer.closed)

    async def test_hung_handshake_times_out_and_drops_partial_pipe(self):
        client = FakePresence("123")
        client.hang = True
        rpc = DiscordRpc("123", 0.03, client_factory=lambda *a, **k: client)
        with self.assertRaises(DiscordRpcError):
            await asyncio.wait_for(rpc.connect(), 0.5)
        self.assertFalse(rpc.connected)
        self.assertTrue(client.sock_writer.closed)

    async def test_cancelled_request_drops_pipe(self):
        await self.rpc.connect()
        self.clients[0].hang = True
        request = asyncio.create_task(self.rpc.update({"details": "Track"}))
        await asyncio.sleep(0)
        request.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await request
        self.assertFalse(self.rpc.connected)
        self.assertTrue(self.clients[0].sock_writer.closed)

    async def test_clear_and_close_preserve_running_event_loop(self):
        await self.rpc.clear()  # Already disconnected is harmless.
        await self.rpc.connect()
        await self.rpc.clear()
        self.assertEqual(self.clients[0].clears, 1)
        await self.rpc.close()
        await self.rpc.close()
        self.assertFalse(asyncio.get_running_loop().is_closed())
        self.assertTrue(self.clients[0].sock_writer.closed)

    async def test_disconnected_update_requires_connect(self):
        with self.assertRaisesRegex(DiscordRpcError, "not connected"):
            await self.rpc.update({})

    def test_invalid_configuration_is_rejected(self):
        for client_id in ("", "not-an-id", "١٢٣"):
            with self.assertRaises(ValueError):
                DiscordRpc(client_id)
        for timeout in (0, -1, float("inf"), float("nan")):
            with self.assertRaises(ValueError):
                DiscordRpc("123", timeout)


class PypresenceContractTests(unittest.IsolatedAsyncioTestCase):
    """Exercise actual 4.6.2 payloads with a simulated local Discord stream."""

    def setUp(self):
        self.client = _WindowsAioPresence("123456789012345678", loop=asyncio.get_event_loop())
        self.client.sock_reader = asyncio.StreamReader()
        self.client.sock_writer = FakeWriter()

    async def test_real_library_serializes_listening_timestamps_and_image_url(self):
        task = asyncio.create_task(
            self.client.update(
                activity_type=2,
                details="A song 🎵",
                state="Artist • Album",
                start=1700000000,
                end=1700000123,
                large_image="https://example.org/cover.jpg",
            )
        )
        await asyncio.sleep(0)
        sent = self.client.sock_writer.frames[0]
        opcode, size = struct.unpack("<II", sent[:8])
        self.assertEqual(size, len(sent[8:]))
        self.assertEqual(opcode, 1)
        payload = json.loads(sent[8:])
        activity = payload["args"]["activity"]
        self.assertEqual(activity["type"], 2)
        self.assertEqual(activity["details"], "A song 🎵")
        self.assertEqual(activity["timestamps"], {"start": 1700000000, "end": 1700000123})
        self.assertEqual(activity["assets"]["large_image"], "https://example.org/cover.jpg")
        self.client.sock_reader.feed_data(frame({"nonce": payload["nonce"], "evt": None}))
        await task

    async def test_clear_includes_explicit_null_activity(self):
        task = asyncio.create_task(self.client.clear())
        await asyncio.sleep(0)
        payload = json.loads(self.client.sock_writer.frames[0][8:])
        self.assertIn("activity", payload["args"])
        self.assertIsNone(payload["args"]["activity"])
        self.client.sock_reader.feed_data(frame({"nonce": payload["nonce"], "evt": None}))
        await task

    async def test_fragmented_frames_and_ping_do_not_break_response(self):
        self.client._expected_nonce = "wanted"
        task = asyncio.create_task(self.client.read_output())
        data = frame({"x": "ping"}, 3) + frame({"evt": "IGNORED"}) + frame({"nonce": "wanted"})
        for offset in range(0, len(data), 3):
            self.client.sock_reader.feed_data(data[offset:offset + 3])
            await asyncio.sleep(0)
        self.assertEqual(await task, {"nonce": "wanted"})
        self.assertEqual(self.client.sock_writer.frames[0], frame({"x": "ping"}, 4))

    async def test_handshake_probes_async_pipes_and_requires_ready(self):
        connect = AsyncMock(side_effect=[InvalidPipe(), None])
        self.client.sock_reader.feed_data(frame({"evt": "READY"}))
        with patch("apple_music_presence.discord_rpc.sys.platform", "win32"):
            with patch.object(self.client, "create_reader_writer", connect):
                await self.client.handshake()
        self.assertEqual(connect.await_count, 2)
        self.assertEqual(connect.await_args_list[0].args, (r"\\?\pipe\discord-ipc-0",))
        self.assertEqual(connect.await_args_list[1].args, (r"\\?\pipe\discord-ipc-1",))
        payload = json.loads(self.client.sock_writer.frames[0][8:])
        self.assertEqual(payload, {"v": 1, "client_id": "123456789012345678"})

    async def test_discord_rejection_has_readable_error(self):
        self.client.sock_reader.feed_data(frame({"message": "Invalid Client ID"}, 2))
        with self.assertRaisesRegex(DiscordRpcError, "Invalid Client ID"):
            await self.client.read_output()

    async def test_closed_pipe_and_oversized_frame_fail(self):
        self.client.sock_reader.feed_data(frame({}, 2))
        with self.assertRaisesRegex(DiscordRpcError, "closed"):
            await self.client.read_output()
        self.client.sock_reader.feed_data(struct.pack("<II", 1, 2_000_000))
        with self.assertRaisesRegex(DiscordRpcError, "oversized"):
            await self.client.read_output()


if __name__ == "__main__":
    unittest.main()
