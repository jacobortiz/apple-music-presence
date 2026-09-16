"""Bounded, reconnectable Discord IPC adapter using pypresence 4.6.2.

Only the local Discord desktop pipe and a public application ID are used. This
module does not authenticate a user account or need any Discord token.
"""

from __future__ import annotations

import asyncio
import json
import math
import os
import struct
import sys
import uuid
from collections.abc import Awaitable, Callable, Mapping
from typing import Any

from pypresence import AioPresence
from pypresence.exceptions import DiscordNotFound, InvalidPipe, ServerError
from pypresence.types import ActivityType


class DiscordRpcError(RuntimeError):
    """A connection or request failed; a fresh connect() may be attempted."""


class _WindowsAioPresence(AioPresence):
    """Contain compatibility fixes for the pinned pypresence release.

The release uses synchronous pipe discovery, read() instead of readexactly(),
and close() closes the caller's event loop. Use its payload builder and async
pipe setup, with bounded discovery and complete-frame handling here. The
outer DiscordRpc adapter supplies a deadline for the entire request.
"""

    _expected_nonce: str | None = None

    async def handshake(self) -> None:
        if sys.platform != "win32":
            raise DiscordRpcError("Discord IPC for this app requires Windows.")

        # Avoid pypresence's synchronous open/scandir probe. Windows' proactor
        # pipe connection is asynchronous and safely cancellable.
        for index in range(10):
            try:
                await self.create_reader_writer(rf"\\?\pipe\discord-ipc-{index}")
            except (InvalidPipe, FileNotFoundError, PermissionError):
                continue
            break
        else:
            raise DiscordNotFound

        self.send_data(0, {"v": 1, "client_id": self.client_id})
        response = await self.read_output()
        if response.get("evt") != "READY":
            raise DiscordRpcError("Discord did not complete the IPC handshake.")

    def send_data(self, op: int, payload: Any) -> None:
        data = getattr(payload, "data", payload)
        if op == 1:
            self._expected_nonce = data.get("nonce")
        super().send_data(op, payload)

    async def read_output(self) -> dict[str, Any]:
        reader = self.sock_reader
        if reader is None:
            raise DiscordRpcError("Discord pipe is not connected.")
        while True:
            opcode, length = struct.unpack("<II", await reader.readexactly(8))
            if length > 1_048_576:
                raise DiscordRpcError("Discord returned an oversized IPC frame.")
            body = await reader.readexactly(length)
            if opcode == 3:  # PING payloads must be echoed byte-for-byte.
                self.sock_writer.write(struct.pack("<II", 4, length) + body)
                continue
            if opcode == 2:
                try:
                    reason = json.loads(body).get("message", "Connection closed")
                except (ValueError, AttributeError):
                    reason = "Connection closed"
                raise DiscordRpcError(f"Discord closed the IPC connection: {reason}")
            if opcode == 4:
                continue
            if opcode != 1:
                raise DiscordRpcError("Discord returned an unknown IPC opcode.")
            response = json.loads(body)
            if not isinstance(response, dict):
                raise DiscordRpcError("Discord returned an invalid IPC response.")
            if response.get("evt") == "ERROR":
                data = response.get("data") or {}
                raise ServerError(data.get("message", "Discord rejected the request."))
            # Unsolicited events must not count as command acknowledgements.
            if self._expected_nonce is not None:
                if response.get("nonce") != self._expected_nonce:
                    continue
                self._expected_nonce = None
            return response

    async def clear(self, pid: int = os.getpid()) -> dict[str, Any]:
        # pypresence 4.6.2's generic remove_none strips activity=None. Preserve
        # explicit null, as required by Discord's SET_ACTIVITY clear command.
        self.send_data(
            1,
            {
                "cmd": "SET_ACTIVITY",
                "args": {"pid": pid, "activity": None},
                "nonce": uuid.uuid4().hex,
            },
        )
        return await self.read_output()


class DiscordRpc:
    """One serialized IPC connection; retry/backoff belongs to the service.

    Every failed or cancelled operation drops the old pipe, so connect() can
    safely construct a fresh client. All methods run on the same asyncio loop.
    update() accepts pypresence keyword arguments as a mapping. The service
    should refresh at intervals (e.g. 30 seconds) to detect an idle disconnect.
    """

    def __init__(
        self,
        client_id: str,
        timeout: float = 5.0,
        *,
        client_factory: Callable[..., Any] | None = None,
    ) -> None:
        if not client_id or not client_id.isascii() or not client_id.isdecimal():
            raise ValueError("Discord application ID must contain only digits.")
        if not math.isfinite(timeout) or timeout <= 0:
            raise ValueError("Discord timeout must be finite and positive.")
        self.client_id = client_id
        self.timeout = timeout
        self._client_factory = client_factory or _WindowsAioPresence
        self._client: Any = None
        self._connected = False
        self._lock = asyncio.Lock()

    @property
    def connected(self) -> bool:
        return self._connected

    def _disconnect(self) -> None:
        client, self._client = self._client, None
        self._connected = False
        writer = getattr(client, "sock_writer", None)
        if writer is not None:
            try:
                # Windows stores a pipe transport here; Unix pypresence stores
                # a StreamWriter. Both have close(). Never close the app loop.
                writer.close()
            except Exception:
                pass  # A broken transport must not block shutdown/reconnect.

    async def _request(
        self, operation: Callable[[], Awaitable[Any]], label: str
    ) -> None:
        try:
            await asyncio.wait_for(operation(), timeout=self.timeout)
        except asyncio.CancelledError:
            self._disconnect()
            raise
        except Exception as exc:
            self._disconnect()
            message = str(exc) or type(exc).__name__
            raise DiscordRpcError(f"Discord {label} failed: {message}") from exc

    async def connect(self) -> None:
        async with self._lock:
            if self._connected:
                return
            try:
                self._client = self._client_factory(
                    self.client_id,
                    loop=asyncio.get_running_loop(),
                    connection_timeout=self.timeout,
                    response_timeout=self.timeout,
                )
            except Exception as exc:
                raise DiscordRpcError(f"Could not create Discord client: {exc}") from exc
            await self._request(self._client.connect, "connection")
            self._connected = True

    async def update(self, payload: Mapping[str, Any]) -> None:
        async with self._lock:
            if not self._connected:
                raise DiscordRpcError("Discord is not connected.")
            options = dict(payload)
            options["activity_type"] = ActivityType.LISTENING
            await self._request(lambda: self._client.update(**options), "update")

    async def clear(self) -> None:
        async with self._lock:
            if not self._connected:
                return
            await self._request(self._client.clear, "clear")

    async def close(self) -> None:
        async with self._lock:
            self._disconnect()
        # Give pending Windows transport-close callbacks a turn to release the
        # handle before an application-owned loop is subsequently shut down.
        await asyncio.sleep(0)
