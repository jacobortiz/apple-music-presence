"""Optional public-catalog artwork lookup; no local artwork is uploaded.

Calling ``resolve`` sends the supplied metadata to Apple's iTunes Search API.
The caller must obtain opt-in before constructing/using this provider. Results
contain remote URLs, never downloaded artwork. The API documents a limit of
approximately 20 calls/minute: https://performance-partners.apple.com/search-api
"""

from __future__ import annotations

import asyncio
from collections import OrderedDict
from dataclasses import dataclass
import json
import logging
import re
from time import monotonic
import unicodedata
from urllib.parse import urlencode, urlsplit, urlunsplit
from urllib.request import Request, HTTPRedirectHandler, build_opener


_LOGGER = logging.getLogger(__name__)
_MAX_RESPONSE_BYTES = 512 * 1024
_MIN_REQUEST_INTERVAL = 3.2
_FAILURE_CACHE_SECONDS = 10.0
ARTWORK_SIZE = 1024


@dataclass(frozen=True)
class Artwork:
    """Verified public artwork and the matching Apple store page."""

    url: str
    track_url: str
    animated: bool = False


@dataclass(frozen=True)
class _CacheEntry:
    artwork: Artwork | None
    expires_at: float


def _normalize(value: str) -> str:
    # Preserve all words, including remix/live/remaster and featured artists.
    # Normalizing punctuation is safe; dropping parenthesized text is not.
    normalized = unicodedata.normalize("NFKC", value).casefold()
    return " ".join("".join(c if c.isalnum() else " " for c in normalized).split())


def _safe_url(value: object, *, artwork: bool) -> str | None:
    if not isinstance(value, str) or len(value) > 2048 or value != value.strip():
        return None
    try:
        parsed = urlsplit(value)
        host = parsed.hostname or ""
        if (
            parsed.scheme != "https"
            or parsed.username is not None
            or parsed.password is not None
            or parsed.port not in (None, 443)
            or any(ord(character) < 32 for character in value)
        ):
            return None
    except ValueError:
        return None
    if artwork:
        permitted = host.endswith(".mzstatic.com") or host.endswith(".itunes.apple.com")
    else:
        permitted = host in {"music.apple.com", "itunes.apple.com"}
    return value if permitted else None


def _full_size_artwork(value: object) -> str | None:
    """Request a larger rendition of recognized Apple CDN thumbnails.

    The Search API returns 100-pixel URLs. Apple's public thumbnail service
    also serves this same image at larger sizes. Leave unfamiliar URL shapes
    untouched rather than guessing how to rewrite them.
    """
    url = _safe_url(value, artwork=True)
    if not url:
        return None
    parsed = urlsplit(url)
    if (not parsed.hostname.endswith(".mzstatic.com")
            or not parsed.path.startswith("/image/thumb/") or parsed.query or parsed.fragment):
        return url
    size = re.search(r"/(\d+)x(\d+)bb(?:-\d+)?\.(jpg|jpeg|png)$", parsed.path)
    if not size or size[1] != size[2] or int(size[1]) >= ARTWORK_SIZE:
        return url
    path = parsed.path[:size.start()] + f"/{ARTWORK_SIZE}x{ARTWORK_SIZE}bb.{size[3]}"
    return urlunsplit(parsed._replace(path=path))


