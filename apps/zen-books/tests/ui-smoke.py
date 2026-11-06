#!/usr/bin/env python3
"""Read, navigate and reopen real EPUB and CBZ windows on a private X11 display."""
import configparser
from contextlib import contextmanager
from io import BytesIO
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from zipfile import ZipFile

from PIL import Image, ImageGrab


ROOT = Path(__file__).resolve().parents[3]
WIDTH, HEIGHT = 660, 550


def x(*args):
    return subprocess.check_output(["xdotool", *map(str, args)], timeout=8, text=True).strip()


def wait(check, message):
    until = time.monotonic() + 5
    while time.monotonic() < until:
        if check():
            return
        time.sleep(.065)
    raise AssertionError(message)


@contextmanager
def reader(binary, file, title, state):
    x("mousemove", 1150, 880)
    app = subprocess.Popen([binary, file], env=dict(os.environ, XDG_STATE_HOME=str(state)))
    try:
        win = x("search", "--sync", "--onlyvisible", "--pid", app.pid, "--name", title).splitlines()[0]
        x("windowmove", "--sync", win, 50, 60)
        x("windowsize", "--sync", win, WIDTH, HEIGHT)
        x("windowfocus", win)
        geometry = dict(s.split("=", 1) for s in x("getwindowgeometry", "--shell", win).splitlines())
        ox, oy = int(geometry["X"]), int(geometry["Y"])

        def shot():
            return ImageGrab.grab(bbox=(ox, oy, ox+WIDTH, oy+HEIGHT),
                                  xdisplay=os.environ["DISPLAY"]).convert("RGB")

        yield win, shot
        x("key", "ctrl+w")
        assert app.wait(timeout=6) == 0, "Reader failed to close cleanly"
    finally:
        if app.poll() is None:
            app.terminate()
            app.wait(timeout=5)


def copy_chapter():
    x("key", "ctrl+a", "ctrl+c")
    result = subprocess.run(["xclip", "-selection", "clipboard", "-o", "-target", "UTF8_STRING"],
                            capture_output=True, text=True, timeout=5)
    return result.stdout


def run(binary, directory):
    epub = ROOT / "examples/above-the-fog.epub"
    state = directory / "state"
    with reader(binary, epub, "Above the fog", state) as (win, shot):
        wait(lambda: min(shot().getpixel((27, 180))) > 210, "EPUB page did not render")

        def has_art():
            image = shot().crop((55, 270, 605, 510)).resize((55, 24))
            pixels = image.tobytes()
            return sum(b > r + 12 for r, b in zip(pixels[::3], pixels[2::3])) > 50

        wait(has_art, "inline illustration did not render")
        wait(lambda: "The first light reached" in copy_chapter(), "EPUB text could not be copied")
        x("key", "ctrl+b")
        wait(lambda: max(shot().getpixel((24, 220))) < 90, "contents panel did not open")
        x("mousemove", "--window", win, 102, 176, "click", 1)
        wait(lambda: "The trail dropped back" in copy_chapter(), "TOC did not open chapter two")
        x("key", "n")
        wait(lambda: max(shot().getpixel((12, 215))) < 90, "night mode did not apply")
    print("PASS EPUB text, inline illustration, selectable copy, contents and night mode", flush=True)

    with reader(binary, epub, "Above the fog", state) as (_, shot):
        wait(lambda: "The trail dropped back" in copy_chapter(), "did not resume chapter two")
        wait(lambda: max(shot().getpixel((12, 215))) < 90, "night mode did not persist")
        x("key", "Left")
        wait(lambda: "The first light reached" in copy_chapter(), "Left did not return to chapter one")
        x("key", "n", "Page_Down")
        time.sleep(.28)
    files = list((state / "zen-books").iterdir())
    assert len(files) == 1
    saved = configparser.ConfigParser()
    saved.read(files[0])
    assert saved.getint("Reading", "chapter") == 0 and saved.getint("Reading", "offset") > 0, dict(saved["Reading"])
    with reader(binary, epub, "Above the fog", state) as (_, shot):
        wait(lambda: min(shot().getpixel((12, 215))) > 210, "day theme did not restore")
        time.sleep(.35)
    restored = configparser.ConfigParser()
    restored.read(files[0])
    assert restored.getint("Reading", "offset") > 0, dict(restored["Reading"])
    print("PASS saved chapter, night preference and in-chapter position", flush=True)

    cbz = directory / "Color walk.cbz"
    green, blue = (42, 147, 84), (55, 82, 190)
    with ZipFile(cbz, "w") as archive:
        for filename, color in (("page 10.png", blue), ("page 2.png", green)):
            data = BytesIO()
            Image.new("RGB", (420, 600), color).save(data, format="PNG")
            archive.writestr(filename, data.getvalue())

    def matches(shot, color):
        return sum(abs(a-b) for a,b in zip(shot().getpixel((WIDTH//2, HEIGHT//2)), color)) < 20

    with reader(binary, cbz, "Color walk", state) as (_, shot):
        wait(lambda: matches(shot, green), "CBZ first page did not use natural order")
        x("key", "Right")
        wait(lambda: matches(shot, blue), "Right did not turn the comic page")
    with reader(binary, cbz, "Color walk", state) as (_, shot):
        wait(lambda: matches(shot, blue), "CBZ did not resume its page")
    print("PASS CBZ natural order, decoded page rendering and resume", flush=True)


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--inside-display":
        with tempfile.TemporaryDirectory(prefix="zen-books-ui-") as temp:
            run(sys.argv[2], Path(temp))
    else:
        binary = Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / "build/zen-books").resolve()
        subprocess.run(["xvfb-run", "-a", "-s", "-screen 0 1280x1024x24", "dbus-run-session", "--", "env",
                        "GTK_A11Y=none", "GDK_BACKEND=x11", "GSK_RENDERER=cairo", "GSETTINGS_BACKEND=memory",
                        "G_DEBUG=fatal-warnings", "GDK_DEBUG=no-portals", sys.executable, str(Path(__file__).resolve()),
                        "--inside-display", str(binary)], check=True)
