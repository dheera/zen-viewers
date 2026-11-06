#!/usr/bin/env python3
"""Verify displayed pixels under a real GTK window. Pass the zen-image executable."""
import ctypes
import hashlib
import os
from pathlib import Path
import shlex
import struct
import subprocess
import sys
import tempfile
import time
from PIL import Image, ImageDraw, ImageGrab, PngImagePlugin, TiffImagePlugin


def x(*args):
    return subprocess.check_output(["xdotool", *map(str, args)], timeout=8, text=True).strip()


def wait(check, label):
    end = time.monotonic() + 5
    while time.monotonic() < end:
        if check():
            return
        time.sleep(.04)
    raise AssertionError(label)


class OpenWatch:
    """Observe actual Linux file opens; cache assertions have no speed threshold."""
    def __init__(self, paths):
        libc = ctypes.CDLL(None, use_errno=True)
        libc.inotify_init1.argtypes = [ctypes.c_int]
        libc.inotify_add_watch.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_uint32]
        self.fd = libc.inotify_init1(os.O_NONBLOCK | os.O_CLOEXEC)
        if self.fd < 0:
            raise OSError(ctypes.get_errno(), "inotify_init1")
        self.paths = {}
        for path in paths:
            descriptor = libc.inotify_add_watch(self.fd, os.fsencode(path), 0x20)  # IN_OPEN
            if descriptor < 0:
                os.close(self.fd)
                raise OSError(ctypes.get_errno(), f"inotify_add_watch({path})")
            self.paths[descriptor] = path.name

    def drain(self):
        opened = []
        while True:
            try:
                events = os.read(self.fd, 65536)
            except BlockingIOError:
                return opened
            offset = 0
            while offset < len(events):
                descriptor, mask, _cookie, length = struct.unpack_from("iIII", events, offset)
                offset += 16 + length
                assert not mask & 0x4000, "inotify queue overflowed"  # IN_Q_OVERFLOW
                if mask & 0x20:
                    opened.append(self.paths[descriptor])

    def close(self):
        os.close(self.fd)


def decode_gate(tmp):
    """Block one regular source file, leaving its thumbnail and UI unrestricted.

    This shim is loaded only into the test process. It does not change the app
    or the fixture's file type, and makes preview ordering independent of image
    size, disk speed, and decoder performance.
    """
    source = tmp / "read-gate.c"
    library = tmp / "read-gate.so"
    source.write_text(r'''
#define _GNU_SOURCE
#include <gio/gio.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
GFileInputStream *g_file_read(GFile *file, GCancellable *cancel, GError **error) {
    typedef GFileInputStream *(*Read)(GFile *, GCancellable *, GError **);
    Read next = (Read)dlsym(RTLD_NEXT, "g_file_read");
    const char *target = getenv("ZEN_IMAGE_TEST_SOURCE");
    const char *gate = getenv("ZEN_IMAGE_TEST_GATE");
    const char *entered = getenv("ZEN_IMAGE_TEST_ENTERED");
    char *path = g_file_get_path(file);
    if (target && gate && entered && path && !strcmp(path, target) && !access(gate, F_OK)) {
        int fd = open(entered, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) close(fd);
        while (!access(gate, F_OK)) g_usleep(1000);
    }
    g_free(path);
    return next(file, cancel, error);
}
''')
    flags = subprocess.check_output(["pkg-config", "--cflags", "--libs", "gio-2.0"], text=True).split()
    subprocess.run([*shlex.split(os.environ.get("CC", "cc")), "-shared", "-fPIC", "-std=c11", "-O2",
                    "-Wall", "-Wextra", source, "-o", library, *flags, "-ldl"], check=True)
    return library


