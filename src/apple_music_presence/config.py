"""Non-secret local preferences. No accounts, credentials, or listening history."""

from dataclasses import asdict, dataclass
import json
import os
from pathlib import Path
import re
import tempfile


_MAX_SETTINGS_BYTES = 16 * 1024


@dataclass
class Settings:
    client_id: str = ""
    artwork: bool = False
    motion_artwork: bool = False
    artwork_repository: str = ""
    country: str = "US"
    source_id: str = ""

    def validate(self, *, demo: bool = False):
        self.client_id = self.client_id.strip()
        self.country = self.country.strip().upper()
        self.source_id = self.source_id.strip()
        self.artwork_repository = self.artwork_repository.strip()
        if demo:
            return
        if not re.fullmatch(r"[0-9]{17,20}", self.client_id):
            raise ValueError("Enter the 17–20 digit Application ID from the Discord Developer Portal.")
        if not re.fullmatch(r"[A-Z]{2}", self.country):
            raise ValueError("Country must be a two-letter store code, such as US or GB.")
        if self.motion_artwork:
            from .motion_artwork import REPOSITORY_PATTERN
            if not re.fullmatch(REPOSITORY_PATTERN, self.artwork_repository):
                raise ValueError("Enter a public GitHub artwork host as owner/repository.")
            self.artwork = True


def settings_path() -> Path:
    base = Path(os.environ.get("LOCALAPPDATA", str(Path.home() / ".config")))
    return base / "AppleMusicPresence" / "settings.json"


def load_settings(path: Path | None = None) -> Settings:
    try:
        with (path or settings_path()).open("rb") as handle:
            raw = handle.read(_MAX_SETTINGS_BYTES + 1)
        if len(raw) > _MAX_SETTINGS_BYTES:
            return Settings()
        data = json.loads(raw.decode("utf-8"))
        return Settings(
            client_id=data.get("client_id", "") if isinstance(data.get("client_id"), str) else "",
            artwork=data.get("artwork") is True,
            motion_artwork=data.get("motion_artwork") is True,
            artwork_repository=data.get("artwork_repository", "") if isinstance(data.get("artwork_repository"), str) else "",
            country=data.get("country", "US") if isinstance(data.get("country"), str) else "US",
            source_id=data.get("source_id", "") if isinstance(data.get("source_id"), str) else "",
        )
    except (OSError, ValueError, AttributeError, RecursionError):
        return Settings()


def save_settings(settings: Settings, path: Path | None = None):
    destination = path or settings_path()
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=destination.parent,
                                         prefix=destination.name + ".", suffix=".tmp", delete=False) as handle:
            temporary = Path(handle.name)
            handle.write(json.dumps(asdict(settings), indent=2) + "\n")
        temporary.replace(destination)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
