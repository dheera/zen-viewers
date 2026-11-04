#!/bin/sh
# Optional native-window test: apt install xvfb xauth xdotool dbus-daemon python3-pil
set -eu
cd "$(dirname "$0")/.."
tempdir=$(mktemp -d)
trap 'rm -rf -- "$tempdir"' EXIT
python3 tests/make-fixture.py "$tempdir/zen-test.pdf"
xvfb-run -a -s '-screen 0 1280x1024x24' dbus-run-session -- \
  env GTK_A11Y=none GSETTINGS_BACKEND=memory GIO_USE_VFS=local GDK_BACKEND=x11 \
  GSK_RENDERER=cairo G_DEBUG=fatal-warnings python3 - "$tempdir/zen-test.pdf" <<'PY'
import os
import subprocess
import sys
import time

from PIL import ImageGrab


def x(*args):
    return subprocess.check_output(["xdotool", *map(str, args)], text=True,
                                   timeout=15).strip()


def screenshot():
    return ImageGrab.grab(xdisplay=os.environ["DISPLAY"]).convert("RGB")


def wait_for(check, description):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if check():
            print("PASS", description, flush=True)
            return
        time.sleep(.06)
    raise AssertionError(description)


def near(actual, expected):
    return sum(abs(a - b) for a, b in zip(actual, expected)) < 12


app = subprocess.Popen([os.environ.get("ZEN_PDF_BIN", "./zen-pdf"), sys.argv[1]])
try:
    window = x("search", "--sync", "--onlyvisible", "--pid", app.pid,
               "--name", "zen-test.pdf").splitlines()[0]
    x("windowfocus", window)
    x("windowsize", "--sync", window, 700, 600)
    x("mousemove", 1200, 900)
    portrait, landscape, tall = (227, 235, 215), (242, 224, 194), (217, 230, 235)

    def page_is(color):
        return near(screenshot().getpixel((10, 200)), color)

    wait_for(lambda: page_is(portrait), "open portrait PDF")
    x("key", "Right")
    wait_for(lambda: page_is(landscape), "right arrow advances")
    x("key", "Left")
    wait_for(lambda: page_is(portrait), "left arrow goes back")

    # The portrait page fills this viewport. In idle mode the lower center is paper.
    strip = (215, 548, 485, 567)
    def dark_fraction():
        counts = screenshot().crop(strip).convert("L").histogram()
        return sum(counts[:90]) / sum(counts)

    x("mousemove", "--window", window, 350, 300)
    wait_for(lambda: dark_fraction() > .65, "pointer reveals controls")
    x("mousemove", 1200, 900)
    wait_for(lambda: dark_fraction() < .1, "leaving the window hides controls")

    x("windowsize", "--sync", window, 340, 260)
    x("key", "End")
    wait_for(lambda: page_is(tall), "resize and open tall page")
    band = (50, 80, 290, 180)
    before = screenshot().crop(band)
    x("key", "Down")
    wait_for(lambda: screenshot().crop(band).tobytes() != before.tobytes(),
             "scroll tall page")
    x("key", "Up")
    wait_for(lambda: screenshot().crop(band).tobytes() == before.tobytes(),
             "return to original scroll position")
    x("key", "ctrl+w")
    assert app.wait(timeout=5) == 0, "clean exit"
finally:
    if app.poll() is None:
        app.terminate()
        app.wait(timeout=5)
PY
