#!/usr/bin/env python3
"""Generates the performance measurement levels in assets/levels/.

These scenes are the instrument every later rendering stage is judged by, so
they are generated rather than hand-authored: the reference scenes publish exact
triangle, instance and light counts as a contract, and a contract nobody can
regenerate is a contract nobody can check. Run this after changing a scene
definition below, or after moving a rung of the prim:// tessellation ladder, and
commit what it writes.

    python3 scripts/make-perf-scenes.py

Placement is deterministic — a fixed LCG seeded per scene, not Python's `random`
— so regenerating produces byte-identical files on any machine and any Python
version. A scene whose contents drifted between regenerations would fold that
drift straight into the frame times it exists to report.

Triangle counts below MUST match Geometry::PrimitiveTessellation. The mismatch
is caught by TestDefaultMeshes.cpp ("The prim:// tessellation ladder matches the
counts the perf scenes publish") on the C++ side and asserted again here.
"""

import json
import math
import pathlib
import uuid

REPO = pathlib.Path(__file__).resolve().parent.parent
LEVELS = REPO / "assets" / "levels"

# prim:// path -> (reserved guid, triangles at the pinned tessellation).
# Mirrors Core/AssetId.cpp's built-in table and Geometry::PrimitiveTessellation.
PRIMITIVES = {
    "prim://cube":            ("00000000-0000-0000-0000-000000000001", 12),
    "prim://sphere-low":      ("00000000-0000-0000-0000-000000000005", 144),
    "prim://sphere":          ("00000000-0000-0000-0000-000000000006", 576),
    "prim://sphere-high":     ("00000000-0000-0000-0000-000000000007", 4096),
    "prim://icosphere-low":   ("00000000-0000-0000-0000-000000000008", 320),
    "prim://icosphere":       ("00000000-0000-0000-0000-000000000009", 1280),
    "prim://icosphere-high":  ("00000000-0000-0000-0000-00000000000a", 20480),
    "prim://cylinder":        ("00000000-0000-0000-0000-00000000000b", 96),
    "prim://cylinder-high":   ("00000000-0000-0000-0000-00000000000c", 256),
}

CHECKER_MATERIAL = {"guid": "8c08e9c0-e9fb-4f84-a9ba-7a90223526fd", "path": "materials/checker.amat"}


class Lcg:
    """A 32-bit linear congruential generator (Numerical Recipes constants).

    Written out rather than using `random` so the placement depends on nothing
    but the seed — not the interpreter version, not the platform.
    """

    def __init__(self, seed):
        self.state = seed & 0xFFFFFFFF

    def next_float(self):
        self.state = (1664525 * self.state + 1013904223) & 0xFFFFFFFF
        return self.state / 4294967296.0

    def range(self, low, high):
        return low + (high - low) * self.next_float()


def round6(value):
    """Keeps the emitted JSON stable and diffable rather than full of float noise."""
    return round(value + 0.0, 6)


def look_at_rotation(position, target):
    """Quaternion [w, x, y, z] for a camera at `position` looking at `target`.

    Mirrors EditorApp::SetupCamera exactly: the rotation matrix's columns are
    (right, up, -forward). Any other convention would aim the measurement camera
    somewhere the scene was not composed for.
    """
    fx, fy, fz = (target[i] - position[i] for i in range(3))
    length = math.sqrt(fx * fx + fy * fy + fz * fz)
    fx, fy, fz = fx / length, fy / length, fz / length

    # right = normalize(cross(forward, worldUp)), worldUp = +Y
    rx, ry, rz = fz, 0.0, -fx
    length = math.sqrt(rx * rx + ry * ry + rz * rz)
    rx, ry, rz = rx / length, ry / length, rz / length

    # up = normalize(cross(right, forward))
    ux = ry * fz - rz * fy
    uy = rz * fx - rx * fz
    uz = rx * fy - ry * fx

    # Columns (right, up, -forward); m[column][row].
    m = [[rx, ry, rz], [ux, uy, uz], [-fx, -fy, -fz]]
    trace = m[0][0] + m[1][1] + m[2][2]
    if trace > 0.0:
        s = math.sqrt(trace + 1.0) * 2.0
        w = 0.25 * s
        x = (m[1][2] - m[2][1]) / s
        y = (m[2][0] - m[0][2]) / s
        z = (m[0][1] - m[1][0]) / s
    elif m[0][0] > m[1][1] and m[0][0] > m[2][2]:
        s = math.sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2.0
        w = (m[1][2] - m[2][1]) / s
        x = 0.25 * s
        y = (m[1][0] + m[0][1]) / s
        z = (m[2][0] + m[0][2]) / s
    elif m[1][1] > m[2][2]:
        s = math.sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2.0
        w = (m[2][0] - m[0][2]) / s
        x = (m[1][0] + m[0][1]) / s
        y = 0.25 * s
        z = (m[2][1] + m[1][2]) / s
    else:
        s = math.sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2.0
        w = (m[0][1] - m[1][0]) / s
        x = (m[2][0] + m[0][2]) / s
        y = (m[2][1] + m[1][2]) / s
        z = 0.25 * s
    return [round6(w), round6(x), round6(y), round6(z)]


