#!/usr/bin/env python3
"""Opt-in check of an external model against an installed binary or extracted DEB.

The model is read in place. It is never copied into the checkout or package.
Output is JSON; timings include process startup and use a warm OS page cache
because the complete model is hashed before inspection. No display is needed.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def inspect(binary, model, timeout):
    # Files drain all output without an extra reader process. wait4 supplies
    # this child's CPU/RSS, rather than the maximum of earlier child commands.
    with tempfile.TemporaryFile() as output, tempfile.TemporaryFile() as errors:
        start = time.monotonic()
        process = subprocess.Popen([binary, "--inspect", model], stdout=output, stderr=errors,
                                   env={**os.environ, "LC_ALL": "C.UTF-8"})
        expired = False
        while True:
            pid, status, usage = os.wait4(process.pid, os.WNOHANG)
            if pid:
                break
            if time.monotonic() - start > timeout:
                os.kill(process.pid, 9)
                _, status, usage = os.wait4(process.pid, 0)
                expired = True
                break
            time.sleep(.01)
        elapsed = time.monotonic() - start
        process.returncode = os.waitstatus_to_exitcode(status)
        output.seek(0)
        errors.seek(0)
        return {
            "returncode": process.returncode,
            "timed_out": expired,
            "elapsed_seconds": round(elapsed, 6),
            "cpu_seconds": round(usage.ru_utime + usage.ru_stime, 6),
            "max_rss_kib": usage.ru_maxrss,
            "stdout": output.read().decode("utf-8", "replace"),
            "stderr": errors.read().decode("utf-8", "replace"),
        }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("model", type=Path)
    target = parser.add_mutually_exclusive_group()
    target.add_argument("--binary", type=Path, help="defaults to /usr/bin/zen-3d")
    target.add_argument("--deb", type=Path, help="extract and test this package without installing it")
    parser.add_argument("--expect-bytes", type=int)
    parser.add_argument("--expect-vertices", type=int)
    parser.add_argument("--expect-triangles", type=int)
    parser.add_argument("--expect-sha256")
    parser.add_argument("--expect-version")
    parser.add_argument("--timeout", type=float, default=180)
    args = parser.parse_args()
    model = args.model.resolve(strict=True)
    if not model.is_file() or args.timeout <= 0:
        parser.error("Choose a regular model file and a positive timeout.")
    metadata = {"path": str(model), "bytes": model.stat().st_size, "sha256": digest(model)}
    for key, expected in (("bytes", args.expect_bytes), ("sha256", args.expect_sha256)):
        if expected is not None and metadata[key] != expected:
            parser.error(f"Model {key} does not match: {metadata[key]}")
    with tempfile.TemporaryDirectory(prefix="zen-3d-package-smoke-") as temp:
        package = None
        if args.deb:
            archive = args.deb.resolve(strict=True)
            package = {"path": str(archive), "sha256": digest(archive)}
            subprocess.run(["dpkg-deb", "--extract", archive, temp], check=True)
            binary = Path(temp) / "usr/bin/zen-3d"
        else:
            binary = (args.binary or Path("/usr/bin/zen-3d")).resolve(strict=True)
        version = subprocess.check_output([binary, "--version"], text=True, timeout=10).strip()
        if args.expect_version and version != "zen-3d " + args.expect_version:
            parser.error(f"Unexpected binary version: {version}")
        report = {
            "model": metadata,
            "binary": {"path": str(binary), "version": version, "sha256": digest(binary)},
            "package": package,
            "cache_state": "warm OS cache: the model was SHA-256 hashed before import",
            "inspection": inspect(binary, model, args.timeout),
        }
    result = report["inspection"]
    counts = {}
    for label in ("Vertices", "Triangles"):
        match = re.search(r"^" + label + r"\s+([\d,]+)\s*$", result["stdout"], re.MULTILINE)
        if match:
            counts[label.lower()] = int(match.group(1).replace(",", ""))
    result["counts"] = counts
    result["passed"] = result["returncode"] == 0 and not result["timed_out"] and all(
        expected is None or counts.get(label) == expected
        for label, expected in (("vertices", args.expect_vertices), ("triangles", args.expect_triangles)))
    print(json.dumps(report, indent=2))
    raise SystemExit(0 if result["passed"] else 1)


if __name__ == "__main__":
    main()
