#!/usr/bin/env python3
"""Importer-only regression checks; no display or downloaded fixtures required."""
import base64
import json
import math
import os
from pathlib import Path
import shlex
import struct
import subprocess
import tempfile
import zlib

HERE = Path(__file__).resolve().parent


def png():
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    pixels = bytes((0, 255, 0, 0, 0, 255, 0, 0, 0, 0, 255, 255, 255, 0))
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 2, 2, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(pixels)) + chunk(b"IEND", b"")


def fixtures(directory):
    (directory / "mats").mkdir()
    texture = png()
    (directory / "mats/checker.png").write_bytes(texture)
    (directory / "mats/surface.mtl").write_text("newmtl surface\nKd 0.2 0.4 0.6\nd 0.75\nmap_Kd checker.png\n")
    (directory / "mixed.obj").write_text("mtllib mats/surface.mtl\nv 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nusemtl surface\nf 1/1 2/2 3/3 4/4\nl 1 2\np 3\n")
    (directory / "triangle.stl").write_text("solid zen\nfacet normal 0 0 0\nouter loop\nvertex 0 0 0\nvertex 0 1 0\nvertex 0 0 1\nendloop\nendfacet\nendsolid zen\n")
    (directory / "points.ply").write_text("ply\nformat ascii 1.0\nelement vertex 3\nproperty float x\nproperty float y\nproperty float z\nproperty uchar red\nproperty uchar green\nproperty uchar blue\nend_header\n0 0 0 255 0 0\n1 0 0 0 255 0\n0 2 0 0 0 255\n")
    binary = bytearray()
    views, accessors = [], []
    def buffer(values, fmt, kind, component, count):
        while len(binary) % 4:
            binary.append(0)
        data = struct.pack("<" + fmt * len(values), *values)
        views.append({"buffer": 0, "byteOffset": len(binary), "byteLength": len(data)})
        binary.extend(data)
        accessors.append({"bufferView": len(views)-1, "componentType": component, "count": count, "type": kind})
        return len(accessors)-1
    position = buffer((0,0,0, 1,0,0, 0,1,0), "f", "VEC3", 5126, 3)
    normal = buffer((math.sqrt(.5),0,math.sqrt(.5))*3, "f", "VEC3", 5126, 3)
    uv = buffer((0,0, 1,0, 0,1), "f", "VEC2", 5126, 3)
    color = buffer((.25,.5,.75,1)*3, "f", "VEC4", 5126, 3)
    indices = buffer((0,1,2), "H", "SCALAR", 5123, 3)
    accessors[position].update(min=[0,0,0], max=[1,1,0])
    doc = {"asset":{"version":"2.0"}, "scene":0, "scenes":[{"nodes":[0]}],
           "nodes":[{"translation":[10,0,0],"children":[1,2]}, {"mesh":0,"scale":[2,1,1]},
                    {"mesh":0,"translation":[5,0,0],"scale":[-1,2,1]}],
           "meshes":[{"primitives":[{"attributes":{"POSITION":position,"NORMAL":normal,"TEXCOORD_0":uv,"COLOR_0":color},"indices":indices,"material":0}]}],
           "materials":[{"pbrMetallicRoughness":{"baseColorFactor":[.5,.75,1,.8],"baseColorTexture":{"index":0}},"alphaMode":"BLEND"}],
           "textures":[{"source":0}], "images":[{"uri":"mats/checker.png"}],
           "bufferViews":views, "accessors":accessors,
           "buffers":[{"byteLength":len(binary),"uri":"data:application/octet-stream;base64,"+base64.b64encode(binary).decode()}]}
    (directory / "transforms.gltf").write_text(json.dumps(doc))
    malformed = json.loads(json.dumps(doc))
    bad_buffer = bytearray(binary)
    struct.pack_into("<H", bad_buffer, views[indices]["byteOffset"], 65535)
    malformed["buffers"][0]["uri"] = "data:application/octet-stream;base64,"+base64.b64encode(bad_buffer).decode()
    (directory / "bad-index.gltf").write_text(json.dumps(malformed))
    while len(binary) % 4:
        binary.append(0)
    views.append({"buffer":0,"byteOffset":len(binary),"byteLength":len(texture)})
    binary.extend(texture)
    doc["images"] = [{"bufferView":len(views)-1,"mimeType":"image/png"}]
    doc["buffers"] = [{"byteLength":len(binary)}]
    data = json.dumps(doc).encode()
    data += b" " * (-len(data) % 4)
    binary.extend(b"\0" * (-len(binary) % 4))
    glb = struct.pack("<4sII", b"glTF", 2, 12+8+len(data)+8+len(binary))
    glb += struct.pack("<I4s",len(data),b"JSON")+data+struct.pack("<I4s",len(binary),b"BIN\0")+binary
    (directory / "embedded.glb").write_bytes(glb)
    (directory / "invalid.obj").write_text("this is not an OBJ model")
    os.mkfifo(directory / "pipe.obj")
    with (directory / "oversize.obj").open("wb") as file:
        file.truncate(256*1024*1024+1)