def make_dng(path):
    # A tiny uncompressed RGGB sensor image with no embedded JPEG preview.
    width, height = 192, 128
    ifd = TiffImagePlugin.ImageFileDirectory_v2()
    tags = {
        262: (3, 32803),                       # CFA photometric interpretation
        271: (2, "Zen"), 272: (2, "Test DNG"),   # camera make and model
        33421: (3, (2, 2)), 33422: (1, b"\0\1\1\2"),  # CFA layout
        50706: (1, b"\1\4\0\0"), 50707: (1, b"\1\1\0\0"),  # DNG versions
        50708: (2, "Zen Test Camera"),
        50714: (3, 0), 50717: (4, 4095),         # black and white levels
        50721: (10, (1, 0, 0, 0, 1, 0, 0, 0, 1)),  # color matrix
        50728: (5, (1, 1, 1)), 50778: (3, 21),   # neutral and illuminant
    }
    for tag, (kind, value) in tags.items():
        ifd.tagtype[tag] = kind
        ifd[tag] = value
    samples = [200 + x * 1300 // width + y * 500 // height for y in range(height) for x in range(width)]
    data = struct.pack(f"<{len(samples)}H", *samples)
    Image.frombytes("I;16", (width, height), data).save(path, format="TIFF", tiffinfo=ifd)


def run(binary, tmp):
    w, h = 513, 257  # RGB stride includes padding.
    red, green, blue, yellow = (185, 49, 45), (56, 150, 78), (42, 75, 172), (232, 196, 63)
    src = Image.new("RGB", (w, h), red)
    draw = ImageDraw.Draw(src)
    draw.rectangle((w//2, 0, w-1, h//2-1), fill=green)
    draw.rectangle((0, h//2, w//2-1, h-1), fill=blue)
    draw.rectangle((w//2, h//2, w-1, h-1), fill=yellow)
    png, webp, jpg, broken = [tmp / s for s in ["odd.png", "odd.webp", "oriented.jpg", "broken.png"]]
    src.save(png)
    src.save(webp, lossless=True)
    exif = Image.Exif()
    exif[274] = 6
    exif[271] = "Zen Camera"
    exif[272] = "Navigation Test"
    exif[306] = "2026:11:06 12:00:00"
    src.save(jpg, quality=95, exif=exif)
    broken.write_bytes(b"this is not an image")
    gate, entered = tmp / "decode-gate", tmp / "decode-entered"
    preview_source = tmp / "thumbnail" / "source.png"
    cache = tmp / "cache"
    environment = dict(os.environ, XDG_CACHE_HOME=str(cache),
                       LD_PRELOAD=str(decode_gate(tmp)),
                       ZEN_IMAGE_TEST_SOURCE=str(preview_source),
                       ZEN_IMAGE_TEST_GATE=str(gate), ZEN_IMAGE_TEST_ENTERED=str(entered))
    app = subprocess.Popen([binary, png], env=environment)
    try:
        win = x("search", "--sync", "--onlyvisible", "--name", "odd.png").splitlines()[0]
        x("windowfocus", win)
        x("windowsize", "--sync", win, 600, 500)
        x("mousemove", 1100, 900)
        geo = dict(s.split("=") for s in x("getwindowgeometry", "--shell", win).splitlines())
        ox, oy = int(geo["X"]), int(geo["Y"])

        def color(px, py, expected):
            rgb = ImageGrab.grab(bbox=(ox+px, oy+py, ox+px+1, oy+py+1),
                                 xdisplay=os.environ["DISPLAY"]).convert("RGB").getpixel((0, 0))
            return sum(abs(a-b) for a,b in zip(rgb,expected)) < 22

        def load(path):
            x("key", "ctrl+o")
            chooser = x("search", "--sync", "--onlyvisible", "--name", "^Open").splitlines()[-1]
            x("windowfocus", chooser)
            subprocess.run(["xclip", "-selection", "clipboard", "-i", "-target", "UTF8_STRING"],
                           input=os.fsencode(path), check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            x("key", "ctrl+l", "ctrl+a", "ctrl+v")
            time.sleep(.2)
            x("key", "Return")
            wait(lambda: chooser not in subprocess.run(["xdotool", "search", "--onlyvisible", "--name", "^Open"],
                                                       capture_output=True, text=True).stdout.splitlines(),
                 "File chooser did not close")
            x("windowfocus", win)

        ambient = (22,24,24)
        wait(lambda: color(5,170,red) and color(5,280,blue) and color(5,75,ambient), "PNG fit and margins")
        x("key", "1")
        wait(lambda: color(5,170,ambient) and color(55,170,red), "Actual physical pixel sizing")
        x("key", "p", "r")
        wait(lambda: color(200,130,blue) and color(390,130,red) and color(100,130,ambient), "Clockwise rotation")
        x("key", "w")
        wait(lambda: color(100,130,blue), "Fit-width rotated image")
        x("key", "p", "r", "r", "r")
        wait(lambda: color(5,170,red), "Four turns restore layout")
        x("key", "plus", "plus", "plus")
        wait(lambda: color(350,200,green), "Zoom in")
        x("mousemove", "--window", win, 400, 250, "mousedown", 1, "sleep", ".1",
          "mousemove", "--window", win, 520, 250, "sleep", ".1", "mouseup", 1)
        wait(lambda: color(350,200,red), "Mouse pan moves center boundary")
        x("key", "p")
        x("mousemove", "--window", win, 120, 130, "keydown", "Control_L", "click", 4, "keyup", "Control_L")
        wait(lambda: color(310,180,red), "Ctrl+wheel zooms around the pointer")
        print("PASS PNG, fit, 1:1 pixels, rotation, wheel zoom and pan", flush=True)

        load(broken)
        wait(lambda: png.name in x("getwindowname",win) and color(160,175,red), "Error should keep the current image")
        load(webp)
        wait(lambda: webp.name in x("getwindowname",win) and color(120,175,red), "Load WebP after failed decode")
        load(jpg)
        wait(lambda: jpg.name in x("getwindowname",win) and color(200,130,blue) and color(390,130,red),
             "JPEG EXIF orientation")
        print("PASS chooser error recovery, WebP and JPEG EXIF orientation", flush=True)

        # Metadata is opt-in and copyable. Starting from the rotated JPEG catches
        # the distinction between source orientation and displayed dimensions.
        x("key", "i", "ctrl+a", "ctrl+c")
        def metadata():
            return subprocess.check_output(["xclip", "-selection", "clipboard", "-o", "-target", "UTF8_STRING"],
                                           timeout=3, text=True)
        info = metadata()
        assert "oriented.jpg" in info and "257 × 513 pixels" in info and "File size" in info, info
        if os.environ.get("EXPECT_EXIF") == "1":
            assert "Zen Camera" in info and "Navigation Test" in info, info
        x("key", "i")
        wait(lambda: color(390,130,red), "Metadata toggle hides panel")
        print("PASS hidden-by-default metadata, original dimensions and native clipboard", flush=True)

        album = tmp / "album"
        album.mkdir()
        for name, paint in [("image1.PNG", red), ("image2.jpg", green), ("image4.bmp", blue), ("image10.tiff", yellow)]:
            Image.new("RGB", (300,200), paint).save(album / name)
        (album / "image3.png").write_bytes(b"unreadable")
        (album / "notes.txt").write_text("Not an image")
        (album / "image9.png").mkdir()
        load(album / "image1.PNG")
        def named(name):
            return name in x("getwindowname", win)
        wait(lambda: named("image1.PNG") and color(300,200,red), "Open an album")
        x("key", "Right")
        wait(lambda: named("image2.jpg") and color(300,200,green), "Natural order image1 -> image2")
        x("key", "greater")
        wait(lambda: named("image4.bmp") and color(300,200,blue), "Skip corrupt image3 and open BMP")
        x("key", "Right")
        wait(lambda: named("image10.tiff") and color(300,200,yellow), "Natural order image4 -> image10 and TIFF")
        x("key", "Right")
        time.sleep(.12)
        assert named("image10.tiff"), "Next should stop at last image"
        x("key", "less")
        wait(lambda: named("image4.bmp"), "Less-than goes back")
        x("key", "Left", "Left")
        wait(lambda: named("image1.PNG") and color(300,200,red), "Rapid navigation ignores obsolete results")
        x("key", "Left")
        time.sleep(.12)
        assert named("image1.PNG"), "Previous should stop at first image"
        print("PASS natural folder order, < > and arrow navigation, corrupt file skipping, mixed formats and boundaries", flush=True)

        # At minimum width all seven toolbar actions remain reachable. These
        # points are within the compact buttons across the supported GTK themes.
        x("windowsize", "--sync", win, 340, 500)
        x("mousemove", "--window", win, 100, 472)
        time.sleep(.25)
        x("click", 1)
        wait(lambda: named("image2.jpg"), "Compact next button")
        x("mousemove", "--window", win, 30, 472, "click", 1)
        wait(lambda: named("image1.PNG"), "Compact previous button")
        x("mousemove", "--window", win, 310, 472, "click", 1)
        x("key", "ctrl+a", "ctrl+c")
        assert "image1.PNG" in metadata(), "Compact metadata button"
        x("key", "Escape")
        x("windowsize", "--sync", win, 600, 500)
        x("mousemove", 1100, 900)
        wait(lambda: color(390,200,red), "Escape closes metadata")
        print("PASS compact navigation and metadata buttons, Escape dismissal", flush=True)

        # Additional loaders use the same decoding path; still images are
        # sufficient here because animation is intentionally not implemented.
        extra = tmp / "formats"
        extra.mkdir()
        for extension in ("gif", "ico", "ppm"):
            image = extra / f"still.{extension}"
            # DIB-backed ICO works with both legacy GdkPixbuf and newer loaders.
            # Some older loaders cannot read PNG-compressed icon entries.
            options = {"bitmap_format": "bmp", "sizes": [(64,64)]} if extension == "ico" else {}
            Image.new("RGBA" if extension == "ico" else "RGB", (64,64), blue).save(image, **options)
            load(image)
            wait(lambda: named(image.name) and color(300,200,blue), f"Decode {extension}")
        installed = subprocess.check_output([binary, "--formats"], text=True).lower()
        if "svg" in installed:
            image = extra / "vector.svg"
            image.write_text('<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64"><rect width="64" height="64" fill="#2a4bac"/></svg>')
            load(image)
            wait(lambda: named(image.name) and color(300,200,blue), "Decode SVG")
        if os.environ.get("ZEN_TEST_EXTRA_FORMATS") == "1":
            source = extra / "source.png"
            Image.new("RGB", (64,64), blue).save(source)
            for extension, encoder in (("avif", "libaom-av1"), ("jxl", "libjxl")):
                assert extension in installed, f"Install a {extension} GdkPixbuf loader first"
                image = extra / f"still.{extension}"
                subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-i", source,
                                "-frames:v", "1", "-c:v", encoder, "-threads", "1", image], check=True, timeout=30)
                load(image)
                wait(lambda: named(image.name) and color(300,200,blue), f"Decode {extension}")
        print("PASS additional installed image loaders", flush=True)

        if "camera raw previews" in installed:
            raw = tmp / "raw"
            raw.mkdir()
            start = raw / "shot1.png"
            Image.new("RGB", (300,200), red).save(start)
            make_dng(raw / "shot2.DNG")
            (raw / "shot3.ARW").write_bytes(b"broken RAW data")
            Image.new("RGB", (300,200), blue).save(raw / "shot4.png")
            load(start)
            wait(lambda: named(start.name) and color(300,200,red), "Open mixed RAW folder")
            x("key", "Right")
            wait(lambda: named("shot2.DNG") and not color(300,200,ambient) and not color(300,200,red),
                 "Render a RAW image without an embedded preview")
            x("key", "i", "ctrl+a", "ctrl+c")
            raw_info = metadata()
            assert all(field in raw_info for field in ("Camera RAW (DNG)", "Quick RAW preview",
                       "96 × 64 pixels", "192 × 128 pixels", "Test DNG")), raw_info
            x("key", "Escape", "Right")
            wait(lambda: named("shot4.png") and color(300,200,blue), "Skip damaged RAW file")
            x("key", "Left")
            wait(lambda: named("shot2.DNG"), "Navigate back to the decoded RAW image")
            print("PASS quick DNG preview, RAW metadata and mixed-folder navigation", flush=True)

        # Observe real opens so this verifies reuse, not a machine-speed budget.
        hot = tmp / "hot-cache"
        hot.mkdir()
        first, second = hot / "hot1.png", hot / "hot2.png"
        Image.new("RGB", (300,200), red).save(first)
        Image.new("RGB", (300,200), green).save(second)
        watch = OpenWatch([first, second])
        try:
            load(first)
            wait(lambda: named(first.name) and color(300,200,red), "First cache fixture")
            x("key", "Right")
            wait(lambda: named(second.name) and color(300,200,green), "Warm next image")
            x("key", "Left")
            wait(lambda: named(first.name) and color(300,200,red), "Warm previous image")
            observed = watch.drain()
            assert first.name in observed and second.name in observed, observed
            for _ in range(4):
                x("key", "Right")
                wait(lambda: named(second.name) and color(300,200,green), "Cached next pixels")
                x("key", "Left")
                wait(lambda: named(first.name) and color(300,200,red), "Cached previous pixels")
            reopened = watch.drain()
            assert not reopened, f"Hot navigation reopened source images: {reopened}"
        finally:
            watch.close()
        print("PASS warm neighbor navigation reuses decoded images without reopening files", flush=True)

        replacement = hot / "replacement.tmp"
        Image.new("RGB", (300,200), blue).save(replacement, format="PNG")
        replacement.replace(second)

        def see_replacement():
            x("key", "Right")
            wait(lambda: named(second.name) and (color(300,200,green) or color(300,200,blue)),
                 "Navigate to the replaced image")
            if color(300,200,blue):
                return True
            x("key", "Left")
            wait(lambda: named(first.name) and color(300,200,red), "Return while awaiting cache invalidation")
            return False

        wait(see_replacement, "A changed neighbor must not keep showing its cached pixels")
        print("PASS replacing a cached image invalidates it", flush=True)

        rapid = tmp / "rapid-reversals"
        rapid.mkdir()
        colors = [red, green, blue, yellow]
        for i, paint in enumerate(colors):
            Image.new("RGB", (300,200), paint).save(rapid / f"rapid{i}.png")
        load(rapid / "rapid1.png")
        wait(lambda: named("rapid1.png") and color(300,200,green), "Open reversal fixture")
        x("key", "Right")
        wait(lambda: named("rapid2.png") and color(300,200,blue), "Reversal folder ready")
        x("key", "Left")
        wait(lambda: named("rapid1.png") and color(300,200,green), "Reversal starting position")
        for _ in range(4):
            x("key", "--delay", 0, "Right", "Left", "Right", "Right", "Left")
            wait(lambda: named("rapid2.png") and color(300,200,blue), "Latest forward reversal owns pixels and title")
            x("key", "--delay", 0, "Left", "Right", "Left", "Left", "Right")
            wait(lambda: named("rapid1.png") and color(300,200,green), "Latest backward reversal owns pixels and title")
        print("PASS rapid direction reversals keep final pixels and title in agreement", flush=True)

        # A distinct thumbnail color makes the intermediate state observable.
        # Hold the full source read until the preview is actually on screen;
        # neither fixture size nor a lucky scheduling delay can make this pass.
        preview_source.parent.mkdir()
        Image.new("RGB", (600,400), yellow).save(preview_source)
        thumbnail_color = (55,165,180)
        uri = preview_source.as_uri()
        thumbnail = cache / "thumbnails" / "large" / (hashlib.md5(uri.encode(), usedforsecurity=False).hexdigest() + ".png")
        thumbnail.parent.mkdir(parents=True)
        fields = PngImagePlugin.PngInfo()
        fields.add_text("Thumb::URI", uri)
        fields.add_text("Thumb::MTime", str(int(preview_source.stat().st_mtime)))
        fields.add_text("Thumb::Size", str(preview_source.stat().st_size))
        Image.new("RGB", (96,64), thumbnail_color).save(thumbnail, pnginfo=fields)
        gate.write_text("hold source decode until the preview has been observed")
        load(preview_source)
        wait(entered.exists, "Full image decode reached the controlled read gate")
        wait(lambda: color(300,200,thumbnail_color), "Cached thumbnail appears while full decode is blocked")
        assert named("rapid1.png"), "A thumbnail must not commit the full image title"
        gate.unlink()
        wait(lambda: named(preview_source.name) and color(300,200,yellow),
             "Full-resolution image replaces its cached thumbnail")
        print("PASS cached thumbnail precedes full decode and full pixels replace the preview", flush=True)
        x("key", "ctrl+w")
        assert app.wait(timeout=5) == 0
    finally:
        gate.unlink(missing_ok=True)
        if app.poll() is None:
            app.terminate()
            app.wait(timeout=4)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit("Usage: ui-smoke.py /path/to/zen-image")
    binary = Path(sys.argv[1]).resolve(strict=True)
    if "--display-ready" in sys.argv:
        os.environ.update(GDK_DEBUG="no-portals", GTK_A11Y="none", GDK_BACKEND="x11", GSK_RENDERER="cairo",
                          GSETTINGS_BACKEND="memory", G_DEBUG="fatal-warnings", GIO_USE_VFS="local")
        with tempfile.TemporaryDirectory() as temp:
            run(binary, Path(temp))
    else:
        raise SystemExit(subprocess.run(["xvfb-run", "-a", "-s", "-screen 0 1280x1024x24", "dbus-run-session", "--",
                                         sys.executable, __file__, binary, "--display-ready"]).returncode)
