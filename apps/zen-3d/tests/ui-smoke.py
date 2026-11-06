#!/usr/bin/env python3
"""Exercise the actual GTK/OpenGL viewport in an isolated X11 session."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

from PIL import ImageChops, ImageGrab


ROOT = Path(__file__).resolve().parents[3]
WIDTH, HEIGHT = 760, 560


def x(*args):
    return subprocess.check_output(["xdotool", *map(str, args)], text=True, timeout=10).strip()


def wait(check, message):
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        if check():
            return
        time.sleep(.06)
    raise AssertionError(message)


def difference(before, after):
    histogram = ImageChops.difference(before, after).convert("L").histogram()
    return sum(i * count for i, count in enumerate(histogram)) / (before.width * before.height)


def run(binary, folder):
    sample = ROOT / "examples" / "trefoil.glb"
    plane = folder / "plate.obj"
    plane.write_text("v -1 -1 0\nv 1 -1 0\nv 1 1 0\nv -1 1 0\nf 1 2 3 4\n")
    broken = folder / "broken.obj"
    broken.write_text("This is not a model.")
    x("mousemove", 1180, 900)
    app = subprocess.Popen([binary, sample])
    try:
        win = x("search", "--sync", "--onlyvisible", "--pid", app.pid,
                "--name", r"trefoil\.glb").splitlines()[0]
        x("windowmove", "--sync", win, 45, 55)
        x("windowsize", "--sync", win, WIDTH, HEIGHT)
        x("windowfocus", win)
        geometry = dict(line.split("=", 1) for line in x("getwindowgeometry", "--shell", win).splitlines())
        ox, oy = int(geometry["X"]), int(geometry["Y"])

        def shot():
            return ImageGrab.grab(bbox=(ox, oy, ox + WIDTH, oy + HEIGHT),
                                  xdisplay=os.environ["DISPLAY"]).convert("RGB")

        def scene():
            return shot().crop((128, 105, 630, 452))

        def model_visible():
            pixels = scene().resize((80, 56)).tobytes()
            return any(r < 110 and g > r + 10 and b > r + 10
                       for r, g, b in zip(pixels[0::3], pixels[1::3], pixels[2::3]))

        def drag(button, dx, dy):
            x("mousemove", "--window", win, 340, 290, "mousedown", button, "sleep", ".09",
              "mousemove", "--window", win, 340 + dx, 290 + dy, "sleep", ".09", "mouseup", button)

        def open_file(path):
            x("key", "ctrl+o")
            dialog = x("search", "--sync", "--onlyvisible", "--name", "^Open").splitlines()[-1]
            x("windowfocus", dialog)
            subprocess.run(["xclip", "-selection", "clipboard", "-i", "-target", "UTF8_STRING"],
                           input=os.fsencode(path), check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            x("key", "ctrl+l", "ctrl+a", "ctrl+v")
            time.sleep(.2)
            x("key", "Return")
            wait(lambda: dialog not in subprocess.run(["xdotool", "search", "--onlyvisible", "--name", "^Open"],
                                                       capture_output=True, text=True).stdout.splitlines(),
                 "File chooser did not close")
            x("windowfocus", win)

        wait(model_visible, "colored GLB model rendered")
        # A mapped X11 size can precede GTK's resized GL texture by a frame.
        time.sleep(.4)
        center = lambda: shot().crop((150, 130, 610, 420))
        quiet_model = center()
        # Opening a file briefly reveals chrome. Let that first reveal expire,
        # then force new widget snapshots without changing the model/camera.
        time.sleep(2.1)
        assert difference(quiet_model, center()) < 1.5, "quiet GL frame lost the model"
        x("key", "Tab")
        x("mousemove", "--window", win, WIDTH // 2, HEIGHT - 24)
        for _ in range(8):
            time.sleep(.05)
            assert difference(quiet_model, center()) < 1.5, "chrome snapshot replaced the GL scene"
        # Return keyboard focus to the canvas so chrome is allowed to fade.
        x("mousemove", "--window", win, WIDTH // 2, HEIGHT // 2, "click", 1)
        x("mousemove", 1180, 900)
        time.sleep(2.1)
        assert difference(quiet_model, center()) < 1.5, "chrome fade replaced the GL scene"
        print("PASS stable GL scene through Tab, bottom hover and chrome fade", flush=True)
        initial = scene()
        drag(1, 138, -60)
        wait(lambda: difference(initial, scene()) > 6, "primary drag orbits")
        x("key", "r")
        wait(lambda: difference(initial, scene()) < 1.5, "R restores the default camera")
        drag(2, 97, 22)
        wait(lambda: difference(initial, scene()) > 5, "middle drag pans")
        x("key", "f")
        wait(lambda: difference(initial, scene()) < 1.5, "F frames the model")
        x("mousemove", "--window", win, 350, 274, "click", "--repeat", 3, 4)
        wait(lambda: difference(initial, scene()) > 6, "scroll wheel zooms")
        x("key", "f", "3", "o")
        wait(lambda: difference(initial, scene()) > 6, "top and orthographic views render")
        print("PASS OpenGL model, orbit, pan, wheel zoom, frame and view presets", flush=True)

        x("key", "r")
        wait(lambda: difference(initial, scene()) < 1.5, "reset perspective")
        x("key", "w")
        wait(lambda: difference(initial, scene()) > .6, "wireframe changes the rendered model")
        x("key", "w", "g")
        wait(lambda: difference(initial, scene()) > .15, "grid can be hidden")
        x("key", "g", "i")
        wait(lambda: max(shot().getpixel((680, 194))) < 90, "details panel opens")
        x("key", "Escape")
        wait(lambda: min(shot().getpixel((680, 194))) > 140, "Escape hides model details")
        print("PASS optional details, grid and wireframe", flush=True)

        open_file(broken)
        wait(lambda: "trefoil.glb" in x("getwindowname", win) and model_visible(),
             "failed import keeps the current model")
        open_file(plane)
        wait(lambda: "plate.obj" in x("getwindowname", win), "valid OBJ replaces the current model")
        assert difference(initial, scene()) > 2, "OBJ did not render a different scene"
        x("key", "ctrl+w")
        assert app.wait(timeout=6) == 0, "Viewer failed on close"
        print("PASS file chooser, decode error recovery and model replacement", flush=True)
    finally:
        if app.poll() is None:
            app.terminate()
            app.wait(timeout=5)


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--inside-display":
        with tempfile.TemporaryDirectory(prefix="zen-3d-ui-") as temp:
            run(sys.argv[2], Path(temp))
    else:
        app = Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / "build/zen-3d").resolve()
        subprocess.run(["xvfb-run", "-a", "-s", "-screen 0 1280x1024x24 +extension GLX", "dbus-run-session",
                        "--", "env", "GTK_A11Y=none", "GDK_BACKEND=x11", "GSK_RENDERER=gl",
                        "GSETTINGS_BACKEND=memory", "G_DEBUG=fatal-warnings", "GDK_DEBUG=no-portals",
                        sys.executable, str(Path(__file__).resolve()), "--inside-display", str(app)], check=True)