def transform(position, scale=(1.0, 1.0, 1.0), rotation=(1.0, 0.0, 0.0, 0.0)):
    return {
        "position": [round6(v) for v in position],
        "rotation": [round6(v) for v in rotation],
        "scale": [round6(v) for v in scale],
    }


def mesh_entity(name, primitive, position, scale):
    guid, _ = PRIMITIVES[primitive]
    return {
        "components": {
            "MeshRenderer": {
                "materialOverrides": [CHECKER_MATERIAL],
                "mesh": {"guid": guid, "path": primitive},
            },
            "Transform": transform(position, scale),
        },
        "name": name,
    }


def camera_entity(position, target):
    return {
        "components": {
            "Camera": {"farZ": 500.0, "fovDegrees": 60.0, "isActive": True, "nearZ": 0.1},
            "Transform": {
                "position": [round6(v) for v in position],
                "rotation": look_at_rotation(position, target),
                "scale": [1.0, 1.0, 1.0],
            },
        },
        "name": "MeasurementCamera",
    }


def sun_entity(direction=(-0.4, -1.0, -0.35), intensity=2.0):
    return {
        "components": {
            "DirectionalLight": {
                "color": [1.0, 0.98, 0.95],
                "direction": [round6(v) for v in direction],
                "intensity": intensity,
            }
        },
        "name": "Sun",
    }


def point_light_entity(name, position, color, radius, intensity):
    return {
        "components": {
            "PointLight": {
                "color": [round6(v) for v in color],
                "intensity": intensity,
                "radius": radius,
            },
            "Transform": transform(position),
        },
        "name": name,
    }


def spot_light_entity(name, position, color, radius, intensity):
    return {
        "components": {
            "SpotLight": {
                "color": [round6(v) for v in color],
                "direction": [0.0, -1.0, 0.0],
                "innerAngle": 18.0,
                "outerAngle": 32.0,
                "intensity": intensity,
                "radius": radius,
            },
            "Transform": transform(position),
        },
        "name": name,
    }


def mover_entity(name, primitive, origin, scale, axis, amplitude, period, phase):
    entity = mesh_entity(name, primitive, origin, scale)
    entity["components"]["Oscillator"] = {
        "origin": [round6(v) for v in origin],
        "axis": [round6(v) for v in axis],
        "amplitude": round6(amplitude),
        "periodSeconds": round6(period),
        "phase": round6(phase),
    }
    return entity


def ground_entity(half_extent):
    return mesh_entity("Ground", "prim://cube", (0.0, -0.25, 0.0), (half_extent * 2.0, 0.5, half_extent * 2.0))


def scatter(rng, count, half_extent, y):
    """`count` positions on a jittered grid over [-half_extent, half_extent]^2.

    A grid rather than free scatter so density is even: clumps would put the
    cost somewhere the camera might or might not be pointing, and the whole
    point is that the same content is visible every run.
    """
    side = math.ceil(math.sqrt(count))
    step = (half_extent * 2.0) / side
    positions = []
    for index in range(count):
        row, column = divmod(index, side)
        x = -half_extent + step * (column + 0.5) + rng.range(-step * 0.3, step * 0.3)
        z = -half_extent + step * (row + 0.5) + rng.range(-step * 0.3, step * 0.3)
        positions.append((x, y, z))
    return positions


