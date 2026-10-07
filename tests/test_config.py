"""Local preference recovery stays bounded and never exposes file contents."""

import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from apple_music_presence.config import Settings, _MAX_SETTINGS_BYTES, load_settings, save_settings


class SettingsLoadTests(unittest.TestCase):
    def test_deeply_nested_settings_restore_defaults(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "settings.json"
            path.write_text('{"unknown":' + '[' * 5000 + '[]' + ']' * 5000 + '}', encoding="utf-8")
            self.assertLess(path.stat().st_size, _MAX_SETTINGS_BYTES)
            self.assertEqual(load_settings(path), Settings())

    def test_oversized_valid_settings_restore_defaults_before_parsing(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "settings.json"
            path.write_text(json.dumps({"client_id": "123456789012345678", "padding": "x" * _MAX_SETTINGS_BYTES}), encoding="utf-8")
            with patch("apple_music_presence.config.json.loads") as parser:
                self.assertEqual(load_settings(path), Settings())
                parser.assert_not_called()

    def test_invalid_utf8_and_nonobject_settings_restore_defaults(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "settings.json"
            for data in (b"\xff", b"[]", b"null", b"broken json"):
                with self.subTest(data=data):
                    path.write_bytes(data)
                    self.assertEqual(load_settings(path), Settings())

    def test_normal_preferences_still_round_trip(self):
        preferences = Settings(client_id="123456789012345678", artwork=True,
                               country="GB", source_id="Apple Music")
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "settings.json"
            save_settings(preferences, path)
            self.assertEqual(load_settings(path), preferences)
