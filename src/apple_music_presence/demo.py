"""Offline preview adapters. No media access, networking, or Discord writes."""

import time

from .models import MediaSnapshot, PlaybackState, Track


class DemoBackend:
    def __init__(self):
        self.started = time.time()

    async def read(self):
        elapsed = time.time() - self.started
        phase = elapsed % 50
        state = PlaybackState.PAUSED if 35 <= phase < 43 else PlaybackState.PLAYING
        return MediaSnapshot(Track("A quiet afternoon", "Sample Artist", "Demo Album"),
                             state, min(phase, 35) if phase < 43 else phase - 8,
                             180, time.time(), "Offline demo")

    async def close(self):
        pass


class DemoRpc:
    connected = False

    async def connect(self):
        self.connected = True

    async def update(self, payload):
        pass

    async def clear(self):
        pass

    async def close(self):
        self.connected = False
