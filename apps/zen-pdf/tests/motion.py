#!/usr/bin/env python3
"""PDF motion, margin and optional-sidebar integration checks.

Runs in private Xvfb with software presentation. Never alters display settings.
Requires xdotool, xclip, Xvfb, dbus-daemon and python3-pil.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from PIL import ImageGrab

ROOT = Path(__file__).resolve().parents[1]
COLORS = [(227, 235, 215), (242, 224, 194), (217, 230, 235)]


def x(*args):
    return subprocess.check_output(["xdotool", *map(str, args)], text=True, timeout=8).strip()


def wait_for(predicate, label):
    until = time.monotonic() + 5
    while time.monotonic() < until:
        if predicate():
            print("PASS", label, flush=True)
            return
        time.sleep(.035)
    raise AssertionError(label)


def near(pixel, color):
    return sum(abs(a-b) for a,b in zip(pixel,color)) < 13


def run(directory, reduced=False, outline=False):
    doc = directory / ("outline-probe.pdf" if outline else "motion-probe.pdf")
    subprocess.run([sys.executable, ROOT / "tests/make-fixture.py", *(["--outline"] if outline else []), doc], check=True)
    config = directory / ("reduced" if reduced else "animated") / "gtk-4.0"
    config.mkdir(parents=True, exist_ok=True)
    (config / "settings.ini").write_text("[Settings]\ngtk-enable-animations=" + ("false" if reduced else "true") + "\n")
    env = dict(os.environ, XDG_CONFIG_HOME=str(config.parent), GDK_BACKEND="x11", GDK_SCALE="1",
               GTK_A11Y="none", GSETTINGS_BACKEND="memory", GIO_USE_VFS="local", GDK_DEBUG="no-portals",
               G_DEBUG="fatal-warnings")
    app = subprocess.Popen([os.environ.get("ZEN_PDF_BIN", str(ROOT / "zen-pdf")), doc], env=env)
    try:
        win = x("search", "--sync", "--onlyvisible", "--pid", app.pid, "--name", doc.name).splitlines()[0]
        x("windowsize", "--sync", win, 700, 600)
        x("windowmove", "--sync", win, 30, 30)
        x("windowfocus", win)
        origin = dict(line.split("=",1) for line in x("getwindowgeometry", "--shell", win).splitlines())
        ox, oy = int(origin["X"]), int(origin["Y"])
        assert int(origin["WIDTH"]) == 700 and int(origin["HEIGHT"]) == 600, origin
        x("mousemove", "--window", win, 660, 300)

        def shot():
            return ImageGrab.grab(bbox=(ox,oy,ox+700,oy+600), xdisplay=os.environ["DISPLAY"]).convert("RGB")

        def page(index):
            image = shot()
            return all(near(image.getpixel(at), COLORS[index]) for at in ((8,180),(692,180),(8,420),(692,420)))

        def settled(index, label):
            wait_for(lambda: page(index), label)
            time.sleep(.33)

        settled(0,"initial page, sidebar hidden")
        if outline:
            x("key","ctrl+b")
            wait_for(lambda: max(shot().getpixel((40,200))) < 90,"contents sidebar opens")
            # Local fixture has three small outline entries; let its asynchronous
            # metadata result replace the immediate page-number fallback.
            time.sleep(.5)
            for y, expected, label in [(146,1,"outline destination"),(190,2,"nested Unicode outline"),(234,0,"named outline destination")]:
                x("mousemove","--window",win,90,y,"click",1)
                wait_for(lambda: near(shot().getpixel((692,300)),COLORS[expected]),label)
                time.sleep(.33)
            x("key","ctrl+w")
            assert app.wait(timeout=6) == 0,"Viewer failed on outline close"
            return
        x("key","p")
        time.sleep(.2)
        x("key","Right")
        if not reduced:
            def split_frame():
                image=shot()
                return near(image.getpixel((8,300)),COLORS[0]) and near(image.getpixel((692,300)),COLORS[1])
            wait_for(split_frame,"horizontal page flip has an intermediate frame")
        settled(1,"right arrow and fit-page colored gutters")
        # A sheet shorter than the window must still advance with vertical wheel.
        x("click",5)
        settled(2,"vertical wheel advances a short page")
        before = shot().crop((220,140,480,460)).tobytes()
        x("click",5)
        if not reduced:
            wait_for(lambda: shot().crop((220,140,480,460)).tobytes() != before,"document end rubber band")
        else:
            time.sleep(.06)
            assert shot().crop((220,140,480,460)).tobytes() == before,"Reduced motion still bounced"
        time.sleep(.9)
        assert shot().crop((220,140,480,460)).tobytes() == before, "Rubber band did not settle"
        print("PASS document end settles without changing page",flush=True)
        x("click",4)
        settled(1,"reverse vertical wheel returns to preceding short page")
        # Width mode makes page 3 tall again; wheel must scroll before turning it.
        x("key","w","Right")
        settled(2,"tall page at fit width")
        before = shot().crop((220,140,480,460)).tobytes()
        x("click",5)
        wait_for(lambda: shot().crop((220,140,480,460)).tobytes() != before,"wheel scrolls inside a tall page")
        time.sleep(.5)
        x("click",4)
        time.sleep(.6)
        assert shot().crop((220,140,480,460)).tobytes() == before,"Scroll up failed to restore page position"
        x("click",4)
        settled(1,"wheel crosses tall-page top into preceding page")
        x("key","ctrl+b")
        wait_for(lambda: max(shot().getpixel((40,200))) < 90,"page sidebar opens only on request")
        time.sleep(.2)
        x("mousemove","--window",win,90,146,"click",1)
        wait_for(lambda: near(shot().getpixel((692,300)),COLORS[0]),"sidebar row navigates")
        time.sleep(.35)
        x("key","Escape")
        settled(0,"Escape hides sidebar")
        x("mousemove","--window",win,660,300)
        x("key","ctrl+w")
        assert app.wait(timeout=6) == 0,"Viewer failed on close"
        print("PASS reduced motion" if reduced else "PASS animated PDF UI",flush=True)
    finally:
        if app.poll() is None:
            app.terminate(); app.wait(timeout=5)


if __name__ == "__main__":
    if "--inside-display" not in sys.argv:
        raise SystemExit(subprocess.run(["xvfb-run","-a","-s","-screen 0 1280x1024x24","dbus-run-session","--",
            "env","GSK_RENDERER=cairo",sys.executable,str(Path(__file__).resolve()),"--inside-display"]).returncode)
    with tempfile.TemporaryDirectory() as temporary:
        directory=Path(temporary)
        run(directory)
        run(directory,reduced=True)
        run(directory,outline=True)
