#!/usr/bin/env python3
"""Exercise navigation and the HTTP cache against localhost only, under Xvfb.

Usage: xvfb-run -a dbus-run-session -- python3 apps/zen-map/tests/ui-smoke.py build/zen-map
Requires Python Pillow and xdotool. Never contacts a public tile provider.
"""
import configparser
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from PIL import Image, ImageDraw, ImageGrab


def x(*args):
    return subprocess.check_output(["xdotool", *map(str, args)], text=True, timeout=8).strip()


def wait(predicate, message, timeout=8):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(.04)
    raise AssertionError(message)


image = Image.new("RGB", (256, 256), "#d9e1ce")
draw = ImageDraw.Draw(image)
draw.line([(0, 190), (100, 140), (200, 80), (256, 0)], fill="#f8f6e9", width=9)
draw.line([(0, 40), (256, 210)], fill="#a8c4c8", width=14)
stream = io.BytesIO()
image.save(stream, format="PNG")
tile = stream.getvalue()
requests = []
lock = threading.Lock()
slow = threading.Event()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        conditional = self.headers.get("If-None-Match") == '"zen-test-tile"'
        with lock:
            requests.append((self.path, dict(self.headers), conditional))
        if slow.is_set():
            time.sleep(2)
        self.send_response(304 if conditional else 200)
        self.send_header("Content-Type", "image/png")
        self.send_header("Cache-Control", "public, max-age=86400")
        self.send_header("ETag", '"zen-test-tile"')
        self.send_header("Last-Modified", "Thu, 05 Nov 2026 00:00:00 GMT")
        self.send_header("Content-Length", "0" if conditional else str(len(tile)))
        self.end_headers()
        if not conditional:
            try:
                self.wfile.write(tile)
            except (BrokenPipeError, ConnectionResetError):
                pass


def count():
    with lock:
        return len(requests)


server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()
binary = str(Path(sys.argv[1] if len(sys.argv) > 1 else "build/zen-map").resolve())
processes = []
with tempfile.TemporaryDirectory(prefix="zen-map-test-") as directory:
    tmp = Path(directory)
    sample = tmp / "test-track.geojson"
    sample.write_text(json.dumps({"type": "FeatureCollection", "features": [
        {"type": "Feature", "properties": {}, "geometry": {"type": "LineString", "coordinates": [[-122.63, 37.88], [-122.61, 37.9], [-122.59, 37.89]]}},
        {"type": "Feature", "properties": {"name": "Lookout"}, "geometry": {"type": "Point", "coordinates": [-122.61, 37.9]}}
    ]}))
    env = dict(os.environ, GDK_BACKEND="x11", GDK_DEBUG="no-portals", GTK_A11Y="none", GSK_RENDERER="cairo", GSETTINGS_BACKEND="memory", G_DEBUG="fatal-warnings", GIO_USE_VFS="local",
               XDG_CACHE_HOME=str(tmp / "cache"), ZEN_MAP_TILE_URL=f"http://127.0.0.1:{server.server_port}/{{z}}/{{x}}/{{y}}.png",
               ZEN_MAP_TILE_ATTRIBUTION="Local test tiles", ZEN_MAP_TILE_CREDIT_URL="https://example.org/credits")

    def launch():
        log = open(tmp / f"app-{len(processes)}.log", "w+")
        p = subprocess.Popen([binary, str(sample)], env=env, stdout=log, stderr=log)
        processes.append((p, log))
        found = []

        def find():
            try:
                found[:] = x("search", "--onlyvisible", "--pid", p.pid).splitlines()
                return bool(found)
            except subprocess.CalledProcessError:
                return False

        wait(find, "map window did not open")
        window = found[-1]
        x("windowfocus", window)
        time.sleep(.45)
        assert p.poll() is None, "map exited during file load"
        return p, window

    def close(p, window):
        x("windowfocus", window)
        x("key", "ctrl+w")
        p.wait(timeout=3)
        assert p.returncode == 0, f"map exited with {p.returncode}"

    try:
        p, window = launch()
        assert count() == 0, "opening a local map made network requests"
        before = ImageGrab.grab().tobytes()
        x("key", "--window", window, "Right")
        time.sleep(.2)
        assert ImageGrab.grab().tobytes() != before, "pan did not change displayed pixels"
        x("key", "--window", window, "f")
        x("key", "--window", window, "m")
        wait(lambda: count() > 0, "Map toggle did not load visible tiles")
        wait(lambda: len(list((tmp / "cache").rglob("*.png"))) == count() and count() >= 4, "tiles were not cached")
        time.sleep(.6)
        first_count = count()
        assert first_count <= 30, "requested more than a single viewport"
        with lock:
            for path, headers, conditional in requests:
                assert headers.get("User-Agent", "").startswith("zen-map/")
                assert "no-cache" not in headers.get("Cache-Control", "")
                assert not conditional
        if os.environ.get("ZEN_MAP_TEST_SCREENSHOT"):
            ImageGrab.grab().save(os.environ["ZEN_MAP_TEST_SCREENSHOT"])
        close(p, window)

        p, window = launch()
        assert count() == first_count, "reopening a file enabled networking"
        x("key", "--window", window, "m")
        time.sleep(.8)
        assert count() == first_count, "fresh disk cache did not prevent HTTP requests"
        close(p, window)

        for ini in (tmp / "cache").rglob("*.ini"):
            config = configparser.ConfigParser()
            config.read(ini)
            config["tile"]["expires"] = "1"
            with ini.open("w") as f:
                config.write(f, space_around_delimiters=False)
        p, window = launch()
        x("key", "--window", window, "m")
        wait(lambda: count() >= first_count * 2, "expired cache was not revalidated")
        time.sleep(.4)
        with lock:
            assert all(r[2] for r in requests[first_count:]), "expired cache did not use ETag validators"
        revalidated = count()
        x("key", "--window", window, "m")
        x("key", "--window", window, "plus")
        time.sleep(.4)
        assert count() == revalidated, "disabled basemap made requests during navigation"
        slow.set()
        x("key", "--window", window, "m")
        wait(lambda: count() > revalidated, "new viewport did not request tiles")
        close(p, window)
        for proc, log in processes:
            log.seek(0)
            output = log.read()
            assert "CRITICAL" not in output and "WARNING" not in output, output
        print("PASS: offline opening, visible pan, opt-in tiles, UA, persistent cache, ETag revalidation, disabled networking, close during fetch")
    finally:
        for proc, log in processes:
            if proc.poll() is None:
                proc.terminate()
                proc.wait(timeout=3)
            log.close()
        server.shutdown()
