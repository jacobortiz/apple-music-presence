"""Public Apple Music page fallback for recordings omitted by iTunes search.

Only verified album headers and their own track rows enter the cache. No
account, cookies, private API, or developer token is used. Page format changes
fail closed and leave ordinary playback sharing available.
"""

import asyncio
from collections import OrderedDict
from dataclasses import dataclass
import json
import logging
from time import monotonic
from urllib.error import HTTPError
from urllib.parse import urlencode, urlsplit, urljoin, quote

from .artwork import Artwork, _normalize, _safe_url, _MIN_REQUEST_INTERVAL
from .motion_artwork import _Scripts, _download, album_page

log = logging.getLogger(__name__)


def _items(html):
    parser = _Scripts()
    parser.feed(html)

    def walk(value):
        if isinstance(value, dict):
            if isinstance(value.get("contentDescriptor"), dict):
                yield value
            for child in value.values():
                yield from walk(child)
        elif isinstance(value, list):
            for child in value:
                yield from walk(child)

    for script in parser.scripts:
        try:
            yield from walk(json.loads(script))
        except (ValueError, RecursionError):
            continue


def _identity(page):
    verified = album_page(page)
    if not verified:
        return None
    parts = urlsplit(verified).path.strip("/").split("/")
    return parts[0].lower(), parts[-1]


def _candidate_pages(html, title):
    pages = {}
    for item in _items(html):
        descriptor = item["contentDescriptor"]
        if (descriptor.get("kind") == "song" and isinstance(item.get("title"), str)
                and _normalize(item["title"]) == _normalize(title)):
            page = album_page(descriptor.get("url"))
            if page:
                pages.setdefault(_identity(page), page)
    # Verify a bounded set; never accept an unexamined ambiguous candidate.
    return list(pages.values()) if len(pages) <= 3 else []


def _album_tracks(html, page, album):
    identity = _identity(page)
    if not identity:
        return {}
    items = list(_items(html))
    covers = set()
    for item in items:
        descriptor = item["contentDescriptor"]
        if (descriptor.get("kind") != "album" or "videoArtwork" not in item
                or str(descriptor.get("identifiers", {}).get("storeAdamID")) != identity[1]
                or not isinstance(item.get("title"), str)
                or _normalize(item["title"]) != _normalize(album)):
            continue
        header_page = album_page(descriptor.get("url"))
        art = item.get("artwork")
        template = art.get("dictionary", {}).get("url") if isinstance(art, dict) else None
        if not isinstance(template, str) or _identity(header_page) != identity:
            continue
        url = _safe_url(template.replace("{w}", "300").replace("{h}", "300")
                        .replace("{f}", "jpg"), artwork=True)
        if url and "{" not in url and "}" not in url:
            covers.add(Artwork(url, header_page))
    if len(covers) != 1:
        return {}
    cover = next(iter(covers))
    tracks = {}
    for item in items:
        descriptor = item["contentDescriptor"]
        if (descriptor.get("kind") != "song" or "trackNumber" not in item
                or _identity(descriptor.get("url")) != identity):
            continue
        title, artist = item.get("title"), item.get("artistName")
        if not all(isinstance(value, str) and value.strip() and len(value) <= 512
                   for value in (title, artist)):
            continue
        tracks[(_normalize(title), _normalize(artist), _normalize(album))] = cover
    return tracks


@dataclass(frozen=True)
class _PageCache:
    cover: Artwork | None
    expires: float


class AppleMusicArtworkResolver:
    def __init__(self, catalog, country="US"):
        if len(country) != 2 or not country.isascii() or not country.isalpha():
            raise ValueError("country must be a two-letter store country code")
        self.catalog, self.country = catalog, country.lower()
        self._tracks = OrderedDict()
        self._lock = asyncio.Lock()
        self._next_request = 0.0

    async def resolve_album(self, artist, album):
        # A confirmed album's track rows already identify its guest artists.
        # Reuse that result before repeating a search for each collaboration.
        key = (_normalize(artist), _normalize(album))
        now = monotonic()
        matches = {entry.cover for track, entry in self._tracks.items()
                   if track[1:] == key and entry.cover and entry.expires > now}
        if len(matches) == 1:
            return next(iter(matches))
        if len(matches) > 1:
            return None
        return await self.catalog.resolve_album(artist, album)

    async def _page(self, url):
        for attempt in range(3):
            delay = self._next_request - monotonic()
            if delay > 0:
                await asyncio.sleep(delay)
            self._next_request = monotonic() + _MIN_REQUEST_INTERVAL
            try:
                return (await asyncio.to_thread(_download, url, 3 * 1024 * 1024)).decode("utf-8")
            except HTTPError as error:
                # Song links can use the song's slug; Apple redirects their
                # album page to its canonical slug. Only the exact same Apple
                # album and storefront may redirect, without any credentials.
                target = album_page(urljoin(url, error.headers.get("Location", "")))
                if (error.code not in (301, 302, 307, 308) or attempt == 2
                        or not target or not _identity(url) or _identity(target) != _identity(url)):
                    raise
                url = target

    def _remember(self, key, cover, ttl):
        self._tracks[key] = _PageCache(cover, monotonic() + ttl)
        self._tracks.move_to_end(key)
        while len(self._tracks) > 512:
            self._tracks.popitem(last=False)

    async def resolve(self, title, artist, album):
        metadata = (title, artist, album)
        if not all(isinstance(value, str) and value.strip() and len(value) <= 512 for value in metadata):
            return None
        key = tuple(_normalize(value) for value in metadata)
        if not all(key):
            return None
        cached = self._tracks.get(key)
        if cached and cached.expires > monotonic():
            self._tracks.move_to_end(key)
            return cached.cover
        cover = await self.catalog.resolve(*metadata)
        if cover:
            return cover
        async with self._lock:
            cached = self._tracks.get(key)
            if cached and cached.expires > monotonic():
                return cached.cover
            ttl = 600
            try:
                query = urlencode({"term": " ".join(value.strip() for value in metadata)}, quote_via=quote)
                search = await self._page(f"https://music.apple.com/{self.country}/search?{query}")
                matches, verified_tracks = set(), []
                for page in _candidate_pages(search, title):
                    if _identity(page)[0] != self.country:
                        continue
                    tracks = _album_tracks(await self._page(page), page, album)
                    if key in tracks:
                        matches.add(tracks[key])
                        verified_tracks.append(tracks)
                cover = next(iter(matches)) if len(matches) == 1 else None
                if cover:
                    # Index only this confirmed album, including guest artists.
                    # Same titles on unrelated artists/editions remain distinct.
                    for tracks in verified_tracks:
                        for track_key, track_cover in tracks.items():
                            if track_cover == cover:
                                self._remember(track_key, track_cover, 86400)
                    return cover
            except (OSError, ValueError, TypeError, AttributeError, RecursionError) as error:
                log.debug("Public Apple Music lookup unavailable (%s)", type(error).__name__)
                cover, ttl = None, 10
            self._remember(key, cover, ttl)
            return cover
