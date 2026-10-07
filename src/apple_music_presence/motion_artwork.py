"""Discover public Apple motion covers, convert once, and cache on GitHub.

Static artwork is returned immediately. Motion discovery runs independently of
media polling and upgrades the cover through ``refresh`` when it is ready.
"""

import asyncio
from collections import OrderedDict
from dataclasses import dataclass
import hashlib
from html.parser import HTMLParser
import json
import logging
import os
from pathlib import Path
import re
import subprocess
import tempfile
import threading
import time
from urllib.error import HTTPError
from urllib.parse import urljoin, urlsplit, urlunsplit, quote
from urllib.request import Request, build_opener, HTTPRedirectHandler

from .artwork import Artwork, _normalize, _safe_url

log = logging.getLogger(__name__)
MAX_IMAGE_BYTES = 8 * 1024 * 1024
# Changing the conversion recipe must also change this profile. The profile is
# included in cache validation and upload names so Discord fetches a new image.
ENCODING_PROFILE = "webp-768-q85-lanczos-v2"
_ENCODING_ATTEMPTS = ((768, 15), (768, 10), (512, 10))
REPOSITORY_PATTERN = r"[A-Za-z0-9][A-Za-z0-9_.-]{0,99}/[A-Za-z0-9][A-Za-z0-9_.-]{0,99}"


class _NoRedirect(HTTPRedirectHandler):
    # Never send GitHub credentials or follow media redirects to another host.
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def _download(url, limit, *, headers=None, method=None, data=None):
    parsed = urlsplit(url)
    permitted = {"music.apple.com", "mvod.itunes.apple.com", "api.github.com", "raw.githubusercontent.com"}
    if (parsed.scheme != "https" or parsed.hostname not in permitted
            or parsed.username is not None or parsed.password is not None
            or parsed.port not in (None, 443) or parsed.fragment
            or any(ord(character) < 32 for character in url)):
        raise ValueError("Artwork request destination is not permitted")
    if any(key.casefold() == "authorization" for key in (headers or {})) and parsed.hostname != "api.github.com":
        raise ValueError("GitHub credentials may only be sent to GitHub's API")
    request = Request(url, headers={"User-Agent": "AppleMusicPresence/0.1", **(headers or {})},
                      method=method, data=data)
    with build_opener(_NoRedirect()).open(request, timeout=10) as response:
        payload = response.read(limit + 1)
    if len(payload) > limit:
        raise ValueError("Artwork response exceeded size limit")
    return payload


def album_page(track_url):
    """Catalog matches normally link to /album/slug/id?i=track-id."""
    if not _safe_url(track_url, artwork=False):
        return None
    url = urlsplit(track_url)
    parts = url.path.strip("/").split("/")
    if (url.hostname != "music.apple.com" or len(parts) != 4 or parts[1] != "album"
            or not re.fullmatch(r"[a-zA-Z]{2}", parts[0]) or not parts[-1].isdigit()):
        return None
    return urlunsplit(("https", "music.apple.com", url.path, "", ""))


def _album_redirect(page, error):
    """Allow only Apple's canonical slug for the same album and storefront."""
    source = album_page(page)
    if not source or error.code not in (301, 302, 307, 308):
        return None
    location = error.headers.get("Location") if error.headers else None
    if not isinstance(location, str) or not location:
        return None
    try:
        target = album_page(urljoin(source, location))
    except ValueError:
        return None
    if not target:
        return None
    source_parts = urlsplit(source).path.strip("/").split("/")
    target_parts = urlsplit(target).path.strip("/").split("/")
    if (source_parts[0].lower(), source_parts[-1]) != (target_parts[0].lower(), target_parts[-1]):
        return None
    return target


def _apple_stream(value):
    if not isinstance(value, str) or len(value) > 2048:
        return None
    try:
        url = urlsplit(value)
        if (url.scheme == "https" and url.hostname == "mvod.itunes.apple.com"
                and url.port in (None, 443) and not url.username and not url.password
                and not url.fragment and not url.query and url.path.endswith(".m3u8")
                and not any(ord(c) < 32 for c in value)):
            return value
    except ValueError:
        pass
    return None


