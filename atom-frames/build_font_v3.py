#!/usr/bin/env python3
"""Build the v3 atom-spinner: 120 SVG frames + Spinner3.ttf, from one set of math.
(v3 = v2 at higher frame density: 120 frames / 2 s loop = 60 fps, same motion.)

The MOTION math is a direct port of atom-frames/sketch-v2.js (orbital / Kepler-
style speed, three-phase stagger, growing particles, motion stretch, and the
whole-atom spin+shrink window). See that file for the full narrative; the short
version:

  * Kepler schedule. Each particle's eccentric angle advances at a rate
    proportional to 1 / r^KEPLER_EXP, so it dwells at the far tips and dives fast
    past the nucleus. The schedule is integrated once into a tau -> t table and
    sampled per frame.
  * Three-phase stagger. The three particles ride the SAME schedule, offset in
    loop-time by k/3, so the six "at a far tip" moments are evenly spaced.
  * Growing particles. Orbits are a fixed size; each particle grows with its
    distance from the nucleus (PARTICLE_GROWTH at a far tip).
  * Motion stretch. Each particle is an ellipse elongated ALONG its travel in
    proportion to speed (STRETCH at the fast closest approach, none at the tips).
  * Whole-atom spin. During [SPIN_START_MS, SPIN_END_MS] the whole atom eases
    through a full 360 and shrinks (SPIN_SHRINK) at peak spin speed; still
    otherwise. A full turn lands back at the original orientation, so it loops.

What is DROPPED versus the p5 sketch, and why:
  the p5 version stacks translucent, Gaussian-blurred ghost copies of the atom
  and of each proton to fake motion blur. A glyf font outline is a hard binary
  mask -- no alpha, no blur -- so those effects can't be baked. We render only
  the sharp lead copy. Everything else (including the stretch and the spin) IS
  baked. The SVG frames match the font glyphs 1:1 for the same reason.

Baking (identical strategy to build_font.py):
    - orbit stroke   = ellipse centerline buffered by STROKE/2       (annulus)
    - clear ring     = subtract each (proton + HALO_GAP) stretched
                       ellipse from the orbit annuli
    - nucleus        = filled disk
    - protons        = filled stretched, rotated ellipses (the sharp lead copy)
    - whole atom     = the union, rotated by `spin` and scaled by `atomScale`
                       about the center C, per the spin window
  then transformed into the em with a safe MARGIN and written as TrueType
  contours with correct winding.

Run with a python that has fontTools + shapely.
"""
import math
import os

from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
from shapely import affinity
from shapely.geometry import LineString, Point, Polygon
from shapely.ops import unary_union

# --- spinner geometry (0..SIZE, y-down; same space as the SVG/font) ------------
SIZE = 512
C = SIZE / 2.0                 # center (256)
A = 200.0                      # orbit semi-major axis
B = 96.0                       # orbit semi-minor axis
NUCLEUS_R = 50.0
ELECTRON_R = 18.0
STROKE = 13.0                  # orbit line thickness
HALO_GAP = 10.0                # width of the clear ring around each particle

ORBITS = [0.0, 60.0, 120.0]    # orbit rotations in degrees (orbit k <-> particle k)

# --- v2 motion tunables (mirror sketch-v2.js) ----------------------------------
KEPLER_EXP = 2.2               # how strongly speed varies with distance
PHASE_STEP = 1.0 / 3.0         # loop-time offset between particles (three-phase)
PARTICLE_GROWTH = 0.5          # particle radius gain from closest (0) to farthest
STRETCH = 1.8                  # motion-blur elongation along travel at top speed

# --- timing / spin window (mirror sketch-v2.js) --------------------------------
N_FRAMES = 120                 # v3: higher fidelity; 120 frames / 2 s LOOP_MS = 60 fps
LOOP_MS = 2000                 # one full loop of the internal particle motion

# Whole-atom rotation: WINDOWED spin like sketch-v2.js -- the atom sits still, whips
# through a full 360 while shrinking in the middle of the loop, then goes still again.
# (Set True only to force a constant 360/loop with no shrink.)
CONTINUOUS_SPIN = False

