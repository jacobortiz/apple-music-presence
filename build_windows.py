"""Build on Windows with: python -m pip install '.[build]'; python build_windows.py."""

from pathlib import Path
import sys


def main():
    if sys.platform != "win32":
        raise SystemExit("Build the Windows executable on Windows.")
    from PyInstaller.__main__ import run
    root = Path(__file__).resolve().parent
    run([
        "--noconfirm", "--onefile", "--hide-console", "hide-early",
        "--name", "AppleMusicPresence", "--paths", str(root / "src"),
        "--collect-all", "winrt", "--distpath", str(root / "dist"),
        "--workpath", str(root / "build"), "--specpath", str(root / "build"),
        str(root / "desktop_entry.py"),
    ])


if __name__ == "__main__":
    main()
