#!/usr/bin/env python3
"""Exercise text selection and both Linux clipboards in a private X11 session.

Test-only dependencies: xvfb xauth xdotool xclip dbus-daemon python3-pil.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

from PIL import ImageGrab


ROOT = Path(__file__).resolve().parents[1]
FIRST_LINE = "One café naïve €"
SECOND_LINE = "Two lines of text"
ALL_TEXT = FIRST_LINE + "\n" + SECOND_LINE + "\nThree anchors"


def x(*args):
    return subprocess.check_output(["xdotool", *map(str, args)], text=True, timeout=6).strip()


def wait_for(check, label, timeout=4):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        result = check()
        if result:
            return result
        time.sleep(.04)
    raise AssertionError(label)


def clipboard(selection="clipboard"):
    result = subprocess.run(["xclip", "-selection", selection, "-o", "-target", "UTF8_STRING"],
                            capture_output=True, timeout=2)
    return result.stdout.decode().strip() if result.returncode == 0 else ""


def put_clipboard(text):
    subprocess.run(["xclip", "-selection", "clipboard", "-i", "-target", "UTF8_STRING"],
                   input=text.encode(), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                   timeout=2, check=True)
    wait_for(lambda: clipboard() == text, "Could not seed CLIPBOARD")


def pasted_text_is(expected, selection="clipboard"):
    # Poppler may place a blank line between separate text runs.
    value = clipboard(selection)
    return "\n".join(line for line in value.splitlines() if line.strip()) == expected


def expect_copy(expected):
    put_clipboard("sentinel")
    x("key", "ctrl+c")
    try:
        wait_for(lambda: pasted_text_is(expected), f"Expected {expected!r} on CLIPBOARD")
    except AssertionError as exc:
        raise AssertionError(f"{exc}; got {clipboard()!r}") from exc


def untouched_after_copy(description):
    put_clipboard("sentinel")
    x("key", "ctrl+c")
    time.sleep(.17)
    assert clipboard() == "sentinel", description


def run(directory):
    doc = directory / "text-probe.pdf"
    other = directory / "other-probe.pdf"
    generator = ROOT / "tests/make-fixture.py"
    subprocess.run([sys.executable, generator, "--selection", doc], check=True)
    subprocess.run([sys.executable, generator, other], check=True)
    app = subprocess.Popen([os.environ.get("ZEN_PDF_BIN", str(ROOT / "zen-pdf")), doc])
    try:
        win = x("search", "--sync", "--onlyvisible", "--pid", app.pid, "--name", doc.name).splitlines()[0]
        x("windowfocus", win)
        x("windowsize", "--sync", win, 600, 590)
        origin = dict(line.split("=", 1) for line in x("getwindowgeometry", "--shell", win).splitlines())
        ox, oy = int(origin["X"]), int(origin["Y"])

        def screen(x1, y1, x2, y2):
            return ImageGrab.grab(bbox=(ox+x1, oy+y1, ox+x2, oy+y2), xdisplay=os.environ["DISPLAY"]).convert("RGB")

        def move(xpos, ypos):
            x("mousemove", "--window", win, xpos, ypos)

        def drag(x1, y1, x2, y2):
            move(x1, y1)
            x("mousedown", "1")
            x("sleep", ".06", "mousemove", "--window", win, (x1+x2)//2, (y1+y2)//2,
              "sleep", ".05", "mousemove", "--window", win, x2, y2, "sleep", ".06", "mouseup", "1")

        def clicks(count, xpos, ypos):
            # Break the previous selection gesture's multi-click sequence.
            time.sleep(.45)
            move(xpos, ypos)
            x("click", "--repeat", count, "--delay", 60, 1)

        # Black letters on white paper, rather than the initial unrendered page.
        wait_for(lambda: sum(screen(80, 160, 265, 186).convert("L").histogram()[:96]) > 200,
                 "Fixture text did not paint")
        normal = screen(75, 155, 270, 224).tobytes()
        drag(81, 174, 252, 212)
        wait_for(lambda: pasted_text_is(FIRST_LINE+"\n"+SECOND_LINE, "primary"),
                 "Multiline drag did not populate PRIMARY")
        expect_copy(FIRST_LINE+"\n"+SECOND_LINE)
        wait_for(lambda: screen(75, 155, 270, 224).tobytes() != normal, "No visible selection highlight")
        x("key", "Escape")
        wait_for(lambda: screen(75, 155, 270, 224).tobytes() == normal, "Escape left a highlight")
        untouched_after_copy("Escape left stale text available")
        print("PASS multiline drag, Unicode CLIPBOARD/PRIMARY and dismiss", flush=True)

        drag(241, 174, 185, 174)
        expect_copy("naïve")
        clicks(2, 151, 174)
        expect_copy("café")
        clicks(3, 160, 211)
        expect_copy(SECOND_LINE)
        x("key", "ctrl+a")
        expect_copy(ALL_TEXT)
        print("PASS reverse drag, word, line and current-page selection", flush=True)
        x("key", "ctrl+b")
        clicks(2, 232, 174)
        expect_copy("naïve")
        x("key", "ctrl+b", "ctrl+a")
        expect_copy(ALL_TEXT)
        print("PASS selection coordinates while page sidebar is open", flush=True)

        x("windowsize", "--sync", win, 720, 600)
        time.sleep(.1)
        x("key", "plus", "Down")
        expect_copy(ALL_TEXT)
        # Width scale 1.2 times zoom 1.2. Zoom anchors the viewport center, then Down scrolls 64.
        clicks(2, 210, 236)
        expect_copy("anchors")
        print("PASS transformed text hits after resize, zoom and scroll", flush=True)

        x("key", "Right", "w")
        untouched_after_copy("Page change left stale text available")
        x("windowsize", "--sync", win, 600, 590)
        # p2's /Rotate 90 turns 'Rotated' into vertical lettering. The width-fit page
        # is 600 by 400 with a 95-pixel top margin. It is not an unrotated hit box.
        clicks(2, 483, 173)
        expect_copy("Rotated")
        print("PASS rotated page and page-change reset", flush=True)

        x("key", "ctrl+l", "ctrl+a")
        expect_copy("2")
        put_clipboard("3")
        x("key", "ctrl+v", "Return")
        wait_for(lambda: screen(310, 230, 311, 231).getpixel((0, 0))[0] < 210,
                 "Native page entry did not paste 3 and navigate")
        before = screen(280, 175, 315, 320).tobytes()
        x("key", "ctrl+a")
        untouched_after_copy("Image-only page produced selectable text")
        drag(350, 405, 350, 250)
        wait_for(lambda: before != screen(280, 175, 315, 320).tobytes(), "Image-only page did not pan")
        print("PASS editable field copy/paste, image-only page and pan", flush=True)

        x("key", "Home", "ctrl+a")
        expect_copy(ALL_TEXT)
        x("key", "ctrl+o")
        dialog = x("search", "--sync", "--onlyvisible", "--name", "^Open").splitlines()[-1]
        x("windowfocus", dialog)
        put_clipboard(str(other))
        x("key", "ctrl+l", "ctrl+a", "ctrl+v")
        time.sleep(.15)
        x("key", "Return")
        wait_for(lambda: other.name in x("getwindowname", win), "The replacement PDF did not open")
        x("windowfocus", win)
        untouched_after_copy("New document left stale text available")
        print("PASS document replacement resets selection", flush=True)
        x("key", "ctrl+w")
        assert app.wait(timeout=5) == 0, "Viewer ended with an error"
    finally:
        if app.poll() is None:
            app.terminate()
            app.wait(timeout=5)


if __name__ == "__main__":
    if "--inside-display" in sys.argv:
        # Scope strict GTK diagnostics to the viewer, not unrelated D-Bus services.
        os.environ.update(GTK_A11Y="none", GSETTINGS_BACKEND="memory", GIO_USE_VFS="local",
                          GTK_USE_PORTAL="0", GDK_DEBUG="no-portals", GDK_BACKEND="x11", GSK_RENDERER="cairo",
                          G_DEBUG="fatal-warnings")
        with tempfile.TemporaryDirectory() as tmp:
            run(Path(tmp))
    else:
        for tool in ("xdotool", "xclip", "xvfb-run", "dbus-run-session"):
            if not shutil.which(tool):
                raise SystemExit(f"Missing test tool: {tool}")
        children = ["xvfb-run", "-a", "-s", "-screen 0 1280x1024x24", "dbus-run-session", "--",
                    sys.executable, str(Path(__file__).resolve()), "--inside-display"]
        raise SystemExit(subprocess.run(children).returncode)
