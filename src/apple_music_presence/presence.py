"""Pure mapping from media snapshots to Discord's presence fields."""

import math
from typing import Any

from .models import MediaSnapshot, PlaybackState


def discord_text(value: str) -> str:
    # Discord's details/state fields must contain 2–128 characters.
    text = " ".join(value.split())[:128]
    return text if len(text) >= 2 else text + "\u200b"


def build_presence(snapshot: MediaSnapshot, now: float, artwork: Any = None) -> dict | None:
    if snapshot.state != PlaybackState.PLAYING or not snapshot.track:
        return None
    track = snapshot.track
    if not track.title.strip():
        return None
    context = " · ".join(part for part in (track.artist, track.album) if part.strip())
    payload = {
        "details": discord_text(track.title),
        "state": discord_text(context or "Apple Music"),
    }
    position = snapshot.position_at(now)
    duration = snapshot.duration
    # Do not manufacture progress when Apple Music supplies no valid timeline.
    # Discord timestamps tick at real-time speed, so omit them for other rates.
    if (position is not None and duration is not None and math.isfinite(duration)
            and duration > 0 and abs(snapshot.playback_rate - 1.0) < 0.01):
        start = int(now - position)
        payload.update(start=start, end=max(start + 1, int(start + duration)))
    if artwork:
        payload.update(
            large_image=artwork.url,
            large_text=discord_text(track.album or track.title),
            buttons=[{"label": "Listen on Apple Music", "url": artwork.track_url}],
        )
    return payload


def materially_changed(previous: dict | None, current: dict | None) -> bool:
    if previous is None or current is None:
        return previous != current
    metadata = lambda payload: {k: v for k, v in payload.items() if k not in {"start", "end"}}
    if metadata(previous) != metadata(current):
        return True
    for key in ("start", "end"):
        before, after = previous.get(key), current.get(key)
        if before is None or after is None:
            if before != after:
                return True
        elif abs(before - after) >= 3:
            return True
    return False