class _Scripts(HTMLParser):
    def __init__(self):
        super().__init__()
        self.scripts, self.current = [], None

    def handle_starttag(self, tag, attrs):
        if tag == "script":
            self.current = []

    def handle_data(self, text):
        if self.current is not None:
            self.current.append(text)

    def handle_endtag(self, tag):
        if tag == "script" and self.current is not None:
            self.scripts.append("".join(self.current))
            self.current = None


def find_motion(html, album_id, artist, album):
    """Use the album header only, never a recommendation or another edition."""
    parser = _Scripts()
    parser.feed(html)
    found_header, streams = False, set()

    def walk(value):
        nonlocal found_header
        if isinstance(value, dict):
            descriptor = value.get("contentDescriptor", {})
            if (isinstance(descriptor, dict) and descriptor.get("kind") == "album"
                    and str(descriptor.get("identifiers", {}).get("storeAdamID")) == album_id
                    and "videoArtwork" in value):
                # The catalog already matched the song's artist and album.
                # Album artist names can differ for collaborations/compilations.
                if _normalize(value.get("title", "")) == _normalize(album):
                    found_header = True
                    video = value.get("videoArtwork") or {}
                    stream = _apple_stream(video.get("dictionary", {}).get("motionDetailSquare", {}).get("video"))
                    if stream:
                        streams.add(stream)
            for child in value.values():
                walk(child)
        elif isinstance(value, list):
            for child in value:
                walk(child)

    for script in parser.scripts:
        try:
            walk(json.loads(script))
        except (ValueError, TypeError, AttributeError, RecursionError):
            continue
    if not found_header:
        raise ValueError("Apple album page format or metadata did not match")
    if len(streams) > 1:
        raise ValueError("Ambiguous motion cover")
    return next(iter(streams), None)


def discover_motion(page, artist, album):
    page = album_page(page)
    if not page:
        raise ValueError("Motion discovery requires a verified Apple album page")
    for attempt in range(3):
        try:
            html = _download(page, 3 * 1024 * 1024).decode("utf-8")
            return find_motion(html, urlsplit(page).path.strip("/").rsplit("/", 1)[-1], artist, album)
        except HTTPError as error:
            target = _album_redirect(page, error)
            if attempt == 2 or not target:
                raise
            page = target


def download_video(stream, directory, stop=None):
    """Restrict HLS to Apple's HTTPS hosts before giving local bytes to FFmpeg."""
    if stop and stop.is_set():
        raise ValueError("Motion download cancelled")
    master = _download(stream, 128 * 1024).decode("utf-8")
    lines = master.splitlines()
    choices = []
    for index, line in enumerate(lines[:-1]):
        if line.startswith("#EXT-X-STREAM-INF:") and 'CODECS="avc1.' in line:
            size = re.search(r"RESOLUTION=(\d+)x(\d+)", line)
            if size and size[1] == size[2] and 0 < int(size[1]) <= 1080:
                url = _apple_stream(urljoin(stream, lines[index + 1].strip()))
                if url:
                    choices.append((int(size[1]), url))
    if not choices:
        raise ValueError("No supported square SDR rendition")
    larger = sorted(choice for choice in choices if choice[0] >= 768)
    smaller = sorted((choice for choice in choices if choice[0] < 768), reverse=True)
    # Prefer enough source pixels for the output without downloading 4K video.
    variants = [larger[0][1] if larger else smaller[0][1]]
    if larger and smaller:
        variants.append(smaller[0][1])
    for index, variant in enumerate(variants):
        try:
            return _download_rendition(variant, directory, stop)
        except ValueError as error:
            # A higher-resolution source can exceed the existing download cap.
            # Retry one smaller rendition, keeping all host/format checks strict.
            if (index == len(variants) - 1 or str(error) not in {
                    "Artwork response exceeded size limit", "Motion download exceeded size limit"}):
                raise