def local_lights(rng, point_count, spot_count, half_extent):
    entities = []
    for i in range(point_count):
        angle = 2.0 * math.pi * i / max(point_count, 1)
        radius = half_extent * 0.65
        entities.append(point_light_entity(
            f"Point_{i}",
            (math.cos(angle) * radius, rng.range(2.5, 5.5), math.sin(angle) * radius),
            (rng.range(0.6, 1.0), rng.range(0.6, 1.0), rng.range(0.6, 1.0)),
            radius=round6(half_extent * 0.55),
            intensity=round6(rng.range(6.0, 12.0)),
        ))
    for i in range(spot_count):
        angle = 2.0 * math.pi * (i + 0.5) / max(spot_count, 1)
        radius = half_extent * 0.35
        entities.append(spot_light_entity(
            f"Spot_{i}",
            (math.cos(angle) * radius, 8.0, math.sin(angle) * radius),
            (rng.range(0.7, 1.0), rng.range(0.7, 1.0), rng.range(0.7, 1.0)),
            radius=round6(half_extent * 0.8),
            intensity=round6(rng.range(15.0, 25.0)),
        ))
    return entities


def movers(rng, primitive, scale, half_extent, count=4):
    entities = []
    for i in range(count):
        angle = 2.0 * math.pi * i / count
        origin = (math.cos(angle) * half_extent * 0.3, 2.0, math.sin(angle) * half_extent * 0.3)
        entities.append(mover_entity(
            f"Mover_{i}", primitive, origin, scale,
            axis=(0.0, 1.0, 0.0) if i % 2 == 0 else (1.0, 0.0, 0.0),
            amplitude=1.5,
            period=3.0 + i * 0.37,   # coprime-ish periods, so the set never resynchronises
            phase=i * 0.25,
        ))
    return entities


# --- Scene definitions -------------------------------------------------------


def build_blank():
    # Deliberately nothing but the camera. This is the pay-for-what-you-place
    # gate: a feature that costs anything here costs it when unused.
    return [camera_entity((0.0, 3.0, 12.0), (0.0, 1.0, 0.0))], []


def build_many_instances(sphere_count=61, cylinder_count=110, cube_count=124, half_extent=20.0,
                         point_count=8, spot_count=4, seed=0x5EED0001):
    """~50k triangles spread thin: many instances, few triangles each.

    Half of the controlled pair. Same triangle budget as the few-instances
    scene, ~7x the draws, casters and instance records — so the difference
    between the two is per-object cost rather than geometry throughput.
    """
    rng = Lcg(seed)
    entities = [camera_entity((0.0, 16.0, 34.0), (0.0, 1.0, 0.0)), sun_entity(), ground_entity(half_extent)]

    for i, position in enumerate(scatter(rng, sphere_count, half_extent * 0.9, 1.0)):
        entities.append(mesh_entity(f"Sphere_{i}", "prim://sphere", position, (1.0, 1.0, 1.0)))
    for i, position in enumerate(scatter(rng, cylinder_count, half_extent * 0.9, 1.0)):
        entities.append(mesh_entity(f"Cylinder_{i}", "prim://cylinder", position, (0.5, 1.0, 0.5)))
    for i, position in enumerate(scatter(rng, cube_count, half_extent * 0.9, 0.5)):
        entities.append(mesh_entity(f"Cube_{i}", "prim://cube", position, (1.0, 1.0, 1.0)))

    entities += movers(rng, "prim://sphere", (1.2, 1.2, 1.2), half_extent)
    entities += local_lights(rng, point_count, spot_count, half_extent)
    return entities, ["Oscillate"]


