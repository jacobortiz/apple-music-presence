"""Tk desktop shell. Every media/IPC operation stays on a dedicated worker."""

import asyncio
from dataclasses import replace
import queue
import threading
import time
import tkinter as tk
from tkinter import messagebox, ttk
import webbrowser

from .config import Settings, save_settings
from .models import PlaybackState
from .service import ServiceStatus


def timestamp(seconds: float | None) -> str:
    if seconds is None:
        return "--:--"
    total = max(0, int(seconds))
    return f"{total // 60}:{total % 60:02}"


class DesktopApp:
    def __init__(self, settings: Settings, make_service, *, demo=False, seconds=None, start=False):
        self.settings, self.make_service, self.demo = settings, make_service, demo
        self.root = tk.Tk()
        self.root.title("Apple Music Presence" + (" — Offline preview" if demo else ""))
        self.root.geometry("650x660")
        self.root.minsize(590, 645)
        self.root.configure(bg="#111318")
        self.root.protocol("WM_DELETE_WINDOW", self.close)
        self.events = queue.SimpleQueue()
        self.worker = None
        self.stop_event = threading.Event()
        self.closing = False
        self.worker_error = None
        self.last_status: ServiceStatus | None = None
        self._configure_style()
        self._build()
        self.root.after(100, self._pump)
        if demo or start:
            self.root.after(100, self.start)
        if seconds is not None:
            self.root.after(max(100, int(seconds * 1000)), self.close)

    def _configure_style(self):
        style = ttk.Style(self.root)
        style.theme_use("clam")
        style.configure("TFrame", background="#111318")
        style.configure("Card.TFrame", background="#1e222b")
        style.configure("TLabel", background="#111318", foreground="#f2f3f5", font=("Segoe UI", 10))
        style.configure("Muted.TLabel", foreground="#a9b1c3")
        style.configure("Title.TLabel", font=("Segoe UI Semibold", 24))
        style.configure("Song.TLabel", background="#1e222b", font=("Segoe UI Semibold", 19))
        style.configure("Card.TLabel", background="#1e222b", foreground="#bac2d2")
        style.configure("TButton", font=("Segoe UI Semibold", 10), padding=(16, 10))
        style.configure("TCheckbutton", background="#111318", foreground="#f2f3f5", font=("Segoe UI", 10))
        style.map("TCheckbutton", background=[("active", "#111318")])
        style.configure("TEntry", padding=9, fieldbackground="#262b35", foreground="#ffffff")
        style.configure("Horizontal.TProgressbar", background="#fa4d70", troughcolor="#343c4a", borderwidth=0)

    def _build(self):
        main = ttk.Frame(self.root, padding=28)
        main.pack(fill="both", expand=True)
        ttk.Label(main, text="Apple Music Presence", style="Title.TLabel").pack(anchor="w")
        subtitle = "Offline preview · nothing is sent to Discord" if self.demo else "Your current track, on your Discord profile."
        ttk.Label(main, text=subtitle, style="Muted.TLabel").pack(anchor="w", pady=(5, 22))
        card = ttk.Frame(main, style="Card.TFrame", padding=22)
        card.pack(fill="x")
        self.state_label = ttk.Label(card, text="READY WHEN YOU ARE", style="Card.TLabel")
        self.state_label.pack(anchor="w")
        self.song_label = ttk.Label(card, text="Play something you love", style="Song.TLabel", wraplength=520)
        self.song_label.pack(anchor="w", pady=(12, 5))
        self.artist_label = ttk.Label(card, text="Artist", style="Card.TLabel", wraplength=520)
        self.artist_label.pack(anchor="w")
        self.album_label = ttk.Label(card, text="Album", style="Card.TLabel", wraplength=520)
        self.album_label.pack(anchor="w", pady=(3, 14))
        self.progress = ttk.Progressbar(card, maximum=100)
        self.progress.pack(fill="x")
        self.time_label = ttk.Label(card, text="--:-- / --:--", style="Card.TLabel")
        self.time_label.pack(anchor="w", pady=(6, 0))
        ttk.Label(main, text="Discord Application ID").pack(anchor="w", pady=(21, 6))
        self.client_id = tk.StringVar(value=self.settings.client_id)
        self.id_entry = ttk.Entry(main, textvariable=self.client_id)
        self.id_entry.pack(fill="x")
        self.artwork_enabled = tk.BooleanVar(value=self.settings.artwork)
        self.art_check = ttk.Checkbutton(main, text="Find album art using Apple’s public catalog", variable=self.artwork_enabled)
        self.art_check.pack(anchor="w", pady=(13, 2))
        ttk.Label(main, text="Optional: sends song, artist, and album to Apple for matching.",
                  style="Muted.TLabel", wraplength=560).pack(anchor="w")
        actions = ttk.Frame(main)
        actions.pack(fill="x", pady=(18, 12))
        self.start_button = ttk.Button(actions, text="Start sharing", command=self.start)
        self.start_button.pack(side="left")
        self.stop_button = ttk.Button(actions, text="Stop", command=self.stop, state="disabled")
        self.stop_button.pack(side="left", padx=8)
        ttk.Button(actions, text="Developer Portal ↗", command=lambda: webbrowser.open(
            "https://discord.com/developers/applications")).pack(side="right")
        self.status_label = ttk.Label(main, text="Add your Application ID, then start sharing.",
                                      style="Muted.TLabel", wraplength=560)
        self.status_label.pack(anchor="w", fill="x")
        self.art_status_label = ttk.Label(main, text="", style="Muted.TLabel", wraplength=560)
        self.art_status_label.pack(anchor="w", fill="x", pady=(5, 0))
        if self.demo:
            self.start_button.configure(text="Start preview")
            self.id_entry.configure(state="disabled")
            self.art_check.configure(state="disabled")

    def start(self):
        if self.worker and self.worker.is_alive():
            return
        settings = replace(self.settings, client_id=self.client_id.get(), artwork=self.artwork_enabled.get())
        try:
            settings.validate(demo=self.demo)
            if not self.demo:
                save_settings(settings)
        except (ValueError, OSError) as exc:
            messagebox.showerror("Check setup", str(exc), parent=self.root)
            return
        self.settings = settings
        self.stop_event = threading.Event()
        self.last_status = None
        self.worker_error = None
        self.start_button.configure(state="disabled")
        self.stop_button.configure(state="normal")
        self.id_entry.configure(state="disabled")
        self.art_check.configure(state="disabled")
        self.status_label.configure(text="Connecting…")
        self.art_status_label.configure(text="")

        def work():
            try:
                service = self.make_service(settings, self.events.put, self.demo)
                asyncio.run(service.run(self.stop_event))
            except Exception as exc:
                self.events.put(("error", str(exc)))
            finally:
                self.events.put(("done", ""))

        self.worker = threading.Thread(target=work, name="presence-worker", daemon=True)
        self.worker.start()

    def stop(self):
        self.stop_event.set()
        self.stop_button.configure(state="disabled")
        self.status_label.configure(text="Stopping and clearing Discord presence…")

    def close(self):
        self.closing = True
        if self.worker and self.worker.is_alive():
            self.stop()
        else:
            self.root.destroy()

    def _pump(self):
        while not self.events.empty():
            event = self.events.get()
            if isinstance(event, ServiceStatus):
                self.last_status = event
                if not self.stop_event.is_set():
                    prefix = "Preview: " if self.demo else ""
                    self.status_label.configure(text=prefix + event.message)
                    self.art_status_label.configure(text="Album art: disabled in offline preview" if self.demo else event.artwork_status)
            elif event[0] == "error":
                self.worker_error = event[1]
                self.status_label.configure(text="Unable to start: " + self.worker_error)
            elif event[0] == "done":
                self.last_status = None
                self.art_status_label.configure(text="")
                if not self.worker_error:
                    self.status_label.configure(text="Stopped. Discord presence cleared or disconnected.")
                self.state_label.configure(text="STOPPED")
                self.start_button.configure(state="normal")
                self.stop_button.configure(state="disabled")
                if not self.demo:
                    self.id_entry.configure(state="normal")
                    self.art_check.configure(state="normal")
        if self.closing:
            if not self.worker or not self.worker.is_alive():
                self.root.destroy()
                return
        if self.last_status:
            snapshot = self.last_status.snapshot
            track = snapshot.track
            self.state_label.configure(text=snapshot.state.value.upper())
            self.song_label.configure(text=track.title if track else "Waiting for Apple Music")
            self.artist_label.configure(text=(track.artist or "Unknown artist") if track else "Open Apple Music and play a track.")
            self.album_label.configure(text=(track.album or "Album unavailable") if track else "")
            position = snapshot.position_at(time.time())
            duration = snapshot.duration
            self.progress["value"] = 100 * position / duration if position is not None and duration and duration > 0 else 0
            self.time_label.configure(text=f"{timestamp(position)} / {timestamp(duration)}")
        self.root.after(100, self._pump)

    def run(self):
        self.root.mainloop()
