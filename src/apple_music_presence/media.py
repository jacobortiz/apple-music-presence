"""Media detection boundary and the Windows system media-session adapter.

The rest of the application only consumes ``MediaSnapshot``. A future C++
backend can implement ``MediaBackend`` without knowing anything about Discord.
WinRT is imported on first use, so importing this module works on other OSes.
"""

from __future__ import annotations

import asyncio
from datetime import datetime, timezone
import math
from pathlib import PureWindowsPath
import sys
import threading
import time
from typing import Any

from .models import MediaBackend, MediaSnapshot, PlaybackState, Track


class MediaBackendError(RuntimeError):
    """A recoverable media-service failure, or a missing platform dependency."""


def is_apple_music_source(source_id: str) -> bool:
    """Match the native Apple Music app, never a browser or other music player."""
    normalized = source_id.casefold()
    return normalized.startswith("appleinc.applemusicwin_") or (
        PureWindowsPath(normalized).name == "applemusic.exe"
    )


def _artist_album(artist: str, album: str, source_id: str) -> tuple[str, str]:
    """Normalize the native Apple Music app's combined SMTC metadata field.

    Observed on Windows: artist and album_artist contain `Artist — Album`,
    while album_title is empty. Restrict this workaround to Apple Music,
    absent explicit album metadata, and one unambiguous spaced em dash.
    """
    if not album and is_apple_music_source(source_id) and artist.count(" — ") == 1:
        candidate_artist, candidate_album = (part.strip() for part in artist.split(" — ", 1))
        if candidate_artist and candidate_album:
            return candidate_artist, candidate_album
    return artist, album


def _state(status: Any) -> PlaybackState:
    # Microsoft's GSMTC enum: Closed=0, Opened=1, Changing=2, Stopped=3,
    # Playing=4, Paused=5. Projected WinRT enum members are IntEnums.
    if status == 4:
        return PlaybackState.PLAYING
    if status == 5:
        return PlaybackState.PAUSED
    return PlaybackState.STOPPED


def _seconds(value: Any) -> float | None:
    try:
        seconds = float(value.total_seconds())
        return seconds if math.isfinite(seconds) else None
    except (AttributeError, TypeError, ValueError, OverflowError):
        return None


def _timestamp(value: Any, now: float) -> float:
    if not isinstance(value, datetime):
        return now
    # WinRT documents UTC and PyWinRT normally returns timezone-aware dates.
    # Treat a naive value as UTC as well, never as the computer's local zone.
    if value.tzinfo is None:
        value = value.replace(tzinfo=timezone.utc)
    try:
        timestamp = value.timestamp()
    except (ValueError, OverflowError, OSError):
        return now
    # An uninitialized WinRT DateTime represents 1601; do not extrapolate it.
    if not math.isfinite(timestamp) or timestamp <= 0:
        return now
    return min(timestamp, now)


def _rate(value: Any) -> float:
    try:
        rate = float(value)
        return rate if math.isfinite(rate) and rate > 0 else 1.0
    except (TypeError, ValueError, OverflowError):
        return 1.0


def _timeline(timeline: Any, state: PlaybackState, rate: float,
              now: float) -> tuple[float | None, float | None]:
    start = _seconds(timeline.start_time)
    end = _seconds(timeline.end_time)
    position = _seconds(timeline.position)
    if start is None:
        return None, None
    duration = end - start if end is not None and end > start else None
    if position is None:
        return None, duration
    position = max(0.0, position - start)
    if state is PlaybackState.PLAYING:
        position += (now - _timestamp(timeline.last_updated_time, now)) * rate
    if duration is not None:
        position = min(position, duration)
    return position, duration