def build_few_instances(icosphere_count=35, half_extent=20.0, point_count=8, spot_count=4, seed=0x5EED0002):
    """~50k triangles concentrated: few instances, many triangles each."""
    rng = Lcg(seed)
    entities = [camera_entity((0.0, 16.0, 34.0), (0.0, 1.0, 0.0)), sun_entity(), ground_entity(half_extent)]

    for i, position in enumerate(scatter(rng, icosphere_count, half_extent * 0.85, 2.0)):
        entities.append(mesh_entity(f"Icosphere_{i}", "prim://icosphere", position, (2.0, 2.0, 2.0)))

    entities += movers(rng, "prim://icosphere", (2.0, 2.0, 2.0), half_extent)
    entities += local_lights(rng, point_count, spot_count, half_extent)
    return entities, ["Oscillate"]


def build_stress():
    """4x the reference scene's geometry and local lights, as the issue specifies.

    A scaling check rather than a limit test: it answers "does cost rise
    smoothly from the reference point", which is the no-cliff principle.
    """
    return build_many_instances(sphere_count=244, cylinder_count=440, cube_count=496,
                                half_extent=40.0, point_count=32, spot_count=16, seed=0x5EED0003)


def build_geometry_stress(icosphere_count=150, half_extent=45.0, seed=0x5EED0004):
    """Millions of triangles from few instances — the geometry throughput limit.

    Deliberately triangle-dominant rather than draw-dominant: draw count is
    already stressed by PerfStress, and light count by Lights.alvl, so what was
    missing was raw geometry. Not a target, a degradation test.
    """
    rng = Lcg(seed)
    entities = [camera_entity((0.0, 34.0, 72.0), (0.0, 1.0, 0.0)), sun_entity(), ground_entity(half_extent)]
    for i, position in enumerate(scatter(rng, icosphere_count, half_extent * 0.85, 3.0)):
        entities.append(mesh_entity(f"Icosphere_{i}", "prim://icosphere-high", position, (3.0, 3.0, 3.0)))
    entities += local_lights(rng, 8, 4, half_extent)
    return entities, []


SCENES = {
    "PerfBlank": build_blank,
    "PerfReferenceManyInstances": build_many_instances,
    "PerfReferenceFewInstances": build_few_instances,
    "PerfStress": build_stress,
    "PerfGeometryStress": build_geometry_stress,
}


def summarise(entities):
    triangles = 0
    meshes = 0
    counts = {"point": 0, "spot": 0, "directional": 0, "movers": 0}
    for entity in entities:
        components = entity["components"]
        if "MeshRenderer" in components:
            meshes += 1
            triangles += PRIMITIVES[components["MeshRenderer"]["mesh"]["path"]][1]
        if "PointLight" in components:
            counts["point"] += 1
        if "SpotLight" in components:
            counts["spot"] += 1
        if "DirectionalLight" in components:
            counts["directional"] += 1
        if "Oscillator" in components:
            counts["movers"] += 1
    return triangles, meshes, counts


def write_scene(name, entities, systems):
    level_path = LEVELS / f"{name}.alvl"
    level = {"entities": entities, "systems": systems, "version": 2}
    level_path.write_text(json.dumps(level, indent=2, sort_keys=True) + "\n")

    # The sidecar carries the level's stable guid. Minted once and preserved on
    # regeneration: a new guid every run would repoint every reference to this
    # level at something the database has never seen.
    sidecar_path = LEVELS / f"{name}.alvl.aast"
    if sidecar_path.exists():
        guid = json.loads(sidecar_path.read_text())["guid"]
    else:
        guid = str(uuid.uuid4())
    sidecar_path.write_text(json.dumps({"guid": guid, "type": "AssetSidecar", "version": 1}, indent=2))
    return level_path


def main():
    print(f"{'scene':<30} {'entities':>9} {'meshes':>7} {'triangles':>10}  lights")
    for name, build in SCENES.items():
        entities, systems = build()
        path = write_scene(name, entities, systems)
        triangles, meshes, counts = summarise(entities)
        lights = f"{counts['point']}pt {counts['spot']}spot {counts['directional']}dir, {counts['movers']} movers"
        print(f"{name:<30} {len(entities):>9} {meshes:>7} {triangles:>10}  {lights}"
              f"   ({path.stat().st_size / 1024:.0f} KB)")


if __name__ == "__main__":
    main()