def _download_rendition(variant, directory, stop):
    if stop and stop.is_set():
        raise ValueError("Motion download cancelled")
    playlist = _download(variant, 128 * 1024).decode("utf-8")
    if "#EXT-X-ENDLIST" not in playlist or "#EXT-X-KEY" in playlist:
        raise ValueError("Only finite, unencrypted motion covers are supported")
    duration = sum(float(value) for value in re.findall(r"#EXTINF:([\d.]+)", playlist))
    if not 0 < duration <= 60:
        raise ValueError("Motion cover exceeds 60 seconds")
    media_urls = {}
    rewritten = []
    for line in playlist.splitlines():
        if line.startswith("#EXT-X-MAP:"):
            match = re.search(r'URI="([^"]+)"', line)
            if not match:
                raise ValueError("Invalid initialization segment")
            raw_url = match[1]
        elif line and not line.startswith("#"):
            raw_url = line
        elif "URI=" in line:
            raise ValueError("Unsupported HLS media reference")
        else:
            rewritten.append(line)
            continue
        url = urljoin(variant, raw_url)
        parsed = urlsplit(url)
        if (not _safe_url(url, artwork=True) or parsed.hostname != "mvod.itunes.apple.com"
                or parsed.query or parsed.fragment):
            raise ValueError("Motion segment must be on Apple's media host")
        name = media_urls.setdefault(url, f"segment-{len(media_urls)}.mp4")
        rewritten.append(line.replace(raw_url, name) if line.startswith("#") else name)
    if not 1 <= len(media_urls) <= 32:
        raise ValueError("Too many motion segments")
    total = 0
    for url, name in media_urls.items():
        if stop and stop.is_set():
            raise ValueError("Motion download cancelled")
        content = _download(url, 8 * 1024 * 1024)
        total += len(content)
        if total > 24 * 1024 * 1024:
            raise ValueError("Motion download exceeded size limit")
        (directory / name).write_bytes(content)
    local = directory / "motion.m3u8"
    local.write_text("\n".join(rewritten), encoding="utf-8")
    return local


def animated_webp(content):
    """Validate actual RIFF animation chunks before uploading an image."""
    if not 20 <= len(content) <= MAX_IMAGE_BYTES or content[:4] != b"RIFF" or content[8:12] != b"WEBP":
        return False
    if int.from_bytes(content[4:8], "little") + 8 != len(content):
        return False
    offset, frames, animation = 12, 0, False
    while offset + 8 <= len(content):
        kind = content[offset:offset + 4]
        size = int.from_bytes(content[offset + 4:offset + 8], "little")
        # Encoder output needs only image/animation chunks. Never upload EXIF,
        # XMP, comments, or other opaque metadata even in an otherwise valid file.
        if kind not in {b"VP8X", b"ANIM", b"ANMF"}:
            return False
        if offset + 8 + size > len(content):
            return False
        animation |= kind == b"ANIM" and size == 6
        frames += kind == b"ANMF" and size >= 16
        offset += 8 + size + size % 2
    return offset == len(content) and animation and frames > 1


async def convert_motion(stream):
    import imageio_ffmpeg
    with tempfile.TemporaryDirectory(prefix="apple-music-motion-") as folder:
        directory = Path(folder)
        # Finish bounded file writes before deleting the temporary directory.
        stop = threading.Event()
        download = asyncio.create_task(asyncio.to_thread(download_video, stream, directory, stop))
        try:
            local = await asyncio.shield(download)
        except asyncio.CancelledError:
            stop.set()
            await asyncio.gather(download, return_exceptions=True)
            raise
        output = directory / "cover.webp"
        deadline = time.monotonic() + 60
        for size, fps in _ENCODING_ATTEMPTS:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("Motion conversion timed out")
            process = await asyncio.create_subprocess_exec(
                imageio_ffmpeg.get_ffmpeg_exe(), "-hide_banner", "-loglevel", "error", "-y",
                "-protocol_whitelist", "file", "-allowed_extensions", "ALL", "-i", str(local),
                "-vf", f"fps={fps},scale={size}:{size}:flags=lanczos,setpts=PTS-STARTPTS",
                "-map_metadata", "-1", "-an", "-c:v", "libwebp_anim",
                "-loop", "0", "-quality", "85", str(output),
                stdout=asyncio.subprocess.DEVNULL, stderr=asyncio.subprocess.DEVNULL,
                env={key: value for key, value in os.environ.items()
                     if key.upper() in {"SYSTEMROOT", "WINDIR", "PATH", "TEMP", "TMP"}},
                **({"creationflags": subprocess.CREATE_NO_WINDOW} if os.name == "nt" else {}))
            try:
                await asyncio.wait_for(process.wait(), max(0.001, deadline - time.monotonic()))
            finally:
                if process.returncode is None:
                    process.kill()
                    await process.wait()
            if process.returncode != 0:
                raise ValueError("Motion conversion failed")
            if output.stat().st_size > MAX_IMAGE_BYTES:
                continue
            content = output.read_bytes()
            if not animated_webp(content):
                raise ValueError("Converted cover is not animated WebP")
            return content
        raise ValueError("Converted cover exceeds size limit")


