#!/usr/bin/env python3
"""Build the atom-spinner Spinner.ttf directly from geometry (self-contained).

The spinner is an atom: a solid nucleus, three orbit ellipses at 0/60/120 deg,
and three electrons that travel ALONG the orbits while the orbits breathe.

Motion / balance:
  Electron k rides orbit k (rotated 60*k deg) at parameter t_k = tau(phi)+180*k deg.
  The 180*k offset makes sum_k position_k == 0 for every phi (mass stays exactly
  centered -> no wobble). All orbits pulse by the SAME factor, which just scales
  that balanced set, so the breathing never unbalances it. tau() couples the
  electron to the pulse: it sits at a major-axis tip at max size and a minor-axis
  tip at min size, eased so it lingers at the ends.

Why a dedicated builder instead of importing the SVGs into a font tool:
  the SVGs use stroked ellipses and a mask (the clear ring around each electron).
  Those must be BAKED into filled outlines for a glyf font, so we rebuild the
  geometry with shapely:
    - orbit stroke = ellipse centerline buffered by STROKE/2  (an annulus)
    - clear ring   = subtract (electron + HALO_GAP) disks from the orbit annuli
    - nucleus      = filled disk
    - electrons    = filled disks (fill the center of each clear ring)
  Everything is unioned, transformed into the em with a safe MARGIN on all sides
  (this is what fixes IcoMoon's right-edge clipping: ink now stays >= MARGIN from
  every em edge instead of running to 8 units of it), and written as TrueType
  contours with correct winding.

Run with a python that has fontTools + shapely.
"""
import math
import os

from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
from shapely.geometry import LineString, Point
from shapely.ops import unary_union

# --- spinner geometry (SVG coordinate space, 0..SIZE, y-down) ------------------
SIZE = 512
C = SIZE / 2.0               # center (256)
A = 200.0                    # orbit semi-major axis
B = 96.0                     # orbit semi-minor axis (keeps electrons clear of nucleus)
NUCLEUS_R = 50.0
ELECTRON_R = 24.0
STROKE = 13.0                # orbit line thickness
HALO_GAP = 10.0              # width of the clear ring around each electron

N_FRAMES = 48
PULSE_AMP = 0.14             # orbits scale between (1-amp) and (1+amp)
PULSE_CYCLES = 2             # breaths per loop; MUST be even (motion is coupled to it)
EASE_STRENGTH = 0.7          # 0 = ellipse-only easing; 1 = electron stalls at each tip
ORBITS = [0.0, 60.0, 120.0]  # orbit rotations in degrees

# --- font layout ---------------------------------------------------------------
UPM = 1024                   # units per em
MARGIN = 96                  # keep all ink at least this far from every em edge
SCALE = (UPM - 2 * MARGIN) / float(SIZE)
ELLIPSE_PTS = 160            # centerline resolution for the orbits
CIRCLE_QS = 32               # quad segments per quarter turn for disk buffers
FAMILY = "Spinner"

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.normpath(os.path.join(HERE, "..", "assets", "editor", "loading", "Spinner.ttf"))


def pulse(phi):
    """Synchronized breathing factor, same for every orbit (preserves balance)."""
    return 1.0 + PULSE_AMP * math.sin(PULSE_CYCLES * phi)


def _ease(u):
    """Ease-in-out on [0,1], blended with linear by EASE_STRENGTH (endpoints exact)."""
    smooth = u * u * (3.0 - 2.0 * u)
    return (1.0 - EASE_STRENGTH) * u + EASE_STRENGTH * smooth


def tau(phi):
    """Pulse-coupled orbit-parameter phase (see module docstring)."""
    psi = PULSE_CYCLES * phi
    chi = psi - math.pi / 2.0            # chi = 0 at the first major-axis tip
    seg = math.floor(chi / math.pi)      # which tip-to-tip segment
    u = chi / math.pi - seg              # progress within the segment, in [0,1)
    return seg * (math.pi / 2.0) + (math.pi / 2.0) * _ease(u)


