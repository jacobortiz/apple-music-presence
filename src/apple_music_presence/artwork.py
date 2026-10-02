"""Optional public-catalog artwork lookup; no local artwork is uploaded.

Calling ``resolve`` sends the supplied metadata to Apple's iTunes Search API.
The caller must obtain opt-in before constructing/using this provider. Results
contain remote URLs. Public covers are fetched in memory only when comparing
ambiguous exact catalog matches; local music and artwork are never uploaded.
The API documents a limit of
approximately 20 calls/minute: https://performance-partners.apple.com/search-api
"""

from __future__ import annotations

import asyncio
from collections import OrderedDict
from dataclasses import dataclass
import hashlib
import json
import logging
import re
from time import monotonic
import threading
import unicodedata
from urllib.parse import urlencode, urlsplit, urlunsplit
from urllib.request import Request, HTTPRedirectHandler, build_opener


_LOGGER = logging.getLogger(__name__)
_MAX_RESPONSE_BYTES = 512 * 1024
_MIN_REQUEST_INTERVAL = 3.2
_FAILURE_CACHE_SECONDS = 10.0
_MAX_COVER_BYTES = 2 * 1024 * 1024
_MAX_COVER_CANDIDATES = 4
ARTWORK_SIZE = 1024


@dataclass(frozen=True)
class Artwork:
    """Verified public artwork and its Apple store page, when unambiguous.

    An empty track_url means exact releases share this static cover, without
    selecting a particular album ID, link, or motion artwork.
    """

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


def _song_candidates(payload: object, key: tuple[str, str, str]) -> set[Artwork]:
    if not isinstance(payload, dict) or not isinstance(payload.get("results"), list):
        return set()
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
    return matches


def _album_candidates(payload: object, key: tuple[str, str, str]) -> set[Artwork]:
    if not isinstance(payload, dict) or not isinstance(payload.get("results"), list):
        return set()
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
    return matches


def _known_match(matches: set[Artwork]) -> Artwork | None:
    if len(matches) == 1:
        return next(iter(matches))
    if 1 < len(matches) <= _MAX_COVER_CANDIDATES:
        urls = {match.url for match in matches}
        if len(urls) == 1:
            # The cover is certain, but no particular release/link is chosen.
            return Artwork(next(iter(urls)), "")
    return None


def _match(payload: object, key: tuple[str, str, str]) -> Artwork | None:
    return _known_match(_song_candidates(payload, key))


def _match_album(payload: object, key: tuple[str, str, str]) -> Artwork | None:
    return _known_match(_album_candidates(payload, key))


def _normalize_exif_comment(exif: bytes) -> bytes | None:
    """Ignore only a validated UserComment in a narrow, known TIFF layout.

    Preserve color space, orientation, dimensions, and every other byte. Unknown
    tags/IFDs and overlapping offsets fail closed; no image decoder is invoked.
    """
    tiff = bytearray(exif[6:])
    if not exif.startswith(b"Exif\0\0") or len(tiff) < 8 or tiff[:2] not in (b"MM", b"II"):
        return None
    endian = "big" if tiff[:2] == b"MM" else "little"

    def number(offset, size):
        if offset < 0 or offset + size > len(tiff):
            raise ValueError("Invalid TIFF offset")
        return int.from_bytes(tiff[offset:offset + size], endian)

    regions = [(0, 8)]
    comments = []

    def reserve(offset, size):
        if offset < 8 or size <= 0 or offset + size > len(tiff):
            raise ValueError("Invalid TIFF region")
        if any(offset < end and start < offset + size for start, end in regions):
            raise ValueError("Overlapping TIFF regions")
        regions.append((offset, offset + size))

    def directory(offset, *, root):
        count = number(offset, 2)
        if not 1 <= count <= 16:
            raise ValueError("Unknown TIFF directory")
        reserve(offset, 2 + 12 * count + 4)
        if number(offset + 2 + 12 * count, 4):
            raise ValueError("Additional TIFF directories are unsupported")
        tags, child = set(), None
        for index in range(count):
            entry = offset + 2 + 12 * index
            tag, kind, length = number(entry, 2), number(entry + 2, 2), number(entry + 4, 4)
            if tag in tags:
                raise ValueError("Duplicate TIFF tag")
            tags.add(tag)
            if root and tag == 34665 and kind == 4 and length == 1:
                child = number(entry + 8, 4)
            elif root and tag == 274 and kind == 3 and length == 1:
                if not 1 <= number(entry + 8, 2) <= 8:
                    raise ValueError("Invalid EXIF orientation")
            elif not root and tag == 37510 and kind == 7 and 8 <= length <= 4096:
                start = number(entry + 8, 4)
                reserve(start, length)
                comments.append((start, length))
            elif not root and tag == 40961 and kind == 3 and length == 1:
                pass  # ColorSpace remains byte-exact.
            elif not root and tag in (40962, 40963) and kind in (3, 4) and length == 1:
                pass  # ExifImageWidth/Height remain byte-exact.
            else:
                raise ValueError("Unknown EXIF tag or layout")
        if root and child is not None:
            directory(child, root=False)

    try:
        if number(2, 2) != 42:
            return None
        directory(number(4, 4), root=True)
        if len(comments) != 1:
            return None
        for start, length in comments:
            tiff[start:start + length] = b"\0" * length
        return exif[:6] + tiff
    except ValueError:
        return None


