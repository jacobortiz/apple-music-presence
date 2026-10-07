import sys
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from apple_music_presence.config import Settings
try:
    import tkinter as tk
    from apple_music_presence.ui import DesktopApp
except ImportError:
    tk = None


@unittest.skipIf(tk is None, "Tk desktop unavailable")
class DesktopLayoutTests(unittest.TestCase):
    def test_long_metadata_keeps_controls_reachable_on_small_screen(self):
        root = None
        try:
            root = tk.Tk()
            root.withdraw()
            root.attributes("-alpha", 0)
            root.overrideredirect(True)
            if sys.platform == "win32":
                root.attributes("-disabled", True)
        except tk.TclError as error:
            if root is not None:
                root.destroy()
            self.skipTest(f"Tk desktop unavailable: {error}")
        try:
            with patch("apple_music_presence.ui.tk.Tk", return_value=root):
                with patch.object(root, "winfo_screenheight", return_value=768):
                    app = DesktopApp(Settings(), lambda *unused: self.fail("Layout must not start sharing"))
            root.deiconify()
            root.update()
            self.assertLessEqual(root.winfo_height(), 700)
            self.assertLessEqual(root.minsize()[1], 420)
            for repeat in (3, 6, 12):
                with self.subTest(repeat=repeat):
                    title = "Symphony No. 9 in D minor, Op. 125: IV. Presto " * repeat
                    app.song_label.configure(text=title)
                    app.artist_label.configure(text="Artist and Orchestra " * repeat)
                    app.album_label.configure(text="Live Concert Album " * repeat)
                    root.update_idletasks()
                    self.assertEqual(app.song_label["text"], title)
                    self.assertGreater(app.content.winfo_height(), app.canvas.winfo_height())
                    app.canvas.yview_moveto(1)
                    root.update_idletasks()
                    self.assert_control_visible(app)
                    app.canvas.yview_moveto(0)
                    app._reveal_focus(SimpleNamespace(widget=app.start_button))
                    root.update_idletasks()
                    self.assert_control_visible(app)
        finally:
            root.destroy()

    def assert_control_visible(self, app):
        top = app.start_button.winfo_rooty() - app.canvas.winfo_rooty()
        self.assertGreaterEqual(top, 0)
        self.assertLessEqual(top + app.start_button.winfo_height(), app.canvas.winfo_height())


if __name__ == "__main__":
    unittest.main()