def github_token(repository):
    # The token stays in memory and is sent only to api.github.com. Never log it.
    token = os.environ.get("APPLE_MUSIC_PRESENCE_GITHUB_TOKEN")
    if token:
        return token
    result = subprocess.run(["git", "-c", "credential.interactive=false", "credential", "fill"],
        input=f"protocol=https\nhost=github.com\npath={repository}.git\n\n", capture_output=True,
        text=True, timeout=10, env={**os.environ, "GIT_TERMINAL_PROMPT": "0", "GCM_INTERACTIVE": "Never"},
        **({"creationflags": subprocess.CREATE_NO_WINDOW} if os.name == "nt" else {}))
    fields = dict(line.split("=", 1) for line in result.stdout.splitlines() if "=" in line)
    if not fields.get("password"):
        raise ValueError("GitHub sign-in is required for new motion cover uploads")
    return fields["password"]


class GithubArtworkHost:
    branch = "motion-artwork"

    def __init__(self, repository):
        if not re.fullmatch(REPOSITORY_PATTERN, repository):
            raise ValueError("Artwork host must be a GitHub owner/repository")
        self.repository = repository

    def publish(self, album_id, stream, content, stop=None):
        import base64
        if not album_id.isdigit() or not _apple_stream(stream) or not animated_webp(content):
            raise ValueError("Invalid motion cover")

        def checkpoint():
            if stop and stop.is_set():
                raise ValueError("Motion upload cancelled")

        checkpoint()
        token = github_token(self.repository)
        headers = {"Authorization": f"Bearer {token}", "Accept": "application/vnd.github+json",
                   "X-GitHub-Api-Version": "2022-11-28", "Content-Type": "application/json"}
        base = f"https://api.github.com/repos/{self.repository}"

        def api(path, method=None, data=None):
            checkpoint()
            encoded = json.dumps(data).encode() if data is not None else None
            return json.loads(_download(base + path, 256 * 1024, headers=headers, method=method, data=encoded))

        repo = api("")
        if repo.get("private") is not False or not repo.get("permissions", {}).get("push"):
            raise ValueError("Artwork host requires a writable public GitHub repository")
        checkpoint()
        identity = json.loads(_download("https://api.github.com/user", 64 * 1024, headers=headers))
        login, user_id = identity.get("login"), identity.get("id")
        if not isinstance(login, str) or not re.fullmatch(r"[A-Za-z0-9-]{1,39}", login) or type(user_id) is not int or user_id <= 0:
            raise ValueError("GitHub commit identity could not be verified")
        committer = {"name": login, "email": f"{user_id}+{login}@users.noreply.github.com"}
        try:
            ref = api(f"/git/ref/heads/{self.branch}")
        except HTTPError as error:
            if error.code != 404:
                raise
            parent = api(f"/git/ref/heads/{quote(repo['default_branch'], safe='')}")
            try:
                ref = api("/git/refs", "POST", {"ref": f"refs/heads/{self.branch}", "sha": parent["object"]["sha"]})
            except HTTPError as conflict:
                if conflict.code != 422:
                    raise
                ref = api(f"/git/ref/heads/{self.branch}")  # Another instance created it.
        digest = hashlib.sha256(f"{ENCODING_PROFILE}\0{stream}".encode()).hexdigest()[:12]
        filename = f"{album_id}-{digest}.webp"
        path = f"artwork/motion/{filename}"
        def public_url(commit):
            return f"https://raw.githubusercontent.com/{self.repository}/{commit}/{path}"
        url = public_url(ref["object"]["sha"])
        try:
            checkpoint()
            existing = _download(url, MAX_IMAGE_BYTES)
            if not animated_webp(existing):
                raise ValueError("Hosted cover is not animated WebP")
            return url
        except HTTPError as error:
            if error.code != 404:
                raise
        try:
            checkpoint()
            uploaded = api(f"/contents/{path}", "PUT", {"branch": self.branch,
                "message": f"Cache motion cover for Apple Music album {album_id}",
                "committer": committer, "author": committer,
                "content": base64.b64encode(content).decode("ascii")})
            url = public_url(uploaded["commit"]["sha"])
        except HTTPError as error:
            if error.code not in (409, 422):
                raise
            url = public_url(api(f"/git/ref/heads/{self.branch}")["object"]["sha"])
        for attempt in range(3):
            checkpoint()
            try:
                hosted = _download(url, MAX_IMAGE_BYTES)
                break
            except HTTPError as error:
                if error.code != 404 or attempt == 2:
                    raise
                delay = 0.5 * (attempt + 1)
                if stop:
                    stop.wait(delay)
                else:
                    time.sleep(delay)
        if not animated_webp(hosted):
            raise ValueError("Public cover verification failed")
        return url