SPIN_START_MS = 250            # 360 spin runs across this window;
SPIN_END_MS = 1750             # outside it the atom is perfectly still.
SPIN_EASE = 5.0                # ease-in-out steepness of the spin within its window
SPIN_SHRINK = 0.3              # atom shrinks to (1-this) at peak spin speed
# (These are the exact sketch-v2.js values. render_webp_v2 imports them, so the WebP
#  and the TTF run the SAME motion math -- the only thing the font can't reproduce is
#  the WebP's motion blur, which is a rendering feature, not part of the math.)

# --- motion trails --------------------------------------------------------------
# The WebP fakes motion blur by stacking translucent, blurred copies of the atom (and
# of each proton) that lag behind the leading one. A font glyph has no alpha and no
# blur, so baking the lag as SOLID trailing copies reads as ugly duplicate rings/blobs
# rather than a smear. So the font disables both trail systems (TRAILS = 0) -- every
# frame is a single crisp atom. The WebP keeps its motion blur (it doesn't use these).
GHOST_TRAILS = 0               # whole-atom copies trailing the lead in rotation (0 = none)
GHOST_LAG = 0.18               # radians each whole-atom copy trails, at peak spin speed
PROTON_TRAILS = 0              # copies each proton trails back along its own travel (0 = none)
PROTON_LAG = 12.0              # px each proton copy trails, at top proton speed

# No alpha in a font, so instead of FADING the trailing copies we TAPER them: each
# successive ghost atom is drawn with a thinner orbit stroke and smaller protons. One
# entry per copy: index 0 = lead (full weight), 1 = 2nd atom, 2 = 3rd atom. Lengths
# must be >= GHOST_TRAILS + 1.
TRAIL_STROKE_SCALE = [1.0, 0.55, 0.35]    # orbit stroke width, per ghost copy
TRAIL_PROTON_SCALE = [1.0, 0.6, 0.4]      # proton radius, per ghost copy

# --- font layout (mirror build_font.py) ----------------------------------------
UPM = 1024                     # units per em
MARGIN = 96                    # keep all ink at least this far from every em edge
SCALE = (UPM - 2 * MARGIN) / float(SIZE)
ELLIPSE_PTS = 160              # centerline resolution for the orbits
CIRCLE_QS = 32                 # quad segments per quarter turn for disk buffers
ELLIPSE_POLY_PTS = 96          # vertices for a proton/halo stretched ellipse
FAMILY = "Spinner3"

# --- Kepler schedule lookup table: tau (loop fraction) -> eccentric angle t -----
TABLE_N = 2048

HERE = os.path.dirname(os.path.abspath(__file__))
SVG_DIR = os.path.join(HERE, "frames-v3")
TTF_OUT = os.path.join(HERE, "Spinner3.ttf")

_tau_to_t = []
_speed_min = 0.0
_speed_max = 1.0


def r_base(t):
    """Distance from center on the base (unpulsed) ellipse at eccentric angle t."""
    x = A * math.cos(t)
    y = B * math.sin(t)
    return math.hypot(x, y)


def build_schedule():
    """Integrate the Kepler 'time cost' r^KEPLER_EXP and invert to tau -> t."""
    global _tau_to_t
    M = 4096
    dt = (2 * math.pi) / M
    c_samples = [0.0] * (M + 1)
    c = 0.0
    for i in range(M):
        t = i * dt
        w = 0.5 * (r_base(t) ** KEPLER_EXP + r_base(t + dt) ** KEPLER_EXP)
        c += w * dt
        c_samples[i + 1] = c
    total = c_samples[M]

    _tau_to_t = [0.0] * TABLE_N
    j = 0
    for i in range(TABLE_N):
        target = (i / TABLE_N) * total
        while j < M and c_samples[j + 1] < target:
            j += 1
        c0 = c_samples[j]
        c1 = c_samples[j + 1]
        frac = (target - c0) / (c1 - c0) if c1 > c0 else 0.0
        _tau_to_t[i] = (j + frac) * dt


def eccentric_angle(tau):
    """Map a loop fraction (wrapped to [0,1)) to the scheduled eccentric angle t."""
    f = tau - math.floor(tau)
    x = f * TABLE_N
    i = int(math.floor(x))
    frac = x - i
    a = _tau_to_t[i % TABLE_N]
    b = _tau_to_t[(i + 1) % TABLE_N]
    if i + 1 >= TABLE_N:
        b += 2 * math.pi                      # continue past 2*pi across the wrap
    return a + (b - a) * frac


