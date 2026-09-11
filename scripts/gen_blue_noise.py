#!/usr/bin/env python3
"""Generates the blue-noise tile the shadow filters rotate their disks by.

    python3 scripts/gen_blue_noise.py

writes modules/Render/src/BlueNoiseTile.inl, which is committed. Run it again
only to change the tile's size or the energy kernel below.

Void-and-cluster: every texel gets a rank, and the ranks are placed
so that each prefix of them is as evenly spread as it can be. Neighbouring
texels therefore hold very different values, which is what lets a spatial
filter rotate by them without printing a pattern, and what makes the residue
look like fine grain rather than the blotches white noise leaves. No random
source after the initial pattern, and that is seeded by a fixed LCG, so a
regeneration is byte-identical on any machine.

Pure Python on purpose: the tile is generated once and checked in, and nothing
that runs once should add a dependency.
"""

import math
import pathlib

REPO = pathlib.Path(__file__).resolve().parent.parent
OUTPUT = REPO / "modules" / "Render" / "src" / "BlueNoiseTile.inl"

# Must match Render::kBlueNoiseTileSize.
SIZE = 64
COUNT = SIZE * SIZE

# Width of the Gaussian each rank repels its neighbours with, in texels. 1.5 is
# the value the method is usually run at; wider blurs the spectrum's low end,
# narrower lets clumps form between the kernels' reach.
SIGMA = 1.5
# The kernel is cut off where it has fallen below a thousandth, which keeps each
# update to a small window instead of the whole tile.
WINDOW = int(math.ceil(SIGMA * math.sqrt(2.0 * math.log(1000.0))))

# Fraction of the tile the initial pattern marks: sparse enough for the
# relaxation to settle, dense enough to have something to relax.
INITIAL_FRACTION = 0.1

# Eight-bit output: each value holds COUNT / 256 ranks.
LEVELS = 256

# A fixed linear congruential generator, so the initial pattern never depends
# on Python's own random module.
LCG_MULTIPLIER = 1664525
LCG_INCREMENT = 1013904223
LCG_MODULUS = 2 ** 32
LCG_SEED = 0x5EED


def kernel():
    """Offsets inside the window and the Gaussian weight at each."""
    taps = []
    for dy in range(-WINDOW, WINDOW + 1):
        for dx in range(-WINDOW, WINDOW + 1):
            taps.append((dx, dy, math.exp(-(dx * dx + dy * dy) / (2.0 * SIGMA * SIGMA))))
    return taps


KERNEL = kernel()


def splat(energy, index, sign):
    """Add (or remove) one point's kernel to the energy field, wrapping at the edges."""
    x, y = index % SIZE, index // SIZE
    for dx, dy, weight in KERNEL:
        energy[((y + dy) % SIZE) * SIZE + (x + dx) % SIZE] += sign * weight


def field(points):
    energy = [0.0] * COUNT
    for index, on in enumerate(points):
        if on:
            splat(energy, index, 1.0)
    return energy


def tightest_cluster(points, energy):
    return max((i for i in range(COUNT) if points[i]), key=lambda i: energy[i])


def largest_void(points, energy):
    return min((i for i in range(COUNT) if not points[i]), key=lambda i: energy[i])


def initial_pattern():
    state = LCG_SEED
    points = [False] * COUNT
    placed = 0
    target = int(COUNT * INITIAL_FRACTION)
    while placed < target:
        state = (LCG_MULTIPLIER * state + LCG_INCREMENT) % LCG_MODULUS
        index = state % COUNT
        if not points[index]:
            points[index] = True
            placed += 1

    # Relax: move the most crowded point into the emptiest gap until the move
    # would put it straight back.
    energy = field(points)
    while True:
        cluster = tightest_cluster(points, energy)
        points[cluster] = False
        splat(energy, cluster, -1.0)
        void = largest_void(points, energy)
        if void == cluster:
            points[cluster] = True
            splat(energy, cluster, 1.0)
            return points
        points[void] = True
        splat(energy, void, 1.0)


def ranks():
    prototype = initial_pattern()
    ones = sum(prototype)
    rank = [0] * COUNT

    # Phase one: the initial points, ranked from the most crowded down.
    points = list(prototype)
    energy = field(points)
    for r in range(ones - 1, -1, -1):
        cluster = tightest_cluster(points, energy)
        points[cluster] = False
        splat(energy, cluster, -1.0)
        rank[cluster] = r

    # Phase two: fill the emptiest gaps up to half the tile.
    points = list(prototype)
    energy = field(points)
    for r in range(ones, COUNT // 2):
        void = largest_void(points, energy)
        points[void] = True
        splat(energy, void, 1.0)
        rank[void] = r

    # Phase three: past half, the zeros are the minority, so the emptiest gap is
    # found as the tightest cluster of what is still empty.
    empty = [not p for p in points]
    energy = field(empty)
    for r in range(COUNT // 2, COUNT):
        cluster = tightest_cluster(empty, energy)
        empty[cluster] = False
        splat(energy, cluster, -1.0)
        rank[cluster] = r
    return rank


def neighbour_difference(values):
    """Mean absolute difference between each texel and its right and lower
    neighbours, wrapping, with values scaled to [0, 1]."""
    total = 0.0
    for y in range(SIZE):
        for x in range(SIZE):
            here = values[y * SIZE + x]
            total += abs(here - values[y * SIZE + (x + 1) % SIZE])
            total += abs(here - values[((y + 1) % SIZE) * SIZE + x])
    return total / (2 * COUNT * (LEVELS - 1))


def main():
    values = [r * LEVELS // COUNT for r in ranks()]
    lines = [
        "// Generated by scripts/gen_blue_noise.py. Do not edit; regenerate.",
        f"// {SIZE}x{SIZE} void-and-cluster ranks, row by row, as 8-bit values.",
    ]
    for row in range(0, COUNT, 16):
        lines.append(", ".join(f"{v:3d}" for v in values[row:row + 16]) + ",")
    OUTPUT.write_text("\n".join(lines) + "\n")
    print(f"wrote {OUTPUT.relative_to(REPO)}")
    print(f"mean 4-neighbour difference: {neighbour_difference(values):.4f} (white noise: 0.3333)")


if __name__ == "__main__":
    main()
