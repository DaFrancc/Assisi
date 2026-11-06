#!/usr/bin/env python3
"""Generates levels/Terminator.alvl and the low-poly meshes it is built from.

The level checks how smooth-shaded low-poly models take shadows where the light
turns away from them: spheres and cylinders of 8, 16 and 32 sides, a torus, a
cone made to be smooth-shaded and one smoothed through its tip (which the
importer warns about), and an arm skinned to two joints whose rest pose bends
it at the elbow. A 32-sided sphere with a normal map of bumps shows how a
normal-mapped surface fades out where the light leaves it. One row is lit by a
low sun, one by a point light and one by a spot light; a bar throws its shadow
across a sphere, and a small block rests on a large eight-sided log lying on
its side.

    python3 scripts/make-terminator-level.py [sun elevation in degrees]

The meshes go to assets/models/terminator/, which is local like the rest of
assets/models/; the level is committed. Each mesh is given a fixed id in its
.aast sidecar, written only where none exists yet, so the committed level finds
the meshes on any machine that runs this. The editor fills in the rest of each
sidecar, and the material file beside each mesh, the first time it opens them.

Every mesh is about a metre across, sits on y = 0, and has one grey, rough
material named "Shape". Smooth sides share their normals; caps and bases have
flat normals of their own, as a careful modeller splits them.
"""

import json
import math
import pathlib
import struct
import sys
import uuid
import zlib

REPO = pathlib.Path(__file__).resolve().parent.parent
ASSETS = REPO / "assets"
MODELS = "models/terminator"
LEVEL = ASSETS / "levels" / "Terminator.alvl"

# The ids the level names the meshes by: version 5 UUIDs of a fixed name, so
# every run on every machine gives the same ones.
ID_NAMESPACE = uuid.uuid5(uuid.NAMESPACE_URL, "https://github.com/DaFrancc/Assisi/levels/Terminator")

GREY = [0.8, 0.8, 0.8, 1.0]
ROUGHNESS = 0.9
DEFAULT_SUN_ELEVATION = 10.0
SUN_AZIMUTH_DEGREES = 15.0
SPACING = 2.5

WHITE_MATERIAL = {"guid": "eae01c9e-6ef3-4d74-9ca3-0d02e5d427dc", "path": "materials/shadow_white.amat"}
CUBE = {"guid": "00000000-0000-0000-0000-000000000001", "path": "prim://cube"}
IDENTITY = [1.0, 0.0, 0.0, 0.0]

# glTF's numbers for the parts of a binary file and the types of an accessor.
GLB_MAGIC, GLB_VERSION, GLB_JSON, GLB_BIN = 0x46546C67, 2, 0x4E4F534A, 0x004E4942
FLOAT, UNSIGNED_BYTE, UNSIGNED_INT = 5126, 5121, 5125

# The bump map: a square of so many texels, with so many bumps along each side,
# each sloping at most so steeply (rise over run, in texture space).
BUMP_TEXELS = 256
BUMPS_ACROSS = 8
BUMP_SLOPE = 0.5
BUMP_MAP = "bumps.png"

# The arm: a tube along +X from the shoulder at the origin, bent at the elbow.
ARM_LENGTH = 2.0
ARM_RADIUS = 0.15
ELBOW = 1.0
ELBOW_BLEND = 0.2


def norm(v):
    length = math.sqrt(sum(c * c for c in v))
    return [c / length for c in v]


class Mesh:
    def __init__(self):
        self.positions, self.normals, self.indices = [], [], []
        self.joints, self.weights, self.uvs = [], [], []

    def vertex(self, position, normal, joint=None, weight=None, uv=None):
        self.positions.append(list(position))
        self.normals.append(norm(normal))
        if joint is not None:
            self.joints.append(joint)
            self.weights.append(weight)
        if uv is not None:
            self.uvs.append(list(uv))
        return len(self.positions) - 1

    def triangle(self, a, b, c, inside):
        """Adds a, b, c wound counter-clockwise seen from away from `inside`."""
        pa, pb, pc = (self.positions[i] for i in (a, b, c))
        e1 = [pb[k] - pa[k] for k in range(3)]
        e2 = [pc[k] - pa[k] for k in range(3)]
        face = [e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]]
        outward = sum(face[k] * (pa[k] - inside[k]) for k in range(3)) > 0
        self.indices += [a, b, c] if outward else [a, c, b]