def local_vel(tau):
    """Velocity in the particle's own (unrotated) orbit frame, via finite diff."""
    h = 1e-4
    t0 = eccentric_angle(tau)
    t1 = eccentric_angle(tau + h)
    vx = (A * math.cos(t1) - A * math.cos(t0)) / h
    vy = (B * math.sin(t1) - B * math.sin(t0)) / h
    return vx, vy


def measure_speeds():
    """Fill the loop-wide speed range used to normalize the stretch."""
    global _speed_min, _speed_max
    N = 1024
    lo = math.inf
    hi = 0.0
    for i in range(N):
        vx, vy = local_vel(i / N)
        sp = math.hypot(vx, vy)
        lo = min(lo, sp)
        hi = max(hi, sp)
    _speed_min = lo
    _speed_max = hi


def orbit_state(k, tau):
    """Particle k's screen position, travel angle, and stretched semi-axes."""
    p = tau + k * PHASE_STEP
    t = eccentric_angle(p)
    u = (r_base(t) - B) / (A - B)                      # 0 at closest, 1 at farthest
    r = ELECTRON_R * (1.0 + PARTICLE_GROWTH * u)       # base particle radius

    th = math.radians(60.0 * k)
    ct, st = math.cos(th), math.sin(th)
    lx = A * math.cos(t)
    ly = B * math.sin(t)
    x = C + ct * lx - st * ly
    y = C + st * lx + ct * ly

    vx, vy = local_vel(p)
    speed = math.hypot(vx, vy)
    s_n = (speed - _speed_min) / max(_speed_max - _speed_min, 1e-9)

    angle = math.atan2(st * vx + ct * vy, ct * vx - st * vy)   # travel dir, screen
    along = r * (1.0 + STRETCH * s_n)                  # semi-axis along travel
    perp = r                                           # semi-axis across travel
    # speedN drives the motion-blur trails in the raster (WebP) renderer; the font
    # bake ignores it.
    return {"x": x, "y": y, "deg": 60.0 * k, "angle": angle,
            "along": along, "perp": perp, "speedN": s_n}


def spin_ease(w):
    """Symmetric power ease-in-out on [0,1] (SPIN_EASE steepness)."""
    if w < 0.5:
        return 0.5 * (2.0 * w) ** SPIN_EASE
    return 1.0 - 0.5 * (2.0 * (1.0 - w)) ** SPIN_EASE


def spin_speed_norm(w):
    """Normalized angular speed of the eased spin: 0 at ends, 1 at the middle."""
    e = 2.0 * w if w < 0.5 else 2.0 * (1.0 - w)
    return e ** (SPIN_EASE - 1.0)


def spin_for_frame(f):
    """(spin radians, atom scale) at frame f.

    CONTINUOUS_SPIN: uniform 360/loop, no shrink -> seamless endless loop.
    Otherwise: the exact sketch-v2.js windowed spin (still -> whip -> still)."""
    if CONTINUOUS_SPIN:
        return (f / N_FRAMES) * 2 * math.pi, 1.0
    tms = (f / N_FRAMES) * LOOP_MS
    if SPIN_START_MS <= tms <= SPIN_END_MS:
        w = (tms - SPIN_START_MS) / (SPIN_END_MS - SPIN_START_MS)
        spin = spin_ease(w) * 2 * math.pi
        atom_scale = 1.0 - SPIN_SHRINK * spin_speed_norm(w)
        return spin, atom_scale
    return 0.0, 1.0


def spin_speed_for_frame(f):
    """Normalized whole-atom spin speed (0..1) at frame f -- drives the rotational
    trail length, so the ghost copies fan out only while the atom is whipping and
    collapse onto the lead when it is still."""
    if CONTINUOUS_SPIN:
        return 1.0
    tms = (f / N_FRAMES) * LOOP_MS
    if SPIN_START_MS <= tms <= SPIN_END_MS:
        w = (tms - SPIN_START_MS) / (SPIN_END_MS - SPIN_START_MS)
        return spin_speed_norm(w)
    return 0.0


def orbit_annulus(deg, rx, ry, stroke=STROKE):
    """Filled stroke of one orbit ellipse: centerline buffered by stroke/2."""
    a = math.radians(deg)
    ca, sa = math.cos(a), math.sin(a)
    pts = []
    for i in range(ELLIPSE_PTS):
        t = 2 * math.pi * i / ELLIPSE_PTS
        px, py = rx * math.cos(t), ry * math.sin(t)
        pts.append((C + ca * px - sa * py, C + sa * px + ca * py))
    pts.append(pts[0])
    return LineString(pts).buffer(stroke / 2.0, quad_segs=16)


