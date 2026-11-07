#!/usr/bin/env python3
"""Exercise visible table selection, clipboard, search, distant rows and close."""
import csv
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

if not os.environ.get("ZEN_TABLE_UI_TEST_DISPLAY"):
    env = dict(os.environ, ZEN_TABLE_UI_TEST_DISPLAY="1", GDK_BACKEND="x11", GSK_RENDERER="cairo",
               GTK_A11Y="none", G_DEBUG="fatal-warnings", GIO_USE_VFS="local", GDK_DEBUG="no-portals")
    raise SystemExit(subprocess.call(["xvfb-run", "-a", "-s", "-screen 0 1280x900x24",
                                     "dbus-run-session", "--", sys.executable, __file__, *sys.argv[1:]], env=env))

binary = str(Path(sys.argv[1] if len(sys.argv) > 1 else "build/zen-table").resolve())

def x(*args):
    return subprocess.check_output(["xdotool", *map(str, args)], text=True, timeout=8).strip()

def copy(whole=False):
    x("key", "ctrl+shift+c" if whole else "ctrl+c")
    result = subprocess.run(["xclip", "-selection", "clipboard", "-o"], text=True, capture_output=True, timeout=3)
    return result.stdout

def wait(check, label):
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        if check():
            return
        time.sleep(.08)
    raise AssertionError(label + "; clipboard=" + repr(copy()))

with tempfile.TemporaryDirectory(prefix="zen-table-ui-") as tmp:
    path = Path(tmp) / "large.csv"
    with path.open("w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["Name", "Notes", "Count"])
        writer.writerows((f"row-{i}", f"note {i}", i * 2) for i in range(100_000))
    sources = [(path, 100_000)]
    try:
        import pyarrow as pa
        import pyarrow.parquet as pq
        parquet = Path(tmp) / "large.parquet"
        pq.write_table(pa.table({"Name": [f"row-{i}" for i in range(100_000)],
                                 "Notes": [f"note {i}" for i in range(100_000)],
                                 "Count": [i*2 for i in range(100_000)]}), parquet, row_group_size=4096)
        sources.append((parquet, 100_000))
    except ImportError:
        # The native runtime must still receive real Parquet UI coverage when
        # PyArrow is absent from the developer's test environment.
        sources.append((Path(__file__).resolve().parent / "fixtures" / "navigation.parquet", 257))
    for source, row_count in sources:
        with (Path(tmp) / "log").open("w+") as log:
            app = subprocess.Popen([binary, str(source)], stdout=log, stderr=log)
            try:
                win = x("search", "--sync", "--onlyvisible", "--pid", app.pid, "--name", source.name).splitlines()[0]
                x("windowmove", win, 0, 0)
                x("windowfocus", win)
                time.sleep(.4)
                x("mousemove", "--window", win, 310, 111, "click", 1)
                wait(lambda: copy() == "note 0", f"{source.suffix}: cell copy failed")
                assert copy(True) == "row-0\tnote 0\t0"
                x("key", "ctrl+l")
                x("type", str(row_count - 1))
                x("key", "Return")
                wait(lambda: copy() == f"note {row_count - 2}", f"{source.suffix}: distant row did not load")
                x("key", "ctrl+f")
                found_row = min(733, row_count // 2)
                x("type", f"row-{found_row}")
                x("key", "Return")
                time.sleep(.4)
                x("key", "Escape")
                wait(lambda: copy() == f"note {found_row}", f"{source.suffix}: background find failed")
                x("key", "ctrl+End")
                wait(lambda: copy() == f"note {row_count - 1}", f"{source.suffix}: end row did not load")
                x("key", "ctrl+w")
                assert app.wait(timeout=8) == 0, "Table did not close cleanly"
                log.seek(0)
                errors = log.read()
                assert "CRITICAL" not in errors and "Gtk-WARNING" not in errors, errors
                print(f"PASS {source.suffix}: cell/row copy, jump to row {row_count - 1}, global find, end and close")
            finally:
                if app.poll() is None:
                    app.terminate()
                    app.wait(timeout=5)
