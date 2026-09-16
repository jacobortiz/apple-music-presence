"""Backend-neutral data contract. This module has no OS or Discord dependencies."""

from dataclasses import dataclass, field
from enum import Enum
import math
import time
from typing import Protocol


class PlaybackState(str, Enum):
    PLAYING = "playing"
    PAUSED = "paused"
    STOPPED = "stopped"


@dataclass(frozen=True)
class Track:
    title: str
    artist: str
    album: str


@dataclass(frozen=True)
class MediaSnapshot:
    track: Track | None
    state: PlaybackState
    position: float | None = None
    duration: float | None = None
    observed_at: float = field(default_factory=time.time)
    source_id: str = ""
    playback_rate: float = 1.0

    def position_at(self, now: float) -> float | None:
        if self.position is None or not math.isfinite(self.position):
            return None
        elapsed = max(0.0, now - self.observed_at)
        rate = self.playback_rate if math.isfinite(self.playback_rate) else 1.0
        position = self.position
        if self.state == PlaybackState.PLAYING:
            position += elapsed * max(0.0, rate)
        if self.duration is not None and math.isfinite(self.duration) and self.duration > 0:
            position = min(position, self.duration)
        return max(0.0, position)


class MediaBackend(Protocol):
    """A future C++ adapter only needs to implement these two async operations."""

    async def read(self) -> MediaSnapshot: ...

    async def close(self) -> None: ...