def ring_points(sides, radius):
    return [(radius * math.cos(2 * math.pi * k / sides), radius * math.sin(2 * math.pi * k / sides))
            for k in range(sides)]


def uv_sphere(segments, rings, radius=0.5):
    m = Mesh()
    centre = [0, radius, 0]
    grid = []
    for r in range(rings + 1):
        polar = math.pi * r / rings
        row = []
        for s in range(segments):
            azimuth = 2 * math.pi * s / segments
            n = [math.sin(polar) * math.cos(azimuth), math.cos(polar), math.sin(polar) * math.sin(azimuth)]
            row.append(m.vertex([radius * n[0], radius + radius * n[1], radius * n[2]], n))
        grid.append(row)
    for r in range(rings):
        for s in range(segments):
            a, b = grid[r][s], grid[r][(s + 1) % segments]
            c, d = grid[r + 1][s], grid[r + 1][(s + 1) % segments]
            if r != 0:
                m.triangle(a, b, c, centre)
            if r != rings - 1:
                m.triangle(b, d, c, centre)
    return m


def mapped_sphere(segments, rings, radius=0.5):
    """A UV sphere with texture coordinates, its seam split so the map wraps."""
    m = Mesh()
    centre = [0, radius, 0]
    grid = []
    for r in range(rings + 1):
        polar = math.pi * r / rings
        row = []
        for s in range(segments + 1):
            azimuth = 2 * math.pi * s / segments
            n = [math.sin(polar) * math.cos(azimuth), math.cos(polar), math.sin(polar) * math.sin(azimuth)]
            row.append(m.vertex([radius * n[0], radius + radius * n[1], radius * n[2]], n,
                                uv=[s / segments, r / rings]))
        grid.append(row)
    for r in range(rings):
        for s in range(segments):
            a, b = grid[r][s], grid[r][s + 1]
            c, d = grid[r + 1][s], grid[r + 1][s + 1]
            if r != 0:
                m.triangle(a, b, c, centre)
            if r != rings - 1:
                m.triangle(b, d, c, centre)
    return m


def png(width, height, rgb_rows):
    """An 8-bit RGB PNG of the given rows, each a bytes of width * 3."""
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    raw = b"".join(b"\x00" + row for row in rgb_rows)
    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


def bump_map():
    """A tangent-space normal map of a grid of round bumps: the slope of
    sin(u) sin(v) at each texel, as glTF stores normals."""
    rows = []
    frequency = 2 * math.pi * BUMPS_ACROSS / BUMP_TEXELS
    for y in range(BUMP_TEXELS):
        row = bytearray()
        for x in range(BUMP_TEXELS):
            du = BUMP_SLOPE * math.cos(frequency * x) * math.sin(frequency * y)
            dv = BUMP_SLOPE * math.sin(frequency * x) * math.cos(frequency * y)
            n = norm([-du, -dv, 1.0])
            row += bytes(round((c * 0.5 + 0.5) * 255) for c in n)
        rows.append(bytes(row))
    return png(BUMP_TEXELS, BUMP_TEXELS, rows)


def cylinder(sides, radius=0.5, height=1.0):
    m = Mesh()
    centre = [0, height / 2, 0]
    ring = ring_points(sides, radius)
    bottom = [m.vertex([x, 0, z], [x, 0, z]) for x, z in ring]
    top = [m.vertex([x, height, z], [x, 0, z]) for x, z in ring]
    for k in range(sides):
        j = (k + 1) % sides
        m.triangle(bottom[k], bottom[j], top[k], centre)
        m.triangle(bottom[j], top[j], top[k], centre)
    for y, up in ((0, -1), (height, 1)):
        middle = m.vertex([0, y, 0], [0, up, 0])
        cap = [m.vertex([x, y, z], [0, up, 0]) for x, z in ring]
        for k in range(sides):
            m.triangle(middle, cap[k], cap[(k + 1) % sides], centre)
    return m


def torus(around=24, across=12, major=0.38, minor=0.12):
    m = Mesh()
    grid = []
    for i in range(around):
        u = 2 * math.pi * i / around
        row = []
        for j in range(across):
            v = 2 * math.pi * j / across
            n = [math.cos(v) * math.cos(u), math.sin(v), math.cos(v) * math.sin(u)]
            p = [(major + minor * math.cos(v)) * math.cos(u), minor + minor * math.sin(v),
                 (major + minor * math.cos(v)) * math.sin(u)]
            row.append(m.vertex(p, n))
        grid.append(row)
    for i in range(around):
        u = 2 * math.pi * (i + 0.5) / around
        tube_centre = [major * math.cos(u), minor, major * math.sin(u)]
        for j in range(across):
            a, b = grid[i][j], grid[(i + 1) % around][j]
            c, d = grid[i][(j + 1) % across], grid[(i + 1) % around][(j + 1) % across]
            m.triangle(a, b, c, tube_centre)
            m.triangle(b, d, c, tube_centre)
    return m