def stretched_ellipse(cx, cy, along, perp, angle):
    """A rotated ellipse polygon (semi-axes along/perp), matching p5's proton."""
    ca, sa = math.cos(angle), math.sin(angle)
    pts = []
    for i in range(ELLIPSE_POLY_PTS):
        t = 2 * math.pi * i / ELLIPSE_POLY_PTS
        ex, ey = along * math.cos(t), perp * math.sin(t)
        pts.append((cx + ca * ex - sa * ey, cy + sa * ex + ca * ey))
    return Polygon(pts)


def build_atom(orbits, stroke, proton_scale):
    """One atom (orbits + clear rings + nucleus + protons-with-trails), at the given
    orbit stroke width and proton-radius scale. Used once per ghost copy so trailing
    copies can be drawn thinner (the no-alpha stand-in for fading)."""
    orbit_ring = unary_union([orbit_annulus(o["deg"], A, B, stroke) for o in orbits])

    # protons: lead ellipse + PROTON_TRAILS copies trailing back along each proton's
    # travel (scaled by its speed), all at this copy's proton radius.
    proton_parts = []
    halo_parts = []
    for o in orbits:
        along, perp = o["along"] * proton_scale, o["perp"] * proton_scale
        halo_parts.append(stretched_ellipse(o["x"], o["y"], along + HALO_GAP, perp + HALO_GAP, o["angle"]))
        dirx, diry = math.cos(o["angle"]), math.sin(o["angle"])
        for i in range(PROTON_TRAILS + 1):
            lag = i * PROTON_LAG * o["speedN"]
            proton_parts.append(stretched_ellipse(
                o["x"] - dirx * lag, o["y"] - diry * lag, along, perp, o["angle"]))

    orbits_gapped = orbit_ring.difference(unary_union(halo_parts))   # clear ring cut in
    nucleus = Point(C, C).buffer(NUCLEUS_R, quad_segs=CIRCLE_QS)
    return unary_union([orbits_gapped, nucleus, unary_union(proton_parts)]).buffer(0)


_geom_cache = {}


def frame_geometry(f):
    """Full baked geometry of frame f, in SVG coordinates (0..512, y-down)."""
    if f in _geom_cache:
        return _geom_cache[f]
    tau = f / N_FRAMES
    orbits = [orbit_state(k, tau) for k in range(3)]

    spin, atom_scale = spin_for_frame(f)
    ghost_lag = GHOST_LAG * spin_speed_for_frame(f)

    # Whole-atom spin with GHOST_TRAILS trailing copies. Each copy is built thinner than
    # the last (TRAIL_STROKE_SCALE / TRAIL_PROTON_SCALE) and rotated a little further
    # back, then all are unioned -- the WebP's rotational ghost stack, baked opaque and
    # tapered instead of faded. When still (ghost_lag ~ 0) only the full-weight lead is
    # drawn, so the trail collapses to one crisp atom.
    n_copies = (GHOST_TRAILS + 1) if ghost_lag > 1e-6 else 1
    copies = []
    for i in range(n_copies):
        a = build_atom(orbits, STROKE * TRAIL_STROKE_SCALE[i], TRAIL_PROTON_SCALE[i])
        if atom_scale != 1.0:
            a = affinity.scale(a, xfact=atom_scale, yfact=atom_scale, origin=(C, C))
        if spin != 0.0 or i > 0:
            a = affinity.rotate(a, math.degrees(spin - i * ghost_lag), origin=(C, C))
        copies.append(a)

    atom = unary_union(copies).buffer(0)
    _geom_cache[f] = atom
    return atom


# --- SVG output ----------------------------------------------------------------

def polygons(geom):
    if geom.is_empty:
        return []
    if geom.geom_type == "Polygon":
        return [geom]
    return [g for g in geom.geoms if g.geom_type == "Polygon"]