class _NoCatalogRedirect(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def _fetch_json(url: str, timeout: float) -> object:
    parsed = urlsplit(url)
    if (parsed.scheme != "https" or parsed.hostname != "itunes.apple.com"
            or parsed.username is not None or parsed.password is not None
            or parsed.port not in (None, 443) or parsed.fragment):
        raise ValueError("Catalog requests must stay on Apple's public HTTPS API")
    request = Request(
        url,
        headers={"Accept": "application/json", "User-Agent": "AppleMusicPresence-MVP/0.1"},
    )
    with build_opener(_NoCatalogRedirect()).open(request, timeout=timeout) as response:
        data = response.read(_MAX_RESPONSE_BYTES + 1)
    if len(data) > _MAX_RESPONSE_BYTES:
        raise ValueError("Catalog response exceeded the size limit")
    return json.loads(data.decode("utf-8"))


def _match(payload: object, key: tuple[str, str, str]) -> Artwork | None:
    if not isinstance(payload, dict) or not isinstance(payload.get("results"), list):
        return None
    matches: set[Artwork] = set()
    for result in payload["results"]:
        if not isinstance(result, dict) or result.get("kind") != "song":
            continue
        values = tuple(result.get(field) for field in ("trackName", "artistName", "collectionName"))
        if any(not isinstance(value, str) for value in values):
            continue
        if tuple(_normalize(value) for value in values) != key:
            continue
        url = _full_size_artwork(result.get("artworkUrl100"))
        track_url = _safe_url(result.get("trackViewUrl"), artwork=False)
        if url and track_url:
            matches.add(Artwork(url=url, track_url=track_url))
    # If exact metadata still identifies multiple recordings, show no art.
    return next(iter(matches)) if len(matches) == 1 else None


def _match_album(payload: object, key: tuple[str, str, str]) -> Artwork | None:
    if not isinstance(payload, dict) or not isinstance(payload.get("results"), list):
        return None
    matches = set()
    for result in payload["results"]:
        if not isinstance(result, dict) or result.get("collectionType") != "Album":
            continue
        values = (result.get("artistName"), result.get("collectionName"))
        if any(not isinstance(value, str) for value in values):
            continue
        if tuple(_normalize(value) for value in values) != key[1:]:
            continue
        url = _full_size_artwork(result.get("artworkUrl100"))
        page = _safe_url(result.get("collectionViewUrl"), artwork=False)
        if url and page:
            matches.add(Artwork(url, page))
    return next(iter(matches)) if len(matches) == 1 else None


class ItunesArtworkResolver:
    """Conservative, asynchronous catalog lookup with a bounded in-memory LRU.

    Use one instance per app/event loop. Calls are serialized and spaced by at
    least 3.2 seconds. Cancellation propagates normally; an already dispatched
    network call may finish in its worker thread within its socket timeout.
    Failed lookups are cached briefly to avoid retrying on every playback tick.
    """

    def __init__(self, country: str = "US", *, timeout: float = 5.0, cache_size: int = 256) -> None:
        if len(country) != 2 or not country.isascii() or not country.isalpha():
            raise ValueError("country must be a two-letter store country code")
        if not 0 < timeout <= 15:
            raise ValueError("timeout must be between 0 and 15 seconds")
        if not 1 <= cache_size <= 4096:
            raise ValueError("cache_size must be between 1 and 4096")
        self.country = country.upper()
        self.timeout = timeout
        self.cache_size = cache_size
        self._cache: OrderedDict[tuple[str, str, str], _CacheEntry] = OrderedDict()
        self._lock = asyncio.Lock()
        self._next_request_at = 0.0

    async def resolve(self, title: str, artist: str, album: str) -> Artwork | None:
        """Return an exact catalog match, or None for missing/uncertain data.

        The original metadata is sent only on an uncached lookup. Long or
        incomplete metadata is skipped, keeping private/local files without
        conventional music tags out of speculative catalog searches.
        """
        metadata = (title, artist, album)
        if any(not isinstance(value, str) or not value.strip() or len(value) > 512 for value in metadata):
            return None
        key = tuple(_normalize(value) for value in metadata)
        if not all(key):
            return None
        return await self._lookup(metadata, key, "song", _match)

    async def resolve_album(self, artist: str, album: str) -> Artwork | None:
        """Album artwork is shared across tracks, with exact artist/edition matching."""
        metadata = (artist, album)
        if any(not isinstance(value, str) or not value.strip() or len(value) > 512 for value in metadata):
            return None
        normalized = tuple(_normalize(value) for value in metadata)
        if not all(normalized):
            return None
        return await self._lookup(metadata, ("", *normalized), "album", _match_album)

    async def _lookup(self, metadata, key, entity, match):
        async with self._lock:
            now = monotonic()
            cached = self._cache.get(key)
            if cached and cached.expires_at > now:
                self._cache.move_to_end(key)
                return cached.artwork
            if cached:
                del self._cache[key]
            delay = self._next_request_at - now
            if delay > 0:
                await asyncio.sleep(delay)
            self._next_request_at = monotonic() + _MIN_REQUEST_INTERVAL
            query = urlencode(
                {
                    "term": " ".join(value.strip() for value in metadata),
                    "country": self.country,
                    "media": "music",
                    "entity": entity,
                    "limit": 25,
                }
            )
            try:
                payload = await asyncio.to_thread(
                    _fetch_json, f"https://itunes.apple.com/search?{query}", self.timeout
                )
                result = match(payload, key)
                ttl = 24 * 60 * 60 if result else 10 * 60
            except (OSError, ValueError, TypeError) as error:
                # Deliberately avoid logging the query or listener metadata.
                _LOGGER.debug("Artwork lookup unavailable (%s)", type(error).__name__)
                result = None
                ttl = _FAILURE_CACHE_SECONDS
            self._cache[key] = _CacheEntry(result, monotonic() + ttl)
            self._cache.move_to_end(key)
            while len(self._cache) > self.cache_size:
                self._cache.popitem(last=False)
            return result