def cone(sides=16, radius=0.5, height=1.0, averaged=False):
    """A cone. `averaged` shares one vertex at the tip and one per base corner,
    which smooths over the tip and the base edge."""
    m = Mesh()
    inside = [0, height / 4, 0]

    def slant(angle):
        return norm([height * math.cos(angle), radius, height * math.sin(angle)])

    angles = [2 * math.pi * k / sides for k in range(sides)]
    ring = [[radius * math.cos(a), 0, radius * math.sin(a)] for a in angles]
    if averaged:
        tip = m.vertex([0, height, 0], [0, 1, 0])
        centre = m.vertex([0, 0, 0], [0, -1, 0])
        shared = [m.vertex(ring[k], [s + d for s, d in zip(slant(angles[k]), [0, -1, 0])]) for k in range(sides)]
        for k in range(sides):
            m.triangle(tip, shared[k], shared[(k + 1) % sides], inside)
            m.triangle(centre, shared[k], shared[(k + 1) % sides], inside)
        return m
    side = [m.vertex(ring[k], slant(angles[k])) for k in range(sides)]
    for k in range(sides):
        tip = m.vertex([0, height, 0], slant(angles[k] + math.pi / sides))
        m.triangle(tip, side[k], side[(k + 1) % sides], inside)
    centre = m.vertex([0, 0, 0], [0, -1, 0])
    base = [m.vertex(ring[k], [0, -1, 0]) for k in range(sides)]
    for k in range(sides):
        m.triangle(centre, base[k], base[(k + 1) % sides], inside)
    return m


def arm_weights(x):
    """The shoulder's and the elbow's pull on a vertex @p x along the arm,
    handed over smoothly across the elbow."""
    t = min(max((x - (ELBOW - ELBOW_BLEND)) / (2 * ELBOW_BLEND), 0.0), 1.0)
    t = t * t * (3 - 2 * t)
    return [0, 1, 0, 0], [1 - t, t, 0, 0]


def arm(sides=12, rings=20):
    m = Mesh()
    grid = []
    for r in range(rings + 1):
        x = ARM_LENGTH * r / rings
        joint, weight = arm_weights(x)
        grid.append([m.vertex([x, ARM_RADIUS + y, z], [0, y, z], joint, weight)
                     for y, z in ring_points(sides, ARM_RADIUS)])
    for r in range(rings):
        axis = [ARM_LENGTH * (r + 0.5) / rings, ARM_RADIUS, 0]
        for k in range(sides):
            j = (k + 1) % sides
            m.triangle(grid[r][k], grid[r][j], grid[r + 1][k], axis)
            m.triangle(grid[r][j], grid[r + 1][j], grid[r + 1][k], axis)
    for x, outward in ((0.0, -1), (ARM_LENGTH, 1)):
        joint, weight = arm_weights(x)
        middle = m.vertex([x, ARM_RADIUS, 0], [outward, 0, 0], joint, weight)
        cap = [m.vertex([x, ARM_RADIUS + y, z], [outward, 0, 0], joint, weight)
               for y, z in ring_points(sides, ARM_RADIUS)]
        for k in range(sides):
            m.triangle(middle, cap[k], cap[(k + 1) % sides], [x - outward, ARM_RADIUS, 0])
    return m