def main():
    flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "assimp", "gdk-pixbuf-2.0", "gio-2.0"], text=True))
    with tempfile.TemporaryDirectory(prefix="zen-3d-import-") as temp:
        directory = Path(temp)
        executable = directory / "check"
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-I"+str(HERE.parent/"src"),
                        str(HERE/"import-check.c"), str(HERE.parent/"src/model.c"), str(HERE.parent/"src/ply.c"),
                        "-o", str(executable), *flags, "-lm"], check=True)
        fixtures(directory)
        def load(name):
            return json.loads(subprocess.check_output([executable, directory/name], text=True, timeout=15))
        mixed = load("mixed.obj")
        assert mixed["parts"] == [1,1,1] and mixed["indices"] == 9, mixed
        assert mixed["textures"] and mixed["dimensions"] == [1,1,0], mixed
        assert any(abs(v[7]-1) < 1e-6 for v in mixed["data"]), mixed
        assert any(abs(c[0]-.2) < 1e-5 and abs(c[3]-.75) < 1e-5 for c in mixed["materials"]), mixed
        triangle = load("triangle.stl")
        assert triangle["parts"] == [1,0,0] and triangle["indices"] == 3, triangle
        assert all(abs(v[3]-1) < 1e-5 for v in triangle["data"]), triangle
        points = load("points.ply")
        assert points["parts"] == [0,0,1] and points["dimensions"] == [1,2,0], points
        assert sorted(v[8:11] for v in points["data"]) == [[0,0,255],[0,255,0],[255,0,0]], points
        for name in ("transforms.gltf", "embedded.glb"):
            scene = load(name)
            assert scene["vertices"] == 6 and scene["indices"] == 6 and scene["parts"] == [2,0,0], scene
            assert scene["dimensions"] == [5,2,0] and scene["textures"] == 1, scene
            assert all(v[8:] == [64,128,191,255] for v in scene["data"]), scene
            assert abs(scene["data"][0][3] - 1/math.sqrt(5)) < 1e-5, scene
            assert abs(scene["data"][0][5] - 2/math.sqrt(5)) < 1e-5, scene
            assert abs(scene["data"][3][3] + math.sqrt(.5)) < 1e-5, scene
            assert scene["index_data"] == [0,1,2,3,5,4], scene
            assert scene["data"][0][6:8] == [0,0], scene
            assert any(abs(c[3]-.8) < 1e-5 for c in scene["materials"]), scene
        for name in ("invalid.obj", "oversize.obj", "missing.obj", "bad-index.gltf", "pipe.obj"):
            result = subprocess.run([executable, directory/name], text=True, capture_output=True, timeout=15)
            assert result.returncode == 1 and result.stderr, (name, result)
        subprocess.run([executable, "--concurrent", directory/"transforms.gltf"], check=True, timeout=15)
        print("PASS OBJ sidecars, triangles/lines/points, STL, colored PLY, glTF/GLB instancing, transforms, normals, mirrored winding, textures/UV, alpha, limits, cancellation and concurrent callers")


if __name__ == "__main__":
    main()
