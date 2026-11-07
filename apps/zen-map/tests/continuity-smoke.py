#!/usr/bin/env python3
"""Deterministic zoom/pan continuity checks with gated localhost tiles only.

Run under Xvfb, passing the zen-map executable. Each tile has uniquely colored
quadrants, so assertions check geographical crop/scale transforms, not merely
that an old screenshot stayed on screen. Network gates hold new detail until
the fallback pixels have been examined. Requires Pillow and xdotool.
"""
import io
import math
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
        time.sleep(.025)
    raise AssertionError(message)


def color(z, x, y, qx, qy):
    return (40 + (31 * z + 17 * x + 37 * qx) % 160,
            40 + (19 * z + 23 * y + 29 * qy) % 160,
            40 + (13 * z + 11 * x + 7 * y + 17 * qx + 31 * qy) % 160)


def tile(z, x, y):
    im = Image.new("RGB", (256, 256))
    draw = ImageDraw.Draw(im)
    for qy in range(2):
        for qx in range(2):
            draw.rectangle((qx * 128, qy * 128, qx * 128 + 127, qy * 128 + 127), fill=color(z, x, y, qx, qy))
    out = io.BytesIO()
    im.save(out, format="PNG")
    return out.getvalue()


gate = threading.Event()
gate.set()
lock = threading.Lock()
requests = []
finished = set()
inflight = set()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        key = tuple(map(int, self.path.removesuffix(".png").strip("/").split("/")))
        with lock:
            requests.append((key, frozenset(inflight), frozenset(finished)))
            inflight.add(key)
        gate.wait(timeout=10)
        body = tile(*key)
        self.send_response(200)
        self.send_header("Content-Type", "image/png")
        self.send_header("Cache-Control", "public, max-age=86400")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            with lock:
                inflight.discard(key)
                finished.add(key)


def visible(z, cx, cy, width, height):
    world = 256 * 2**z
    first_x, last_x = math.floor((cx * world - width / 2) / 256), math.floor((cx * world + width / 2) / 256)
    first_y, last_y = max(0, math.floor((cy * world - height / 2) / 256)), min(2**z - 1, math.floor((cy * world + height / 2) / 256))
    return {(z, x % 2**z, y) for x in range(first_x, last_x + 1) for y in range(first_y, last_y + 1)}


server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()
binary = str(Path(sys.argv[1] if len(sys.argv) > 1 else "build/zen-map").resolve())