class WindowsMediaBackend(MediaBackend):
    """Poll native Apple Music sessions using Windows.Media.Control.

    Construct anywhere, but call read/list_sessions/close on one asyncio worker
    thread. The adapter explicitly owns one MTA COM initialization on that
    thread. Each read enumerates sessions again, so reopening Apple Music is
    detected without keeping a dead session reference. A failed request drops
    the session manager and retries discovery once.
    """

    def __init__(self, source_id: str | None = None, *, timeout: float = 5.0):
        if timeout <= 0 or not math.isfinite(timeout):
            raise ValueError("Media request timeout must be a positive number.")
        self.source_id = source_id or None
        self.timeout = timeout
        self._manager: Any = None
        self._manager_type: Any = None
        self._runtime: Any = None
        self._owner_thread: int | None = None

    def _initialize(self) -> None:
        if self._owner_thread is not None:
            if self._owner_thread != threading.get_ident():
                raise MediaBackendError("Use the media backend on one worker thread.")
            return
        if sys.platform != "win32":
            raise MediaBackendError("Apple Music detection requires Windows 10/11.")
        try:
            import winrt.runtime as runtime
            # Foundation provides the projected IAsyncOperation result type.
            import winrt.windows.foundation  # noqa: F401
            from winrt.windows.media.control import (
                GlobalSystemMediaTransportControlsSessionManager,
            )
        except ImportError as exc:
            raise MediaBackendError(
                "Windows media dependencies are missing. Install the project "
                "with the supported Python version; see README setup."
            ) from exc
        try:
            runtime.init_apartment(runtime.ApartmentType.MULTI_THREADED)
        except OSError as exc:
            raise MediaBackendError(
                "Windows media detection needs its own background thread "
                "with a multithreaded COM apartment."
            ) from exc
        self._runtime = runtime
        self._manager_type = GlobalSystemMediaTransportControlsSessionManager
        self._owner_thread = threading.get_ident()

    async def _ensure_manager(self) -> Any:
        self._initialize()
        if self._manager is None:
            self._manager = await asyncio.wait_for(
                self._manager_type.request_async(), timeout=self.timeout
            )
        return self._manager

    def _matches(self, source: str) -> bool:
        if self.source_id is not None:
            return source.casefold() == self.source_id.casefold()
        return is_apple_music_source(source)

    async def list_sessions(self) -> list[str]:
        """Return source IDs for diagnostics, including unmatched applications."""
        for attempt in range(2):
            try:
                manager = await self._ensure_manager()
                return sorted({session.source_app_user_model_id
                               for session in manager.get_sessions()})
            except (OSError, TimeoutError) as exc:
                self._manager = None
                if attempt:
                    raise MediaBackendError(
                        "Windows media sessions are unavailable; retrying later."
                    ) from exc
        raise AssertionError("unreachable")

    async def read(self) -> MediaSnapshot:
        for attempt in range(2):
            try:
                return await self._read_once()
            except (OSError, TimeoutError) as exc:
                self._manager = None
                if attempt:
                    raise MediaBackendError(
                        "Apple Music's media session is unavailable; retrying later."
                    ) from exc
        raise AssertionError("unreachable")

    async def _read_once(self) -> MediaSnapshot:
        manager = await self._ensure_manager()
        candidates = []
        for session in manager.get_sessions():
            source = session.source_app_user_model_id
            if self._matches(source):
                state = _state(session.get_playback_info().playback_status)
                priority = {PlaybackState.PLAYING: 0, PlaybackState.PAUSED: 1,
                            PlaybackState.STOPPED: 2}[state]
                candidates.append((priority, source, session))

        if not candidates:
            return MediaSnapshot(track=None, state=PlaybackState.STOPPED)

        _, source, session = min(candidates, key=lambda candidate: candidate[:2])
        info = session.get_playback_info()
        state = _state(info.playback_status)
        if state is PlaybackState.STOPPED:
            return MediaSnapshot(track=None, state=state, source_id=source)

        properties = await asyncio.wait_for(
            session.try_get_media_properties_async(), timeout=self.timeout
        )
        title = (properties.title or "").strip() if properties else ""
        if not title:
            # Never keep the previous song after metadata disappears.
            return MediaSnapshot(track=None, state=PlaybackState.STOPPED,
                                 source_id=source)

        # Refresh state after awaiting metadata, which may have taken time.
        info = session.get_playback_info()
        state = _state(info.playback_status)
        if state is PlaybackState.STOPPED:
            return MediaSnapshot(track=None, state=state, source_id=source)
        rate = _rate(info.playback_rate)
        now = time.time()
        try:
            position, duration = _timeline(session.get_timeline_properties(),
                                           state, rate, now)
        except OSError:
            # Metadata is useful even if this media item has no timeline.
            position, duration = None, None
        artist, album = _artist_album((properties.artist or "").strip(),
                                      (properties.album_title or "").strip(), source)
        return MediaSnapshot(
            track=Track(title=title, artist=artist, album=album),
            state=state, position=position, duration=duration, observed_at=now,
            source_id=source, playback_rate=rate,
        )

    async def close(self) -> None:
        if self._owner_thread is not None:
            if self._owner_thread != threading.get_ident():
                raise MediaBackendError("Close the media backend on its worker thread.")
            self._manager = None
            self._manager_type = None
            self._runtime.uninit_apartment()
            self._runtime = None
            self._owner_thread = None
