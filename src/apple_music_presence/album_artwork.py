"""Explicit album-to-animation mappings, with the public catalog as fallback."""

from importlib.resources import files
import json
import logging
from pathlib import Path
from urllib.parse import urlsplit

from .artwork import Artwork, _normalize, _safe_url

log = logging.getLogger(__name__)


def _animation_url(value) -> str | None:
    if not isinstance(value, str) or len(value) > 2048 or value != value.strip():
        return None
    try:
        url = urlsplit(value)
        if (url.scheme != "https" or not url.hostname or url.username is not None
                or url.password is not None or url.port not in (None, 443)
                or url.fragment or url.query or any(ord(character) < 32 for character in value)
                or not url.path.lower().endswith((".webp", ".gif", ".avif"))):
            return None
    except ValueError:
        return None
    return value


def _entries(text: str) -> list:
    if len(text) > 128 * 1024:
        raise ValueError("Album artwork map is too large")
    data = json.loads(text)
    if not isinstance(data, dict) or data.get("version") != 1:
        raise ValueError("Expected album artwork map version 1")
    albums = data.get("albums")
    if not isinstance(albums, list) or len(albums) > 256:
        raise ValueError("Expected at most 256 album mappings")
    return albums


class MappedArtworkResolver:
    """Match complete artist/album names; do not blur editions or artists.

    Configured URLs are passed directly to Discord, without scraping artwork
    sites or doing a catalog search for each song on a mapped album.
    """

    def __init__(self, catalog, entries):
        self.catalog = catalog
        self._albums = {}
        conflicts = set()
        for entry in entries:
            if not isinstance(entry, dict):
                continue
            artist, album = entry.get("artist"), entry.get("album")
            if any(not isinstance(value, str) or not value.strip() or len(value) > 512
                   for value in (artist, album)):
                continue
            key = (_normalize(artist), _normalize(album))
            image_url = _animation_url(entry.get("image_url"))
            album_url = _safe_url(entry.get("album_url"), artwork=False)
            if album_url and (urlsplit(album_url).query or urlsplit(album_url).fragment):
                album_url = None
            if not all(key) or not image_url or not album_url:
                continue
            artwork = Artwork(image_url, album_url, animated=True)
            if key in self._albums and self._albums[key] != artwork:
                conflicts.add(key)
            self._albums[key] = artwork
        for key in conflicts:
            self._albums.pop(key, None)
        if conflicts:
            log.warning("Conflicting animated cover mappings were ignored")

    @classmethod
    def load(cls, catalog, custom_path: Path | None = None):
        try:
            entries = _entries(files("apple_music_presence").joinpath("album_artwork.json").read_text(encoding="utf-8"))
        except (OSError, ValueError):
            log.warning("Bundled album artwork map unavailable; using catalog artwork")
            entries = []
        if custom_path is not None and custom_path.exists():
            try:
                entries += _entries(custom_path.read_text(encoding="utf-8"))
            except (OSError, ValueError):
                log.warning("Custom album artwork map invalid; ignoring it")
        return cls(catalog, entries)

    async def resolve(self, title: str, artist: str, album: str) -> Artwork | None:
        if artist.strip() and album.strip():
            artwork = self._albums.get((_normalize(artist), _normalize(album)))
            if artwork is not None:
                return artwork
        return await self.catalog.resolve(title, artist, album)

    @property
    def status(self):
        return getattr(self.catalog, "status", "")

    async def refresh(self, title, artist, album):
        if (_normalize(artist), _normalize(album)) in self._albums:
            return self._albums[(_normalize(artist), _normalize(album))]
        refresh = getattr(self.catalog, "refresh", None)
        return await refresh(title, artist, album) if refresh else None

    async def close(self):
        close = getattr(self.catalog, "close", None)
        if close:
            await close()
