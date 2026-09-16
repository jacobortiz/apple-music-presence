"""Non-secret local preferences. No accounts, credentials, or listening history."""

from dataclasses import asdict, dataclass
import json
import os
from pathlib import Path
import re


@dataclass
class Settings:
    client_id: str = ""
    artwork: bool = False
    country: str = "US"
    source_id: str = ""

    def validate(self, *, demo: bool = False):
        self.client_id = self.client_id.strip()
        self.country = self.country.strip().upper()
        self.source_id = self.source_id.strip()
        if not demo and not re.fullmatch(r"[0-9]{17,20}", self.client_id):
            raise ValueError("Enter the 17–20 digit Application ID from the Discord Developer Portal.")
        if not re.fullmatch(r"[A-Z]{2}", self.country):
            raise ValueError("Country must be a two-letter store code, such as US or GB.")


def settings_path() -> Path:
    base = Path(os.environ.get("LOCALAPPDATA", str(Path.home() / ".config")))
    return base / "AppleMusicPresence" / "settings.json"


def load_settings(path: Path | None = None) -> Settings:
    try:
        data = json.loads((path or settings_path()).read_text(encoding="utf-8"))
        return Settings(
            client_id=data.get("client_id", "") if isinstance(data.get("client_id"), str) else "",
            artwork=data.get("artwork") is True,
            country=data.get("country", "US") if isinstance(data.get("country"), str) else "US",
            source_id=data.get("source_id", "") if isinstance(data.get("source_id"), str) else "",
        )
    except (OSError, ValueError, AttributeError):
        return Settings()


def save_settings(settings: Settings, path: Path | None = None):
    destination = path or settings_path()
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_suffix(".tmp")
    temporary.write_text(json.dumps(asdict(settings), indent=2) + "\n", encoding="utf-8")
    temporary.replace(destination)