def electron_pos(k, phi, s):
    """Screen position of electron k on its (pulsed) orbit."""
    th = math.radians(60.0 * k)
    t = tau(phi) + math.radians(180.0 * k)
    lx = A * s * math.cos(t)
    ly = B * s * math.sin(t)
    x = C + math.cos(th) * lx - math.sin(th) * ly
    y = C + math.sin(th) * lx + math.cos(th) * ly
    return x, y


def orbit_annulus(deg, rx, ry):
    """Filled stroke of one orbit ellipse: centerline buffered by STROKE/2."""
    a = math.radians(deg)
    ca, sa = math.cos(a), math.sin(a)
    pts = []
    for i in range(ELLIPSE_PTS):
        t = 2 * math.pi * i / ELLIPSE_PTS
        px, py = rx * math.cos(t), ry * math.sin(t)
        pts.append((C + ca * px - sa * py, C + sa * px + ca * py))
    pts.append(pts[0])
    return LineString(pts).buffer(STROKE / 2.0, quad_segs=16)


def frame_geometry(f):
    """Full baked geometry of frame f, in SVG coordinates (0..512, y-down)."""
    phi = 2 * math.pi * f / N_FRAMES
    s = pulse(phi)
    rx, ry = A * s, B * s
    elec = [electron_pos(k, phi, s) for k in range(3)]

    orbits = unary_union([orbit_annulus(deg, rx, ry) for deg in ORBITS])
    halos = unary_union([Point(x, y).buffer(ELECTRON_R + HALO_GAP, quad_segs=CIRCLE_QS)
                         for x, y in elec])
    orbits_gapped = orbits.difference(halos)          # clear ring cut into the orbits
    nucleus = Point(C, C).buffer(NUCLEUS_R, quad_segs=CIRCLE_QS)
    electrons = unary_union([Point(x, y).buffer(ELECTRON_R, quad_segs=CIRCLE_QS)
                             for x, y in elec])
    return unary_union([orbits_gapped, nucleus, electrons]).buffer(0)


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


def polygons(geom):
    if geom.is_empty:
        return []
    if geom.geom_type == "Polygon":
        return [geom]
    return [gg for gg in geom.geoms if gg.geom_type == "Polygon"]


def build_glyph(f):
    pen = TTGlyphPen(None)
    xmin_box = [UPM]
    for poly in polygons(frame_geometry(f)):
        emit_ring(pen, list(poly.exterior.coords), True, xmin_box)
        for ring in poly.interiors:
            emit_ring(pen, list(ring.coords), False, xmin_box)
    return pen.glyph(), (xmin_box[0] if xmin_box[0] < UPM else 0)


def main():
    order = [".notdef"] + [f"atom{i}" for i in range(N_FRAMES)]
    glyphs = {".notdef": TTGlyphPen(None).glyph()}
    metrics = {".notdef": (UPM, 0)}
    for i in range(N_FRAMES):
        glyph, xmin = build_glyph(i)
        glyphs[f"atom{i}"] = glyph
        metrics[f"atom{i}"] = (UPM, xmin)      # advance = full em, lsb = ink xMin

    fb = FontBuilder(UPM, isTTF=True)
    fb.setupGlyphOrder(order)
    # map to the Private Use Area (F000+), the layout IcoMoon expects
    fb.setupCharacterMap({0xF000 + i: f"atom{i}" for i in range(N_FRAMES)})
    fb.setupGlyf(glyphs)
    fb.setupHorizontalMetrics(metrics)
    fb.setupHorizontalHeader(ascent=UPM, descent=0)
    fb.setupNameTable({"familyName": FAMILY, "styleName": "Regular"})
    fb.setupOS2(sTypoAscender=UPM, sTypoDescender=0, sTypoLineGap=0,
                usWinAscent=UPM, usWinDescent=0)
    fb.setupPost()
    fb.save(OUT)
    print(f"wrote {OUT}  ({N_FRAMES} frames, UPM={UPM}, margin={MARGIN})")


if __name__ == "__main__":
    main()