def glb(name, mesh, skinned, normal_map=None):
    blob = bytearray()
    views, accessors = [], []

    def add(data, fmt, component, kind, count, extra=None):
        while len(blob) % 4:
            blob.append(0)
        offset = len(blob)
        blob.extend(struct.pack("<" + fmt * len(data), *data))
        views.append({"buffer": 0, "byteOffset": offset, "byteLength": len(blob) - offset})
        accessor = {"bufferView": len(views) - 1, "componentType": component, "count": count, "type": kind}
        accessor.update(extra or {})
        accessors.append(accessor)
        return len(accessors) - 1

    low = [min(p[k] for p in mesh.positions) for k in range(3)]
    high = [max(p[k] for p in mesh.positions) for k in range(3)]
    attributes = {
        "POSITION": add([c for p in mesh.positions for c in p], "f", FLOAT, "VEC3", len(mesh.positions),
                        {"min": low, "max": high}),
        "NORMAL": add([c for n in mesh.normals for c in n], "f", FLOAT, "VEC3", len(mesh.normals)),
    }
    if skinned:
        attributes["JOINTS_0"] = add([c for j in mesh.joints for c in j], "B", UNSIGNED_BYTE, "VEC4",
                                     len(mesh.joints))
        attributes["WEIGHTS_0"] = add([c for w in mesh.weights for c in w], "f", FLOAT, "VEC4", len(mesh.weights))
    if mesh.uvs:
        attributes["TEXCOORD_0"] = add([c for uv in mesh.uvs for c in uv], "f", FLOAT, "VEC2", len(mesh.uvs))
    indices = add(mesh.indices, "I", UNSIGNED_INT, "SCALAR", len(mesh.indices))

    material = {"name": "Shape", "pbrMetallicRoughness": {"baseColorFactor": GREY, "metallicFactor": 0.0,
                                                           "roughnessFactor": ROUGHNESS}}
    gltf = {
        "asset": {"version": "2.0", "generator": "scripts/make-terminator-level.py"},
        "scene": 0,
        "meshes": [{"name": name, "primitives": [{"attributes": attributes, "indices": indices, "material": 0}]}],
        "materials": [material],
    }
    if normal_map is not None:
        material["normalTexture"] = {"index": 0}
        gltf["textures"] = [{"source": 0}]
        gltf["images"] = [{"uri": normal_map}]
    if skinned:
        # Bound straight: the shoulder at the origin, the elbow a metre along.
        # The elbow's rest pose turns it a right angle, so the arm loads bent.
        identity = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]
        to_elbow = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -ELBOW, 0, 0, 1]
        half = math.sqrt(0.5)
        gltf["skins"] = [{"joints": [1, 2], "skeleton": 1,
                          "inverseBindMatrices": add(identity + to_elbow, "f", FLOAT, "MAT4", 2)}]
        gltf["nodes"] = [{"name": "Arm", "mesh": 0, "skin": 0},
                         {"name": "Shoulder", "children": [2]},
                         {"name": "Elbow", "translation": [ELBOW, 0, 0], "rotation": [0, 0, half, half]}]
        gltf["scenes"] = [{"nodes": [0, 1]}]
    else:
        gltf["nodes"] = [{"name": name, "mesh": 0}]
        gltf["scenes"] = [{"nodes": [0]}]
    gltf["accessors"] = accessors
    gltf["bufferViews"] = views
    gltf["buffers"] = [{"byteLength": len(blob)}]

    text = json.dumps(gltf, separators=(",", ":")).encode()
    text += b" " * (-len(text) % 4)
    while len(blob) % 4:
        blob.append(0)
    body = struct.pack("<II", len(text), GLB_JSON) + text + struct.pack("<II", len(blob), GLB_BIN) + bytes(blob)
    return struct.pack("<III", GLB_MAGIC, GLB_VERSION, 12 + len(body)) + body


def mesh_id(name):
    return str(uuid.uuid5(ID_NAMESPACE, name))


def write_mesh(name, mesh, skinned=False, normal_map=None):
    path = ASSETS / MODELS / f"{name}.glb"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(glb(name, mesh, skinned, normal_map))
    sidecar = path.with_name(path.name + ".aast")
    if not sidecar.exists():
        sidecar.write_text(json.dumps({"guid": mesh_id(name), "type": "AssetSidecar", "uses": [{"kind": "mesh"}],
                                       "version": 1}, indent=2) + "\n")
    elif json.loads(sidecar.read_text())["guid"] != mesh_id(name):
        sys.exit(f"{sidecar} names a different id than the level expects; delete it and run again")


def transform(position, scale=(1.0, 1.0, 1.0), rotation=IDENTITY):
    return {"position": list(position), "rotation": list(rotation), "scale": list(scale)}


def model(entity, name, position, scale=(1.0, 1.0, 1.0), rotation=IDENTITY, skinned=False):
    components = {"MeshRenderer": {"castsShadows": True,
                                   "mesh": {"guid": mesh_id(name), "path": f"{MODELS}/{name}.glb"}},
                  "Transform": transform(position, scale, rotation)}
    if skinned:
        components["SkinnedMesh"] = {}
    return {"name": entity, "components": components}