with tempfile.TemporaryDirectory(prefix="zen-map-continuity-") as directory:
    tmp = Path(directory)
    env = dict(os.environ, GDK_BACKEND="x11", GDK_DEBUG="no-portals", GTK_A11Y="none", GSK_RENDERER="cairo", GSETTINGS_BACKEND="memory", G_DEBUG="fatal-warnings", GIO_USE_VFS="local",
               ZEN_MAP_TILE_URL=f"http://127.0.0.1:{server.server_port}/{{z}}/{{x}}/{{y}}.png",
               ZEN_MAP_TILE_ATTRIBUTION="Local continuity test", ZEN_MAP_TILE_CREDIT_URL="https://example.org")
    processes = []

    def launch(number):
        log = open(tmp / f"window-{number}.log", "w+")
        p = subprocess.Popen([binary], env=dict(env, XDG_CACHE_HOME=str(tmp / f"cache-{number}")), stdout=log, stderr=log)
        processes.append((p, log))
        found = []

        def find():
            try:
                found[:] = x("search", "--onlyvisible", "--pid", p.pid).splitlines()
                return bool(found)
            except subprocess.CalledProcessError:
                return False

        wait(find, "window did not open")
        win = found[-1]
        x("windowfocus", win)
        geom = dict(line.split("=") for line in x("getwindowgeometry", "--shell", win).splitlines())
        left, top, width, height = (int(geom[k]) for k in ("X", "Y", "WIDTH", "HEIGHT"))
        # Keep hover controls away from the pixel probes and animation anchor.
        x("mousemove", left + 20, top + height // 2)
        return p, win, (left, top, width, height)

    def capture(geometry):
        left, top, width, height = geometry
        return ImageGrab.grab().crop((left, top, left + width, top + height)).convert("RGB")

    def matches(geometry, zoom, source_zoom, cx, cy, points):
        im = capture(geometry)
        _, _, width, height = geometry
        for px, py in points:
            world_x = (cx + (px - width / 2) / (256 * 2**zoom)) % 1
            world_y = cy + (py - height / 2) / (256 * 2**zoom)
            sx, sy = world_x * 2**source_zoom, world_y * 2**source_zoom
            expected = color(source_zoom, math.floor(sx), math.floor(sy), int((sx % 1) * 2), int((sy % 1) * 2))
            actual = im.getpixel((px, py))
            if max(abs(a - e) for a, e in zip(actual, expected)) > 4:
                return False
        return True

    def close(p, win):
        x("windowfocus", win)
        x("key", "ctrl+w")
        p.wait(timeout=3)
        assert p.returncode == 0, f"unexpected exit {p.returncode}"

    try:
        p, win, geometry = launch(1)
        width, height = geometry[2:]
        probes = [(140, 170), (290, 210), (500, 280), (800, 440), (920, 530)]
        assert not requests, "initial map made network requests"
        x("key", "m")
        initial = visible(2, .5, .5, width, height)
        wait(lambda: initial <= finished, "initial visible tiles did not load")
        wait(lambda: matches(geometry, 2, 2, .5, .5, probes), "initial pixels do not match tiles")
        gate.clear()
        x("key", "plus")
        wait(lambda: any(k[0] == 3 for k, _, _ in requests), "new zoom did not request detail")
        assert not any(k[0] == 3 for k in finished), "network gate failed"
        assert matches(geometry, 3, 2, .5, .5, probes), "zoom-in lost the ancestor image or cropped it incorrectly"
        gate.set()
        current = visible(3, .5, .5, width, height)
        wait(lambda: current <= finished, "zoomed exact tiles did not finish")
        wait(lambda: matches(geometry, 3, 3, .5, .5, probes), "exact zoom detail did not replace fallback")
        wait(lambda: len({k for k in finished if k[0] == 3} - current) == 4, "bounded nearby tiles did not warm")
        time.sleep(.15)
        zoom_requests = [(k, active, done) for k, active, done in requests if k[0] == 3]
        near = {k for k, _, _ in zoom_requests} - current
        assert len(near) == 4, f"lookahead exceeded four tiles: {near}"
        xs, ys = [k[1] for k in current], [k[2] for k in current]
        for k, active, done in zoom_requests:
            if k in near:
                assert min(xs) - 1 <= k[1] <= max(xs) + 1 and min(ys) - 1 <= k[2] <= max(ys) + 1
                assert current <= done, "lookahead ran before visible requests finished"
                assert not active, "lookahead used more than one idle request"
        gate.clear()
        # The two central southern neighbors were warmed before moving; hold
        # every new response and verify those pixels appear immediately.
        assert (3, 3, 6) in near and (3, 4, 6) in near, near
        x("key", "--repeat", "5", "--delay", "15", "Down")
        cy = .5 + 500 / (256 * 2**3)
        wait(lambda: matches(geometry, 3, 3, .5, cy, [(450, 450), (600, 450), (450, 200)]),
             "pan did not retain visible pixels/use warmed adjacent tiles")
        close(p, win)
        gate.set()
        wait(lambda: not inflight, "cancelled server requests did not drain")
        with lock:
            requests.clear()
            finished.clear()

        # Start at z3 without ever loading z2, to exercise the descendant path.
        p, win, geometry = launch(2)
        x("key", "plus")
        time.sleep(.55)
        x("key", "m")
        wait(lambda: current <= finished, "second visible tile set did not load")
        descendant_probes = [(350, 230), (470, 330), (630, 420), (770, 520)]
        wait(lambda: matches(geometry, 3, 3, .5, .5, descendant_probes), "second exact image missing")
        gate.clear()
        x("key", "minus")
        wait(lambda: any(k[0] == 2 for k, _, _ in requests), "zoom-out did not request coarser detail")
        assert not any(k[0] == 2 for k in finished)
        assert matches(geometry, 2, 3, .5, .5, descendant_probes), "zoom-out lost or misplaced cached descendants"
        gate.set()
        wait(lambda: initial <= finished, "zoom-out detail did not finish")
        wait(lambda: matches(geometry, 2, 2, .5, .5, descendant_probes), "coarser exact detail did not replace descendants")
        close(p, win)
        for proc, log in processes:
            log.seek(0)
            output = log.read()
            assert "CRITICAL" not in output and "WARNING" not in output, output
        print("PASS: gated ancestor/descendant crop transforms, progressive exact refinement, pan continuity, four adjacent tiles, visible-first ordering, single idle lookahead request")
    finally:
        gate.set()
        for proc, log in processes:
            if proc.poll() is None:
                proc.terminate()
                proc.wait(timeout=3)
            log.close()
        server.shutdown()
