"""Coordinates detection, optional enrichment, and reconnecting Discord updates."""

import asyncio
from dataclasses import dataclass
import logging
import threading
import time
from typing import Callable

from .models import MediaBackend, MediaSnapshot, PlaybackState
from .presence import build_presence, materially_changed

log = logging.getLogger(__name__)

# Send changed metadata sooner; Discord may still coalesce visible updates.
UPDATE_INTERVAL_SECONDS = 5.0
# Give cached/fast covers a chance to join the first track update, rather than
# sending text alone and then waiting another update interval for the cover.
ARTWORK_GRACE_SECONDS = 0.75
ARTWORK_RETRY_SECONDS = 10.0
ARTWORK_MAX_RETRY_SECONDS = 60.0


@dataclass(frozen=True)
class ServiceStatus:
    snapshot: MediaSnapshot
    message: str
    connected: bool = False
    sharing: bool = False
    artwork_status: str = "Album art is off"


class PresenceService:
    def __init__(self, backend: MediaBackend, rpc, *, artwork=None,
                 notify: Callable[[ServiceStatus], None] = lambda status: None,
                 poll_interval: float = 1.0):
        self.backend, self.rpc, self.artwork = backend, rpc, artwork
        self.notify, self.poll_interval = notify, poll_interval
        self._last_payload: dict | None = None
        self._last_sent = float("-inf")
        self._next_connect = 0.0
        self._retry_delay = 1.0
        self._art_track = None
        self._art_task: asyncio.Task | None = None
        self._art_result = None
        self._art_next_retry = 0.0
        self._art_retry_delay = ARTWORK_RETRY_SECONDS

    async def _enrich(self, snapshot: MediaSnapshot):
        track = snapshot.track
        new_track = track != self._art_track
        if new_track:
            if self._art_task:
                self._art_task.cancel()
                await asyncio.gather(self._art_task, return_exceptions=True)
            self._art_task, self._art_result = None, None
            self._art_track = track
            self._art_next_retry = 0.0
            self._art_retry_delay = ARTWORK_RETRY_SECONDS
        if (track and self.artwork and snapshot.state == PlaybackState.PLAYING
                and self._art_task is None and self._art_result is None
                and time.monotonic() >= self._art_next_retry):
            self._art_task = asyncio.create_task(
                self.artwork.resolve(track.title, track.artist, track.album))
        if (new_track and self._art_task and snapshot.state == PlaybackState.PLAYING
                and time.monotonic() - self._last_sent >= UPDATE_INTERVAL_SECONDS):
            # asyncio.wait leaves a slow lookup running after the grace period.
            # Pausing/stopping never waits for artwork, and update throttling
            # already gives queued tracks time to finish their lookup.
            await asyncio.wait({self._art_task}, timeout=ARTWORK_GRACE_SECONDS)
        if self._art_task and self._art_task.done():
            try:
                self._art_result = self._art_task.result()
            except asyncio.CancelledError:
                # A cancelled optional lookup is not a cancelled bridge.
                # Cancellation of tick/run still propagates at their awaits.
                log.debug("Optional artwork lookup was cancelled; retrying later")
            except Exception:
                log.debug("Optional artwork lookup failed", exc_info=True)
            self._art_task = None
            if self._art_result is None:
                self._art_next_retry = time.monotonic() + self._art_retry_delay
                self._art_retry_delay = min(ARTWORK_MAX_RETRY_SECONDS, self._art_retry_delay * 2)
        if (track and self._art_result and snapshot.state == PlaybackState.PLAYING
                and not getattr(self._art_result, "animated", False)
                and hasattr(self.artwork, "refresh")):
            try:
                upgraded = await self.artwork.refresh(track.title, track.artist, track.album)
                if upgraded:
                    self._art_result = upgraded
            except Exception:
                # Keep the already matched cover and ordinary sharing alive.
                log.debug("Optional artwork refresh failed", exc_info=True)
        return self._art_result

    async def tick(self) -> ServiceStatus:
        """One poll, exposed independently for deterministic failure/recovery tests."""
        detection_error = False
        try:
            snapshot = await self.backend.read()
        except Exception:
            detection_error = True
            log.debug("Media read failed; clearing stale presence", exc_info=True)
            snapshot = MediaSnapshot(None, PlaybackState.STOPPED)
        artwork = await self._enrich(snapshot)
        payload = build_presence(snapshot, time.time(), artwork)
        now = time.monotonic()
        message = "Waiting for Apple Music"
        if detection_error:
            message = "Media unavailable; retrying automatically"
        elif snapshot.state == PlaybackState.PAUSED:
            message = "Paused — Discord presence cleared"
        elif payload:
            message = "Sharing with Discord"

        try:
            if not self.rpc.connected and now >= self._next_connect:
                await self.rpc.connect()
                # New pipes never inherit our local assumption about activity.
                self._last_payload = None
                self._last_sent = float("-inf")
            if self.rpc.connected:
                if payload is None:
                    if self._last_payload is not None or self._last_sent == float("-inf"):
                        await self.rpc.clear()
                        self._retry_delay = 1.0
                        self._last_payload = None
                        self._last_sent = now
                    elif now - self._last_sent >= 30:
                        # Acknowledged clears also probe an idle pipe for disconnects.
                        await self.rpc.clear()
                        self._retry_delay = 1.0
                        self._last_sent = now
                elif (now - self._last_sent >= UPDATE_INTERVAL_SECONDS and
                      (materially_changed(self._last_payload, payload) or now - self._last_sent >= 30)):
                    await self.rpc.update(payload)
                    self._retry_delay = 1.0
                    self._last_payload = payload
                    self._last_sent = now
                if payload is not None and materially_changed(self._last_payload, payload):
                    message = "Track update queued for Discord"
        except Exception as exc:
            log.debug("Discord RPC unavailable", exc_info=True)
            await self.rpc.close()
            self._last_payload = None
            self._next_connect = time.monotonic() + self._retry_delay
            self._retry_delay = min(30.0, self._retry_delay * 2)
            message = f"Discord unavailable; retrying ({exc})"

        if not self.rpc.connected and not message.startswith("Discord unavailable"):
            message = "Waiting for Discord desktop; reconnecting automatically"
        art_status = "Album art is off — enable the catalog option before starting"
        if self.artwork:
            if not snapshot.track:
                art_status = "Album art: waiting for a track"
            elif snapshot.state != PlaybackState.PLAYING:
                art_status = "Album art: lookup resumes with playback"
            elif self._art_task:
                art_status = "Album art: looking up this track…"
            elif artwork:
                sent = self._last_payload and self._last_payload.get("large_image") == artwork.url
                label = "Animated album art" if getattr(artwork, "animated", False) else "Album art"
                art_status = f"{label}: sent to Discord" if sent else f"{label}: found; waiting to publish"
                if not getattr(artwork, "animated", False) and getattr(self.artwork, "status", ""):
                    art_status = self.artwork.status
            else:
                art_status = "Album art: no catalog match or lookup unavailable; retrying automatically"
        status = ServiceStatus(snapshot, message, self.rpc.connected,
                               self.rpc.connected and self._last_payload is not None, art_status)
        self.notify(status)
        return status

    async def run(self, stop: threading.Event) -> None:
        try:
            while not stop.is_set():
                await self.tick()
                # Interruptible polling keeps the desktop Stop button responsive.
                deadline = time.monotonic() + self.poll_interval
                while not stop.is_set() and time.monotonic() < deadline:
                    if self._art_task and self._art_task.done():
                        break  # Publish a completed cover without another full poll.
                    await asyncio.sleep(min(0.1, max(0, deadline - time.monotonic())))
        finally:
            await self.close()

    async def close(self) -> None:
        if self._art_task:
            self._art_task.cancel()
            await asyncio.gather(self._art_task, return_exceptions=True)
            self._art_task = None
        try:
            if self.rpc.connected:
                await self.rpc.clear()
        except Exception:
            log.debug("Discord disappeared during shutdown")
        finally:
            try:
                await self.rpc.close()
            finally:
                try:
                    if self.artwork and hasattr(self.artwork, "close"):
                        await self.artwork.close()
                finally:
                    await self.backend.close()