def box(name, position, scale):
    return {"name": name, "components": {
        "MeshRenderer": {"castsShadows": True, "materialOverrides": [WHITE_MATERIAL], "mesh": CUBE},
        "Transform": transform(position, scale)}}


def level(elevation_degrees):
    elevation, azimuth = math.radians(elevation_degrees), math.radians(SUN_AZIMUTH_DEGREES)
    sun = [math.sin(azimuth) * math.cos(elevation), -math.sin(elevation), -math.cos(azimuth) * math.cos(elevation)]
    entities = [
        {"name": "Sun", "components": {"DirectionalLight": {"castsShadows": True, "direction": sun,
                                                            "intensity": 1.0}}},
        {"name": "Camera", "components": {
            "Camera": {"farZ": 200.0, "fovDegrees": 60.0, "isActive": True, "nearZ": 0.1},
            "Transform": transform((11.0, 2.5, 9.0))}},
        box("Ground", (11.0, -0.25, -8.0), (40.0, 0.5, 40.0)),
    ]

    # The sun row: every shape side by side, the arm at the end.
    shapes = ["sphere8", "sphere16", "sphere32", "cylinder8", "cylinder16", "cylinder32", "torus", "cone",
              "cone_averaged"]
    for index, shape in enumerate(shapes):
        entities.append(model(f"Sun_{shape}", shape, (index * SPACING, 0.0, 0.0)))
    entities.append(model("Sun_arm", "arm", (len(shapes) * SPACING, 0.0, 0.0), skinned=True))
    entities.append(model("Sun_sphere32_bumpy", "sphere32_bumpy", ((len(shapes) + 1.5) * SPACING, 0.0, 0.0)))

    # A bar between the sun and the 16-sided sphere, whose shadow crosses the
    # sphere where the light turns away from it.
    toward = math.hypot(sun[0], sun[2])
    entities.append(box("Bar", (SPACING - 2.0 * sun[0] / toward, 0.5, -2.0 * sun[2] / toward), (1.6, 0.1, 0.1)))

    # A small block resting on a large eight-sided log lying on its side.
    half = math.sqrt(0.5)
    entities.append(model("Log", "cylinder8", (30.0, 1.0, 0.0), (2.0, 2.0, 2.0), (half, 0.0, 0.0, half)))
    entities.append(box("Block", (29.0, 2.1, 0.0), (0.2, 0.2, 0.2)))

    # The point light row, lit low from in front.
    for index, shape in enumerate(["sphere16", "cylinder8", "torus"]):
        entities.append(model(f"Point_{shape}", shape, (index * SPACING, 0.0, -10.0)))
    entities.append({"name": "PointLight", "components": {
        "PointLight": {"castsShadows": True, "color": [0.4, 0.6, 1.0], "intensity": 120.0, "radius": 15.0},
        "Transform": transform((SPACING, 0.6, -7.0))}})

    # The spot row, lit from straight above between its shapes.
    for index, shape in enumerate(["sphere16", "cylinder8"]):
        entities.append(model(f"Spot_{shape}", shape, (index * SPACING, 0.0, -20.0)))
    entities.append({"name": "SpotLight", "components": {
        "SpotLight": {"castsShadows": True, "color": [1.0, 0.7, 0.4], "innerAngle": 40.0, "intensity": 200.0,
                      "outerAngle": 55.0, "radius": 12.0},
        "Transform": transform((SPACING / 2.0, 2.5, -20.0))}})
    return {"entities": entities, "systems": [], "version": 2}


def main():
    elevation = float(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_SUN_ELEVATION
    for sides in (8, 16, 32):
        write_mesh(f"sphere{sides}", uv_sphere(sides, sides // 2))
        write_mesh(f"cylinder{sides}", cylinder(sides))
    write_mesh("torus", torus())
    write_mesh("cone", cone())
    write_mesh("cone_averaged", cone(averaged=True))
    write_mesh("arm", arm(), skinned=True)
    (ASSETS / MODELS / BUMP_MAP).write_bytes(bump_map())
    write_mesh("sphere32_bumpy", mapped_sphere(32, 16), normal_map=BUMP_MAP)
    LEVEL.write_text(json.dumps(level(elevation), indent=2) + "\n")
    print(f"wrote {LEVEL.relative_to(REPO)} and the meshes in assets/{MODELS}/")


if __name__ == "__main__":
    main()
