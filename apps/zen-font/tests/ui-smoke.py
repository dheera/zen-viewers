#!/usr/bin/env python3
"""Exercise actual font rendering, editing, glyph copy and variable axes under X11."""
import os
from pathlib import Path
import subprocess
import struct
import sys
import tempfile
import time
import zlib


def xdo(*args):
    return subprocess.check_output(["xdotool", *map(str, args)], text=True, timeout=8).strip()


def wait(predicate, message):
    end = time.monotonic() + 8
    while time.monotonic() < end:
        try:
            value = predicate()
            if value:
                return value
        except subprocess.CalledProcessError:
            pass
        time.sleep(.05)
    raise AssertionError(message)


def key(*keys):
    xdo("key", "--clearmodifiers", *keys)
    time.sleep(.12)


def clipboard():
    return subprocess.check_output(["xclip", "-selection", "clipboard", "-o"], text=True, timeout=4)


def launch(binary, font, log):
    process = subprocess.Popen([binary, font], stdout=log, stderr=log)
    window = wait(lambda: xdo("search", "--onlyvisible", "--pid", process.pid), "Font window did not open").splitlines()[-1]
    wait(lambda: Path(font).name in xdo("getwindowname", window), "Font did not finish loading")
    xdo("windowfocus", window)
    xdo("mousemove", "--window", window, 950, 400)
    time.sleep(.5)
    return process, window


def close(process):
    key("ctrl+q")
    assert process.wait(timeout=5) == 0


def make_woff(source, destination):
    """Wrap an installed TTF for a webfont regression without another test dependency."""
    original = Path(source).read_bytes()
    count = struct.unpack_from(">H", original, 4)[0]
    payload = bytearray(44 + count * 20)
    sfnt_size = 12 + count * 16
    for index in range(count):
        tag, checksum, offset, length = struct.unpack_from(">4sIII", original, 12 + index * 16)
        table = original[offset:offset + length]
        compressed = zlib.compress(table)
        if len(compressed) >= length:
            compressed = table
        struct.pack_into(">4sIIII", payload, 44 + index * 20, tag, len(payload), len(compressed), length, checksum)
        payload.extend(compressed)
        payload.extend(b"\0" * (-len(payload) % 4))
        sfnt_size += (length + 3) & ~3
    struct.pack_into(">4s4sIHHIHHIIIII", payload, 0, b"wOFF", original[:4], len(payload),
                     count, 0, sfnt_size, 1, 0, 0, 0, 0, 0, 0)
    destination.write_bytes(payload)


def run(binary, variable=None):
    from PIL import ImageChops, ImageGrab, ImageStat
    font = "/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf"
    inspection = subprocess.check_output([binary, "--inspect", font], text=True)
    assert "DejaVu Serif" in inspection and "Unicode characters" in inspection
    with tempfile.TemporaryDirectory(prefix="zen-font-test-") as work:
        invalid = Path(work) / "invalid.ttf"
        invalid.write_text("This is not a font.")
        bad = subprocess.run([binary, "--inspect", str(invalid)], capture_output=True)
        assert bad.returncode == 1 and b"could not be read" in bad.stderr
        with open(Path(work) / "ui.log", "w+") as log:
            process, window = launch(binary, font, log)
            try:
                original_specimen = ImageGrab.grab().crop((70, 220, 850, 390))
                xdo("mousemove", "--window", window, 220, 280); xdo("click", 1)
                key("ctrl+a", "ctrl+c")
                assert clipboard() == "The quiet art\nof everyday things."
                xdo("type", "--clearmodifiers", "Sphinx of black quartz, judge my vow.")
                key("ctrl+a", "ctrl+c")
                assert clipboard() == "Sphinx of black quartz, judge my vow."
                key("ctrl+r", "ctrl+a", "ctrl+c")
                assert clipboard() == "The quiet art\nof everyday things."
                key("ctrl+g")
                xdo("mousemove", "--window", window, 835, 90); xdo("click", 1)
                xdo("type", "--clearmodifiers", "U+0041")
                time.sleep(.4)
                # Filtering leaves one item; the grid uses its minimum of two columns.
                xdo("mousemove", "--window", window, 120, 220); xdo("click", 1)
                wait(lambda: clipboard() == "A", "Glyph did not copy to clipboard")
                key("ctrl+g", "ctrl+plus", "ctrl+minus", "ctrl+i", "Escape")
                close(process)
            finally:
                if process.poll() is None:
                    process.terminate(); process.wait(timeout=5)
            webfont = Path(work) / "regression.woff"
            make_woff(font, webfont)
            process, window = launch(binary, str(webfont), log)
            try:
                unpacked_specimen = ImageGrab.grab().crop((70, 220, 850, 390))
                delta = sum(ImageStat.Stat(ImageChops.difference(original_specimen, unpacked_specimen)).mean)
                assert delta < .5, f"Webfont differs from its TTF source ({delta})"
                close(process)
            finally:
                if process.poll() is None:
                    process.terminate(); process.wait(timeout=5)
            if variable:
                process, window = launch(binary, variable, log)
                try:
                    key("ctrl+i", "Home", "Escape")
                    xdo("mousemove", "--window", window, 950, 400)
                    time.sleep(.3)
                    light = ImageGrab.grab().crop((70, 220, 850, 390))
                    key("ctrl+i", "End", "Escape")
                    xdo("mousemove", "--window", window, 950, 400)
                    time.sleep(.3)
                    heavy = ImageGrab.grab().crop((70, 220, 850, 390))
                    delta = sum(ImageStat.Stat(ImageChops.difference(light, heavy)).mean)
                    assert delta > .5, f"Variable axis did not change specimen rendering ({delta})"
                    close(process)
                finally:
                    if process.poll() is None:
                        process.terminate(); process.wait(timeout=5)
            log.flush(); log.seek(0); output = log.read()
            assert "CRITICAL" not in output and "Gtk-WARNING" not in output, output
    print("PASS font inspection, invalid-file rejection, editable specimen, reset, Unicode search/copy, size/details, WOFF rendering" +
          (", variable-axis rendering" if variable else ""))


if __name__ == "__main__":
    arguments = sys.argv[1:]
    display_ready = "--display-ready" in arguments
    arguments = [arg for arg in arguments if arg != "--display-ready"]
    binary = str(Path(arguments[0] if arguments else "build/zen-font").resolve())
    variable = str(Path(arguments[1]).resolve()) if len(arguments) > 1 else None
    os.environ.update(GDK_BACKEND="x11", GSK_RENDERER="cairo", GDK_DEBUG="no-portals",
                      GTK_A11Y="none", G_DEBUG="fatal-warnings", GIO_USE_VFS="local")
    if not display_ready:
        args = ["xvfb-run", "-a", "-s", "-screen 0 1280x1024x24", "dbus-run-session", "--",
                sys.executable, str(Path(__file__).resolve()), "--display-ready", binary]
        if variable:
            args.append(variable)
        raise SystemExit(subprocess.run(args).returncode)
    run(binary, variable)