def _svg_path(geom):
    """One SVG path 'd' string (evenodd) for the whole baked geometry."""
    parts = []
    for poly in polygons(geom):
        rings = [list(poly.exterior.coords)] + [list(r.coords) for r in poly.interiors]
        for ring in rings:
            pts = ring[:-1] if len(ring) > 1 and ring[0] == ring[-1] else ring
            if len(pts) < 3:
                continue
            d = "M {:.2f} {:.2f} ".format(pts[0][0], pts[0][1])
            d += " ".join("L {:.2f} {:.2f}".format(x, y) for x, y in pts[1:])
            d += " Z"
            parts.append(d)
    return " ".join(parts)


def write_svg(f):
    d = _svg_path(frame_geometry(f))
    svg = (
        '<svg xmlns="http://www.w3.org/2000/svg" width="{s}" height="{s}" '
        'viewBox="0 0 {s} {s}">\n'
        '  <rect width="{s}" height="{s}" fill="#111111"/>\n'
        '  <path d="{d}" fill="#ffffff" fill-rule="evenodd"/>\n'
        '</svg>\n'
    ).format(s=SIZE, d=d)
    path = os.path.join(SVG_DIR, "spinner{}.svg".format(f))
    with open(path, "w") as fh:
        fh.write(svg)


def write_svgs():
    os.makedirs(SVG_DIR, exist_ok=True)
    for f in range(N_FRAMES):
        write_svg(f)
    print("wrote {} SVG frames -> {}/spinner0.svg .. spinner{}.svg".format(
        N_FRAMES, SVG_DIR, N_FRAMES - 1))


# --- TTF output (same winding/margin logic as build_font.py) -------------------

def to_font(x, y):
    """SVG (y-down) -> font (y-up), centered with MARGIN."""
    return (round(MARGIN + x * SCALE), round((UPM - MARGIN) - y * SCALE))


def signed_area(pts):
    a = 0.0
    n = len(pts)
    for i in range(n):
        x0, y0 = pts[i]
        x1, y1 = pts[(i + 1) % n]
        a += x0 * y1 - x1 * y0
    return a / 2.0


def emit_ring(pen, coords, exterior, xmin_box):
    pts = [to_font(x, y) for x, y in coords[:-1]]
    if len(pts) < 3:
        return
    # TrueType: exterior clockwise (area<0), holes counter-clockwise (area>0).
    is_ccw = signed_area(pts) > 0
    want_ccw = not exterior
    if is_ccw != want_ccw:
        pts = pts[::-1]
    pen.moveTo(pts[0])
    for p in pts[1:]:
        pen.lineTo(p)
    pen.closePath()
    xmin_box[0] = min(xmin_box[0], min(p[0] for p in pts))


def build_glyph(f):
    pen = TTGlyphPen(None)
    xmin_box = [UPM]
    for poly in polygons(frame_geometry(f)):
        emit_ring(pen, list(poly.exterior.coords), True, xmin_box)
        for ring in poly.interiors:
            emit_ring(pen, list(ring.coords), False, xmin_box)
    return pen.glyph(), (xmin_box[0] if xmin_box[0] < UPM else 0)


def build_ttf():
    order = [".notdef"] + ["atom{}".format(i) for i in range(N_FRAMES)]
    glyphs = {".notdef": TTGlyphPen(None).glyph()}
    metrics = {".notdef": (UPM, 0)}
    for i in range(N_FRAMES):
        glyph, xmin = build_glyph(i)
        glyphs["atom{}".format(i)] = glyph
        metrics["atom{}".format(i)] = (UPM, xmin)   # advance = full em, lsb = ink xMin

    fb = FontBuilder(UPM, isTTF=True)
    fb.setupGlyphOrder(order)
    # map to the Private Use Area (F000+), the layout IcoMoon expects
    fb.setupCharacterMap({0xF000 + i: "atom{}".format(i) for i in range(N_FRAMES)})
    fb.setupGlyf(glyphs)
    fb.setupHorizontalMetrics(metrics)
    fb.setupHorizontalHeader(ascent=UPM, descent=0)
    fb.setupNameTable({"familyName": FAMILY, "styleName": "Regular"})
    fb.setupOS2(sTypoAscender=UPM, sTypoDescender=0, sTypoLineGap=0,
                usWinAscent=UPM, usWinDescent=0)
    fb.setupPost()
    fb.save(TTF_OUT)
    print("wrote {}  ({} frames, UPM={}, margin={})".format(
        TTF_OUT, N_FRAMES, UPM, MARGIN))


def main():
    build_schedule()
    measure_speeds()
    write_svgs()
    build_ttf()


if __name__ == "__main__":
    main()