@dataclass
class _MotionCache:
    cover: Artwork | None
    expires: float


class AutomaticArtworkResolver:
    def __init__(self, catalog, host, cache_path):
        self.catalog, self.host, self.cache_path = catalog, host, Path(cache_path)
        self._cache, self._jobs, self._static = OrderedDict(), {}, {}
        self._aliases = OrderedDict()
        self._statuses, self._current_key = {}, None
        self._serial = asyncio.Semaphore(1)
        self._load()

    @property
    def status(self):
        return self._statuses.get(self._current_key, "")

    def _key(self, artist, album, title=""):
        # Metadata aliases belong to a verified song. Only Apple album IDs
        # permit sharing the cached result across different songs or artists.
        return hashlib.sha256(f"{_normalize(artist)}\0{_normalize(album)}\0{_normalize(title)}".encode()).hexdigest()

    def _album_key(self, page):
        verified = album_page(page)
        if not verified:
            return None
        parts = urlsplit(verified).path.strip("/").split("/")
        return f"album:{parts[0].lower()}:{parts[-1]}"

    def _load(self):
        try:
            with self.cache_path.open("rb") as handle:
                raw = handle.read(128 * 1024 + 1)
            if len(raw) > 128 * 1024:
                return
            data = json.loads(raw.decode("utf-8"))
            if data.get("repository") != self.host.repository:
                return
            for key, item in list(data.get("albums", {}).items())[-128:]:
                cover = item.get("cover")
                if cover:
                    if data.get("encoding_profile") != ENCODING_PROFILE:
                        continue
                    pattern = (r"https://raw\.githubusercontent\.com/" + re.escape(self.host.repository)
                               + r"/[a-f0-9]{40}/artwork/motion/[0-9]+-[a-f0-9]{12}\.webp")
                    if not re.fullmatch(pattern, cover["url"]) or not album_page(cover["track_url"]):
                        continue
                    cover = Artwork(cover["url"], album_page(cover["track_url"]), animated=True)
                    # Migrate older artist-based entries using their verified
                    # album ID. Different guest artists share the same cover.
                    key = self._album_key(cover.track_url)
                expires = float(item["expires"])
                if time.time() < expires <= time.time() + 8 * 86400:
                    self._cache[key] = _MotionCache(cover, expires)
        except (OSError, ValueError, TypeError, KeyError, AttributeError, RecursionError):
            pass

    def _save(self):
        data = {"repository": self.host.repository, "encoding_profile": ENCODING_PROFILE, "albums": {key: {
            "cover": {"url": item.cover.url, "track_url": item.cover.track_url} if item.cover else None,
            "expires": item.expires} for key, item in self._cache.items()}}
        try:
            self.cache_path.parent.mkdir(parents=True, exist_ok=True)
            temporary = self.cache_path.with_suffix(".tmp")
            temporary.write_text(json.dumps(data), encoding="utf-8")
            temporary.replace(self.cache_path)
        except OSError:
            log.debug("Motion cache could not be saved")

    async def _prepare(self, key, page, artist, album):
        try:
            async with self._serial:
                self._statuses[key] = "Motion cover: checking Apple Music; using normal artwork"
                stream = await asyncio.to_thread(discover_motion, page, artist, album)
                cover = None
                if stream:
                    self._statuses[key] = "Motion cover: preparing animation; using normal artwork"
                    content = await convert_motion(stream)
                    self._statuses[key] = "Motion cover: uploading animation; using normal artwork"
                    stop = threading.Event()
                    upload = asyncio.create_task(asyncio.to_thread(self.host.publish,
                        urlsplit(page).path.rsplit("/", 1)[-1], stream, content, stop))
                    try:
                        url = await asyncio.shield(upload)
                    except asyncio.CancelledError:
                        stop.set()
                        await asyncio.gather(upload, return_exceptions=True)
                        raise
                    cover = Artwork(url, page, animated=True)
                self._cache[key] = _MotionCache(cover, time.time() + (7 if cover else 1) * 86400)
                self._cache.move_to_end(key)
                while len(self._cache) > 128:
                    discarded, _ = self._cache.popitem(last=False)
                    self._statuses.pop(discarded, None)
                self._statuses[key] = "" if cover else "Motion cover: unavailable for this album; using normal artwork"
                self._save()
        except asyncio.CancelledError:
            raise
        except Exception as error:
            # Retry transient failures; never misclassify an outage as no motion.
            self._cache[key] = _MotionCache(None, time.time() + 60)
            self._statuses[key] = ("Motion cover: GitHub access needs updating; using normal artwork"
                           if isinstance(error, HTTPError) and error.code in (401, 403)
                           else "Motion cover: temporarily unavailable; using normal artwork and retrying")
            log.debug("Optional motion artwork failed (%s)", type(error).__name__)
        finally:
            while len(self._cache) > 128:
                discarded, _ = self._cache.popitem(last=False)
                self._statuses.pop(discarded, None)
            self._jobs.pop(key, None)

    async def refresh(self, title, artist, album):
        key = self._key(artist, album, title)
        key = self._aliases.get(key, key)
        self._current_key = key
        cached = self._cache.get(key)
        if cached and cached.expires > time.time() and cached.cover:
            return cached.cover
        static = self._static.get(key)
        page = album_page(static.track_url) if static else None
        if page and key not in self._jobs and (not cached or cached.expires <= time.time()):
            # Keep only the currently requested album queued behind active work.
            if len(self._jobs) < 2:
                self._jobs[key] = asyncio.create_task(self._prepare(key, page, artist, album))
        return static

    async def resolve(self, title, artist, album):
        if not artist.strip() or not album.strip():
            return await self.catalog.resolve(title, artist, album)
        key = self._key(artist, album, title)
        metadata_key = key
        key = self._aliases.get(key, key)
        self._current_key = key
        cached = self._cache.get(key)
        if cached and cached.expires > time.time() and cached.cover:
            return cached.cover
        static = self._static.get(key)
        if not static:
            static = await self.catalog.resolve_album(artist, album)
        if not static:
            static = await self.catalog.resolve(title, artist, album)
        if static:
            canonical = self._album_key(static.track_url)
            if canonical:
                self._aliases[metadata_key] = canonical
                self._aliases.move_to_end(metadata_key)
                while len(self._aliases) > 128:
                    self._aliases.popitem(last=False)
                key = canonical
            self._static[key] = Artwork(static.url, album_page(static.track_url) or static.track_url)
            while len(self._static) > 128:
                self._static.pop(next(iter(self._static)))
        return await self.refresh(title, artist, album)

    async def close(self):
        jobs = list(self._jobs.values())
        for job in jobs:
            job.cancel()
        await asyncio.gather(*jobs, return_exceptions=True)
