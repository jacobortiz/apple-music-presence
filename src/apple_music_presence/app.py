"""Application composition and command-line entry point."""

import argparse
import asyncio
import json
import logging
import math
import signal
import sys
import threading

from .config import load_settings
from .service import PresenceService


def make_service(settings, notify, demo=False):
    if demo:
        from .demo import DemoBackend, DemoRpc
        return PresenceService(DemoBackend(), DemoRpc(), notify=notify)
    from .discord_rpc import DiscordRpc
    from .media import WindowsMediaBackend
    from .artwork import ItunesArtworkResolver
    resolver = ItunesArtworkResolver(country=settings.country) if settings.artwork else None
    return PresenceService(WindowsMediaBackend(source_id=settings.source_id or None),
                           DiscordRpc(settings.client_id), artwork=resolver, notify=notify)


async def diagnose(source_id):
    from .media import WindowsMediaBackend
    backend = WindowsMediaBackend(source_id=source_id or None)
    try:
        sessions = await backend.list_sessions()
        snapshot = await backend.read()
        print(json.dumps({
            "sessions": sessions,
            "selected_source": snapshot.source_id,
            "state": snapshot.state.value,
            "track": vars(snapshot.track) if snapshot.track else None,
            "position_seconds": snapshot.position,
            "duration_seconds": snapshot.duration,
        }, indent=2, ensure_ascii=False))
    finally:
        await backend.close()


def main(argv=None):
    parser = argparse.ArgumentParser(description="Apple Music → Discord Rich Presence for Windows")
    parser.add_argument("--client-id", help="Discord Application ID (not a token)")
    parser.add_argument("--headless", action="store_true", help="Run without the desktop window")
    parser.add_argument("--start", action="store_true", help="Start sharing immediately using saved settings")
    parser.add_argument("--demo", action="store_true", help="Offline preview; never connects to Discord or Apple")
    parser.add_argument("--diagnose", action="store_true", help="Print local media sessions and exit; no Discord writes")
    parser.add_argument("--source-id", help="Exact Windows media source ID override")
    parser.add_argument("--artwork", action=argparse.BooleanOptionalAction, default=None,
                        help="Opt into public Apple catalog matching for artwork")
    parser.add_argument("--country", help="Two-letter Apple catalog country code; default US")
    parser.add_argument("--seconds", type=float, help="Exit after this many seconds (useful for a smoke test)")
    parser.add_argument("--verbose", action="store_true", help="Enable technical diagnostic logging")
    args = parser.parse_args(argv)
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.WARNING,
                        format="%(levelname)s %(name)s: %(message)s")
    if args.seconds is not None and (not math.isfinite(args.seconds) or args.seconds <= 0):
        parser.error("--seconds must be finite and greater than zero")
    if sys.platform != "win32" and not args.demo:
        parser.error("Live detection requires Windows 10 1809 or newer; use --demo for an offline preview")
    settings = load_settings()
    for key in ("client_id", "source_id", "artwork", "country"):
        value = getattr(args, key)
        if value is not None:
            setattr(settings, key, value)
    try:
        if args.diagnose:
            asyncio.run(diagnose(settings.source_id))
        elif args.headless:
            settings.validate(demo=args.demo)
            stop = threading.Event()
            signal.signal(signal.SIGINT, lambda *unused: stop.set())
            signal.signal(signal.SIGTERM, lambda *unused: stop.set())
            last_message = None

            def notify(status):
                nonlocal last_message
                if status.message != last_message:
                    print(("Offline preview: " if args.demo else "") + status.message, flush=True)
                    last_message = status.message

            async def run():
                timer = None
                if args.seconds:
                    timer = asyncio.get_running_loop().call_later(args.seconds, stop.set)
                try:
                    await make_service(settings, notify, args.demo).run(stop)
                finally:
                    if timer:
                        timer.cancel()

            asyncio.run(run())
        else:
            from .ui import DesktopApp
            DesktopApp(settings, make_service, demo=args.demo, seconds=args.seconds, start=args.start).run()
    except (ValueError, RuntimeError, OSError, ImportError) as exc:
        print(f"Apple Music Presence: {exc}", file=sys.stderr)
        return 1
    return 0
