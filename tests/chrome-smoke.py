#!/usr/bin/env python3
"""Pixel and pointer checks of the shared shell in a private X11 session."""
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from PIL import Image, ImageGrab


PAPER = (205, 227, 214)


def x(*args):
    return subprocess.check_output(["xdotool", *map(str, args)], text=True, timeout=8).strip()


def wait(check, message):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if check():
            return
        time.sleep(.045)
    raise AssertionError(message)


def matches(pixel, expected=PAPER):
    return sum(abs(a - b) for a, b in zip(pixel, expected)) < 14


def run(binary):
    with tempfile.TemporaryDirectory() as temp:
        sample = Path(temp) / "zen-chrome.png"
        Image.new("RGB", (640, 480), PAPER).save(sample)
        for desktop, mode, squared in [("GNOME", "auto", False), ("niri", "auto", True),
                                        ("niri", "rounded", False), ("GNOME", "system", True)]:
            x("mousemove", 1050, 850)
            env = dict(os.environ, XDG_CURRENT_DESKTOP=desktop, ZEN_CORNERS=mode)
            env.pop("NIRI_SOCKET", None)
            app = subprocess.Popen([binary, str(sample)], env=env)
            try:
                win = x("search", "--sync", "--onlyvisible", "--pid", app.pid,
                        "--name", "zen-chrome.png").splitlines()[0]
                x("windowfocus", win)
                x("windowsize", "--sync", win, 640, 480)
                geometry = dict(line.split("=", 1) for line in x("getwindowgeometry", "--shell", win).splitlines())
                ox, oy = int(geometry["X"]), int(geometry["Y"])

                def grab():
                    return ImageGrab.grab(bbox=(ox, oy, ox+640, oy+480),
                                          xdisplay=os.environ["DISPLAY"]).convert("RGB")

                wait(lambda: matches(grab().getpixel((320, 240))), "image did not load")
                assert matches(grab().getpixel((1, 1))) == squared, (desktop, mode, "corner shape")
                def fades():
                    img = grab()
                    return tuple(sum(img.getpixel(p)) for p in ((105, 12), (105, 52), (105, 465)))

                if desktop == "GNOME" and mode == "auto":
                    x("mousemove", "--window", win, 320, 240)
                    assert all(v > 610 for v in fades()), "center hover must not reveal chrome"
                    x("mousemove", "--window", win, 530, 28)
                    wait(lambda: (lambda c: c[0] < 250 and c[0]+80 < c[1] and c[2] > 610)(fades()),
                         "top gradient did not reveal by itself")
                    x("mousemove", "--window", win, 320, 455)
                    wait(lambda: (lambda c: c[0] > 610 and c[2] < 250)(fades()),
                         "bottom gradient did not reveal by itself")
                    x("mousemove", "--window", win, 320, 240)
                    wait(lambda: all(v > 610 for v in fades()), "chrome did not fade from center")
                    x("mousemove", "--window", win, 613, 27)
                    wait(lambda: fades()[0] < 250, "direct hover over close did not reveal controls")
                    x("click", 1)
                else:
                    if desktop == "niri" and mode == "auto":
                        x("key", "Tab")
                        wait(lambda: (lambda c: c[0] < 250 and c[2] < 250)(fades()), "keyboard cannot reveal controls")
                    x("key", "ctrl+w")
                assert app.wait(timeout=5) == 0, "viewer did not close cleanly"
            finally:
                if app.poll() is None:
                    app.terminate()
                    app.wait(timeout=5)
        print("PASS shared edge-only fades, keyboard access, top-right close and corner policy")


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--run":
        run(sys.argv[2])
    else:
        app = str(Path(sys.argv[1] if len(sys.argv) > 1 else "build/zen-image").resolve())
        subprocess.run(["xvfb-run", "-a", "-s", "-screen 0 1280x1024x24", "dbus-run-session", "--", "env",
                        "GTK_A11Y=none", "GDK_BACKEND=x11", "GSK_RENDERER=cairo", "GSETTINGS_BACKEND=memory",
                        "G_DEBUG=fatal-warnings", "GIO_USE_VFS=local", sys.executable,
                        str(Path(__file__).resolve()), "--run", app], check=True)