def _jpeg_display_fingerprint(content: bytes) -> bytes | None:
    if len(content) > _MAX_COVER_BYTES or not content.startswith(b"\xff\xd8") or not content.endswith(b"\xff\xd9"):
        return None
    normalized = bytearray(content)
    offset, frame, exif_seen = 2, False, False
    while offset + 4 <= len(content):
        if content[offset] != 255:
            return None
        marker = content[offset + 1]
        size = int.from_bytes(content[offset + 2:offset + 4], "big")
        end = offset + 2 + size
        if size < 2 or end > len(content):
            return None
        payload = content[offset + 4:end]
        if marker == 225 and payload.startswith(b"Exif\0\0"):
            if exif_seen:
                return None
            exif_seen = True
            exif = _normalize_exif_comment(payload)
            if exif is None:
                return None
            normalized[offset + 4:end] = exif
        elif marker in (192, 194):
            if (len(payload) < 6 or not 1 <= payload[5] <= 4
                    or size != 8 + 3 * payload[5]
                    or not 0 < int.from_bytes(payload[1:3], "big") <= 4096
                    or not 0 < int.from_bytes(payload[3:5], "big") <= 4096):
                return None
            frame = True
        elif marker == 218:
            if not frame or end >= len(content) - 2:
                return None
            # Keep every byte of image data and later segments unchanged.
            return hashlib.sha256(normalized).digest()
        elif marker not in {196, 219, 221, 254, *range(224, 240)}:
            return None
        offset = end
    return None


def _fetch_cover(url: str, timeout: float, stop: threading.Event) -> bytes:
    if not _safe_url(url, artwork=True) or urlsplit(url).query or urlsplit(url).fragment:
        raise ValueError("Cover comparison requires a public Apple CDN URL")
    deadline = monotonic() + timeout
    request = Request(url, headers={"Accept": "image/jpeg", "User-Agent": "AppleMusicPresence-MVP/0.1"})
    if stop.is_set():
        raise ValueError("Cover comparison cancelled")
    with build_opener(_NoCatalogRedirect()).open(request, timeout=timeout) as response:
        content = bytearray()
        while True:
            if stop.is_set():
                raise ValueError("Cover comparison cancelled")
            if monotonic() >= deadline:
                raise TimeoutError("Cover comparison timed out")
            chunk = response.read1(min(64 * 1024, _MAX_COVER_BYTES + 1 - len(content)))
            if not chunk:
                return bytes(content)
            content.extend(chunk)
            if len(content) > _MAX_COVER_BYTES:
                raise ValueError("Cover comparison exceeded the image size limit")


def _shared_cover(matches: set[Artwork], timeout: float, stop: threading.Event) -> Artwork | None:
    urls = sorted({match.url for match in matches})
    if not 1 < len(matches) <= _MAX_COVER_CANDIDATES or not 1 < len(urls) <= _MAX_COVER_CANDIDATES:
        return None
    fingerprint = None
    for url in urls:
        if stop.is_set():
            return None
        image = _jpeg_display_fingerprint(_fetch_cover(url, min(timeout, 5.0), stop))
        if image is None or (fingerprint is not None and image != fingerprint):
            return None
        fingerprint = image
    return Artwork(urls[0], "")


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
        return await self._lookup(metadata, key, "song", _song_candidates)

    async def resolve_album(self, artist: str, album: str) -> Artwork | None:
        """Album artwork is shared across tracks, with exact artist/edition matching."""
        metadata = (artist, album)
        if any(not isinstance(value, str) or not value.strip() or len(value) > 512 for value in metadata):
            return None
        normalized = tuple(_normalize(value) for value in metadata)
        if not all(normalized):
            return None
        return await self._lookup(metadata, ("", *normalized), "album", _album_candidates)

    async def _lookup(self, metadata, key, entity, candidates):
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
                matches = candidates(payload, key)
                result = _known_match(matches)
                if result is None and 1 < len(matches) <= _MAX_COVER_CANDIDATES:
                    stop = threading.Event()
                    try:
                        result = await asyncio.to_thread(_shared_cover, matches, self.timeout, stop)
                    except asyncio.CancelledError:
                        stop.set()
                        raise
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
