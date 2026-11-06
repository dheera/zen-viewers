#!/usr/bin/env python3
"""Generated binary PLY regressions. No external model or display is required."""
import json
import math
import os
from pathlib import Path
import shlex
import struct
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
VERTICES = [(0., 0., 0.), (4., 0., 0.), (0., 1., 0.), (0., 0., 2.)]
FACES = [(0, 1, 2), (0, 3, 1)]


def header(vertices, faces=None, *, endian="little", scalar="float", count="uchar",
           index="int", name="vertex_indices", extra="", newline="\n", comment=""):
    lines = ["ply", f"format binary_{endian}_endian 1.0"]
    if comment:
        lines.append(comment)
    lines += [f"element vertex {vertices}", f"property {scalar} x", f"property {scalar} y",
              f"property {scalar} z"]
    if extra:
        lines.extend(extra.splitlines())
    if faces is not None:
        lines += [f"element face {faces}", f"property list {count} {index} {name}"]
    return (newline.join(lines + ["end_header", ""])).encode()


def body(vertices=VERTICES, faces=FACES, endian="<"):
    data = b"".join(struct.pack(endian + "fff", *v) for v in vertices)
    return data + b"".join(struct.pack(endian + "B" + "i" * len(face), len(face), *face) for face in faces)


def main():
    flags = shlex.split(subprocess.check_output(
        ["pkg-config", "--cflags", "--libs", "assimp", "gdk-pixbuf-2.0", "gio-2.0"], text=True))
    with tempfile.TemporaryDirectory(prefix="zen-3d-ply-") as temp:
        directory = Path(temp)
        executable = directory / "check"
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-I" + str(HERE.parent / "src"), str(HERE / "ply-check.c"),
                        str(HERE.parent / "src/model.c"), str(HERE.parent / "src/ply.c"),
                        "-o", str(executable), *flags, "-lm"], check=True)

        def check(name, contents=None, *, mode="direct", success=True, supported=True):
            if contents is not None:
                (directory / name).write_bytes(contents)
            result = subprocess.run([executable, mode, directory / name], capture_output=True,
                                    text=True, timeout=15)
            assert result.returncode == (0 if success else 1), (name, result)
            value = json.loads(result.stdout)
            assert value["handled"] is supported, (name, value)
            if not success:
                assert result.stderr, name
            return value

        packed = header(4, 2) + body()
        mesh = check("indexed.ply", packed)
        assert mesh["vertices"] == 4 and mesh["indices"] == 6, mesh
        assert mesh["index_data"] == [0, 1, 2, 0, 3, 1] and mesh["dimensions"] == [4, 1, 2], mesh
        assert mesh["parts"] == [0] and mesh["progress_last"] == 90 and mesh["previews"] == 0, mesh
        expected = [0, 2 / math.sqrt(5), 1 / math.sqrt(5)]
        assert all(abs(a-b) < 1e-5 for a, b in zip(mesh["data"][0][3:6], expected)), mesh
        assert mesh["data"][2][3:6] == [0, 0, 1] and mesh["data"][3][3:6] == [0, 1, 0], mesh
        integrated = check("indexed.ply", mode="fallback")
        assert integrated["vertices"] == 4 and integrated["indices"] == 6, integrated
        check("aliases.ply", header(4, 2, scalar="float32", count="uint8", index="uint32",
                                    name="vertex_index", newline="\r\n", comment="obj_info generated test") + body())
        points = check("points.ply", header(4) + body(faces=[]))
        assert points["vertices"] == points["indices"] == 4 and points["parts"] == [2], points
        check("huge-coordinates.ply", header(3, 1) + body(
            [(0, 0, 0), (1e20, 0, 0), (0, 1e20, 0)], [(0, 1, 2)]))
        print("PASS shared indexing, weighted normals, normalization, aliases and point clouds")

        for name, value in {
            "empty.ply": header(0, 0),
            "missing-data.ply": header(4, 2),
            "truncated-vertices.ply": header(4, 2) + body()[:47],
            "truncated-faces.ply": packed[:-1],
            "negative-index.ply": header(4, 1) + body(faces=[(0, -1, 2)]),
            "outside-index.ply": header(4, 1) + body(faces=[(0, 4, 2)]),
            "nan.ply": header(3, 1) + body([(float("nan"), 0, 0), (1, 0, 0), (0, 1, 0)], [(0, 1, 2)]),
            "infinite.ply": header(3, 1) + body([(float("inf"), 0, 0), (1, 0, 0), (0, 1, 0)], [(0, 1, 2)]),
            "budget.ply": header(20_000_000, 1),
            "count-overflow.ply": header("18446744073709551616", 1),
        }.items():
            check(name, value, success=False)
        assert check("indexed.ply", mode="cancel", success=False)["cancelled"]
        with (directory / "oversize.ply").open("wb") as stream:
            stream.write(packed)
            stream.truncate(256 * 1024 * 1024 + 1)
        check("oversize.ply", success=False)
        print("PASS truncated/malformed/nonfinite/index/budget rejection and pre-cancellation")

        check("big-endian.ply", header(4, 2, endian="big") + body(endian=">"), supported=False)
        assert check("big-endian.ply", mode="fallback")["dimensions"] == [4, 1, 2]
        colored_header = header(3, 1, extra="property uchar red\nproperty uchar green\nproperty uchar blue")
        colored_data = b"".join(struct.pack("<fffBBB", *v, *rgb) for v, rgb in zip(
            VERTICES[:3], [(255, 0, 0), (0, 255, 0), (0, 0, 255)])) + struct.pack("<Biii", 3, 0, 1, 2)
        check("colored.ply", colored_header + colored_data, supported=False)
        colored = check("colored.ply", mode="fallback")
        assert sorted(v[6:9] for v in colored["data"]) == [[0, 0, 255], [0, 255, 0], [255, 0, 0]], colored
        check("quad.ply", header(4, 1) + body(faces=[(0, 1, 2, 3)]), supported=False)
        assert check("quad.ply", mode="fallback")["indices"] == 6
        check("texture-comment.ply", header(4, 2, comment="comment TextureFile texture.png") + body(), supported=False)
        check("ascii.ply", b"ply\nformat ascii 1.0\nend_header\n", supported=False)
        print("PASS explicit fallback preserves big-endian geometry, colors, polygons and texture declarations")

        # About 20 MiB on disk: enough faces to exercise progressive preview, while
        # staying far smaller than the user's external multi-million-face model.
        with (directory / "preview.ply").open("wb") as stream:
            stream.write(header(70_002, 1_500_000))
            stream.write(body(VERTICES[:3], []) * 23_334)
            face = struct.pack("<Biii", 3, 0, 1, 2)
            for start in range(0, 1_500_000, 65536):
                stream.write(face * min(65536, 1_500_000 - start))
        progressive = check("preview.ply")
        assert progressive["vertices"] == 70_002 and progressive["indices"] == 4_500_000, progressive
        assert progressive["previews"] == 1 and progressive["preview_vertices"] == 65536, progressive
        assert progressive["progress_calls"] > 3 and progressive["progress_last"] == 90, progressive
        stopped = check("preview.ply", mode="cancel-progress", success=False)
        assert stopped["cancelled"] and stopped["previews"] == 1, stopped
        with (directory / "preview.ply").open("r+b") as stream:
            stream.seek(-13, 2)
            stream.write(struct.pack("<Biiii", 4, 0, 1, 2, 0))
        late_polygon = check("preview.ply", supported=False)
        assert late_polygon["previews"] == 0, late_polygon
        print("PASS bounded early preview ownership, monotonic progress and cancellation during face loading")


if __name__ == "__main__":
    main()
