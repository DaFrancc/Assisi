#!/usr/bin/env python3
"""Generates assets/meshes/crt_screen.gltf: the glass of an old CRT monitor.

A CRT's faceplate is a slice of a sphere: the tube is a vacuum, and a domed
front stands up to the air pressing on it. So the glass bulges towards the
viewer, its middle nearest and its corners curving back by

    sag(x, y) = R - sqrt(R^2 - x^2 - y^2)

for a sphere of radius R centred R behind the middle. Real tubes used a radius
of about one and a half times the screen's diagonal; this one is a little
rounder than that, so the curve reads at a glance.

The mesh is a grid over the screen, 1.6 by 1 metres, facing +z, its middle at
the origin. Its texture coordinates run from (0, 0) at the top-left to (1, 1)
at the bottom-right, measured flat across the screen, so a picture shown on it
covers the glass and bends with it, as a CRT's picture does. Its normals point
out from the sphere's centre. It has one material slot, "Screen", for a
ScreenSurface to show a screen in.

    python3 scripts/make-crt-screen.py

The sandbox's levels/Test.alvl shows ui/Monitor.amdn on it. The mesh is given a
fixed id in its .aast sidecar, written only where none exists yet, so the
level finds it however many times this runs. The editor fills in the rest of
the sidecar, and the material file beside it, the first time it opens it.
"""

import base64
import json
import math
import pathlib
import struct
import uuid

REPO = pathlib.Path(__file__).resolve().parent.parent
MESH = REPO / "assets" / "meshes" / "crt_screen.gltf"

# The id levels/Test.alvl names the mesh by: a version 5 UUID of a fixed name,
# so it is the same on every machine.
MESH_ID = str(uuid.uuid5(uuid.NAMESPACE_URL, "https://github.com/DaFrancc/Assisi/meshes/crt_screen"))

# The screen's size in metres, across and up: the sandbox monitor's 16:10.
WIDTH = 1.6
HEIGHT = 1.0

# The faceplate's radius in metres: about 1.3 diagonals, a little rounder than
# a real tube's 1.5 so the bulge shows from across a room.
RADIUS = 2.5

# Grid cells across and up. Enough that the curve's silhouette and its
# shading are smooth at arm's length.
COLUMNS = 32
ROWS = 20

# glTF's codes for the data the buffers hold.
FLOAT = 5126
UNSIGNED_SHORT = 5123
ARRAY_BUFFER = 34962
ELEMENT_ARRAY_BUFFER = 34963
TRIANGLES = 4


def vertices():
    """Every grid point's position, normal and texture coordinate, row by row
    from the top."""
    out = []
    for row in range(ROWS + 1):
        v = row / ROWS
        y = (0.5 - v) * HEIGHT
        for column in range(COLUMNS + 1):
            u = column / COLUMNS
            x = (u - 0.5) * WIDTH
            # The sphere's surface, with its middle at z = 0.
            z = math.sqrt(RADIUS * RADIUS - x * x - y * y) - RADIUS
            # Out from the sphere's centre, at (0, 0, -RADIUS).
            nx, ny, nz = x / RADIUS, y / RADIUS, (z + RADIUS) / RADIUS
            out.append(((x, y, z), (nx, ny, nz), (u, v)))
    return out


def indices():
    """Two triangles a cell, wound anticlockwise seen from the front (+z)."""
    out = []
    stride = COLUMNS + 1
    for row in range(ROWS):
        for column in range(COLUMNS):
            top_left = row * stride + column
            top_right = top_left + 1
            bottom_left = top_left + stride
            bottom_right = bottom_left + 1
            out += [top_left, bottom_left, bottom_right, top_left, bottom_right, top_right]
    return out


def gltf():
    points = vertices()
    triangles = indices()
    positions = b"".join(struct.pack("<3f", *p) for p, _, _ in points)
    normals = b"".join(struct.pack("<3f", *n) for _, n, _ in points)
    uvs = b"".join(struct.pack("<2f", *t) for _, _, t in points)
    index_bytes = b"".join(struct.pack("<H", i) for i in triangles)
    blob = positions + normals + uvs + index_bytes

    xs = [p[0] for p, _, _ in points]
    ys = [p[1] for p, _, _ in points]
    zs = [p[2] for p, _, _ in points]
    views = []
    offset = 0
    for data, target in ((positions, ARRAY_BUFFER), (normals, ARRAY_BUFFER), (uvs, ARRAY_BUFFER),
                         (index_bytes, ELEMENT_ARRAY_BUFFER)):
        views.append({"buffer": 0, "byteOffset": offset, "byteLength": len(data), "target": target})
        offset += len(data)

    count = len(points)
    return {
        "asset": {"version": "2.0", "generator": "assisi-make-crt-screen"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": "CrtScreen"}],
        "materials": [{"name": "Screen",
                       "pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1], "metallicFactor": 0,
                                                "roughnessFactor": 0.25}}],
        "meshes": [{"name": "CrtScreen",
                    "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
                                    "indices": 3, "material": 0, "mode": TRIANGLES}]}],
        "buffers": [{"byteLength": len(blob),
                     "uri": "data:application/octet-stream;base64," + base64.b64encode(blob).decode()}],
        "bufferViews": views,
        "accessors": [
            {"bufferView": 0, "componentType": FLOAT, "count": count, "type": "VEC3",
             "min": [min(xs), min(ys), min(zs)], "max": [max(xs), max(ys), max(zs)]},
            {"bufferView": 1, "componentType": FLOAT, "count": count, "type": "VEC3"},
            {"bufferView": 2, "componentType": FLOAT, "count": count, "type": "VEC2"},
            {"bufferView": 3, "componentType": UNSIGNED_SHORT, "count": len(triangles), "type": "SCALAR"},
        ],
    }


def main():
    MESH.parent.mkdir(parents=True, exist_ok=True)
    MESH.write_text(json.dumps(gltf(), separators=(",", ":")) + "\n")
    sidecar = MESH.with_name(MESH.name + ".aast")
    if not sidecar.exists():
        sidecar.write_text(json.dumps({"guid": MESH_ID, "type": "AssetSidecar", "uses": [{"kind": "mesh"}],
                                       "version": 1}, indent=2) + "\n")
    sag = RADIUS - math.sqrt(RADIUS * RADIUS - (WIDTH / 2) ** 2 - (HEIGHT / 2) ** 2)
    print(f"wrote {MESH.relative_to(REPO)} ({MESH_ID}); corners {sag:.3f} m behind the middle")


if __name__ == "__main__":
    main()
