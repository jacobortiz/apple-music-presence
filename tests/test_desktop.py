import tempfile
import threading
import unittest
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from unittest.mock import patch

from apple_music_presence.config import Settings, load_settings, save_settings


class SettingsTests(unittest.TestCase):
    def test_offline_preview_ignores_unused_live_setup(self):
        settings = Settings(country="invalid", motion_artwork=True, artwork_repository="")
        settings.validate(demo=True)
        self.assertFalse(settings.artwork)

    def test_live_setup_still_rejects_invalid_country_and_motion_host(self):
        for settings in (
            Settings(client_id="123456789012345678", country="invalid"),
            Settings(client_id="123456789012345678", motion_artwork=True),
        ):
            with self.subTest(settings=settings), self.assertRaises(ValueError):
                settings.validate()

    def test_concurrent_saves_keep_a_complete_file_and_both_succeed(self):
        both_written = threading.Barrier(2)
        first_replaced = threading.Event()
        writer = threading.local()
        original_replace = Path.replace
        preferences = [Settings(client_id=str(index)) for index in range(2)]

        def replace_after_both_writes(temporary, destination):
            both_written.wait(timeout=5)
            if writer.index == 1:
                self.assertTrue(first_replaced.wait(timeout=5))
            try:
                return original_replace(temporary, destination)
            finally:
                if writer.index == 0:
                    first_replaced.set()

        with tempfile.TemporaryDirectory() as folder:
            destination = Path(folder) / "settings.json"

            def save(index):
                writer.index = index
                save_settings(preferences[index], destination)

            with patch.object(Path, "replace", replace_after_both_writes):
                with ThreadPoolExecutor(max_workers=2) as workers:
                    futures = [workers.submit(save, index) for index in range(2)]
                    for future in futures:
                        future.result(timeout=10)
            self.assertEqual(load_settings(destination), preferences[1])
            self.assertEqual(list(Path(folder).iterdir()), [destination])

    def test_failed_save_preserves_preferences_and_removes_temporary_file(self):
        with tempfile.TemporaryDirectory() as folder:
            destination = Path(folder) / "settings.json"
            original = Settings(client_id="123456789012345678")
            save_settings(original, destination)
            with patch.object(Path, "replace", side_effect=OSError("Cannot replace settings")):
                with self.assertRaises(OSError):
                    save_settings(Settings(client_id="987654321098765432"), destination)
            self.assertEqual(load_settings(destination), original)
            self.assertEqual(list(Path(folder).iterdir()), [destination])


if __name__ == "__main__":
    unittest.main()
