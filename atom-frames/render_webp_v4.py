#!/usr/bin/env python3
"""Render the v4 atom-spinner to an animated, motion-BLURRED WebP.

v4 = a 10 s loop: the protons orbit at the v3 tempo for ORBIT_CYCLES cycles, then the
whole atom whips through its single 360 spin in the last 1500 ms. All that timing math
lives in build_font_v4; this file only rasterizes it. Three frame densities are built,
selected by CLI arg (matching the TTF variants):

    python render_webp_v4.py 15 | 30 | 60     -> atom-spinner-v4-NN.webp
    (no arg -> all three)

This is the raster counterpart to build_font_v4.py. Where the font bake must drop
the blur (a glyf outline is a hard binary mask -- no alpha, no blur), a raster
frame can carry the full sketch-v2.js look, so this renderer reproduces it:

  * the whole atom is stacked as 3 translucent copies, each lagging further back
    in rotation and blurrier than the last (a rotational motion trail), scaled by
    the spin speed so the copies collapse to one crisp atom when the atom is still;
  * each proton is likewise stacked as 3 copies that trail back along its own
    travel direction and blur, scaled by that proton's speed -- so a proton smears
    as it whips past the nucleus and is crisp out at the slow tips;
  * the clear ring around each proton is punched as real transparency so the copies
    composite cleanly when blurred.

All the MOTION math (Kepler schedule, three-phase stagger, growth, stretch, and the
whole-atom spin+shrink window) is imported verbatim from build_font_v2 so the WebP
and the TTF share one source of truth.

Rendered at SS x supersampling with Pillow, then downsampled -- matching p5's AA.
Run with a python that has fontTools + shapely (for the import) + Pillow.
"""
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFilter

import build_font_v4 as m

# --- ghost / motion-blur tunables (mirror sketch-v2.js) ------------------------
GHOST_ALPHA = [255, 190, 130]      # whole-atom copies [lead (sharp), mid, trailing]
GHOST_LAG = 0.18                   # radians each copy trails the previous, at peak spin
GHOST_BLUR = 5.0                   # px of blur added per copy, at peak spin

PROTON_GHOST_ALPHA = [255, 150, 90]  # per-proton copies [lead (sharp), mid, trailing]
PROTON_LAG = 12.0                  # px each proton copy trails back, at top speed
PROTON_BLUR = 4.0                  # px of blur added per proton copy, at top speed

# --- output ---
SS = 3                             # supersampling factor for smooth AA + blur
OUT_SIZE = 256                     # final WebP frame size (px)
D = m.SIZE * SS                    # working canvas size


def sc(v):
    return v * SS


def spin_state(tau):
    """(spin radians, atom scale, spin speed 0..1) at loop fraction tau, per the
    spin window. Parameterized on tau (not a frame index) so the WebP can sample the
    loop at any density independently of the font's 48 frames."""
    tms = tau * m.LOOP_MS
    if m.SPIN_START_MS <= tms <= m.SPIN_END_MS:
        w = (tms - m.SPIN_START_MS) / (m.SPIN_END_MS - m.SPIN_START_MS)
        spin = m.spin_ease(w) * 2 * math.pi
        sspeed = m.spin_speed_norm(w)
        return spin, 1.0 - m.SPIN_SHRINK * sspeed, sspeed
    return 0.0, 1.0, 0.0


def ellipse_pts(cx, cy, along, perp, angle, n=96):
    """Supersampled polygon of a rotated ellipse (p5's proton / halo shape)."""
    ca, sa = math.cos(angle), math.sin(angle)
    pts = []
    for i in range(n):
        t = 2 * math.pi * i / n
        ex, ey = along * math.cos(t), perp * math.sin(t)
        pts.append((sc(cx + ca * ex - sa * ey), sc(cy + sa * ex + ca * ey)))
    return pts


def orbit_pts(deg, rx, ry, n=160):
    """Supersampled closed polyline for one orbit ellipse centerline."""
    a = math.radians(deg)
    ca, sa = math.cos(a), math.sin(a)
    pts = []
    for i in range(n + 1):
        t = 2 * math.pi * i / n
        px, py = rx * math.cos(t), ry * math.sin(t)
        pts.append((sc(m.C + ca * px - sa * py), sc(m.C + sa * px + ca * py)))
    return pts


def draw_atom(orbits):
    """The whole atom as white-on-transparent RGBA, with proton trails and the
    clear rings punched to real transparency -- ready to be stacked/blurred."""
    buf = Image.new("RGBA", (D, D), (255, 255, 255, 0))
    dr = ImageDraw.Draw(buf)

    # orbits: three stroked, rotated ellipses (closed loops -> caps don't show).
    w = max(1, int(round(sc(m.STROKE))))
    for o in orbits:
        dr.line(orbit_pts(o["deg"], m.A, m.B), fill=(255, 255, 255, 255), width=w, joint="curve")

    # clear rings: zero the alpha inside each proton's (stretched) halo.
    halo = Image.new("L", (D, D), 0)
    hd = ImageDraw.Draw(halo)
    for o in orbits:
        hd.polygon(ellipse_pts(o["x"], o["y"], o["along"] + m.HALO_GAP,
                               o["perp"] + m.HALO_GAP, o["angle"]), fill=255)
    outside = halo.point(lambda v: 255 - v)            # 255 outside the halos
    kept = Image.composite(buf.split()[3], Image.new("L", (D, D), 0), outside)
    buf.putalpha(kept)

    # nucleus: solid, sharp disk (drawn after the erase so it fills the holes).
    dr = ImageDraw.Draw(buf)
    r = sc(m.NUCLEUS_R)
    cc = sc(m.C)
    dr.ellipse([cc - r, cc - r, cc + r, cc + r], fill=(255, 255, 255, 255))

    # protons: each a back-to-front stack of copies trailing along its own travel
    # direction and blurring, scaled by that proton's speed.
    for o in orbits:
        dirx, diry = math.cos(o["angle"]), math.sin(o["angle"])
        sN = o["speedN"]
        for i in range(len(PROTON_GHOST_ALPHA) - 1, -1, -1):
            lag = i * PROTON_LAG * sN
            blur = i * PROTON_BLUR * sN
            layer = Image.new("RGBA", (D, D), (255, 255, 255, 0))
            ld = ImageDraw.Draw(layer)
            cx, cy = o["x"] - dirx * lag, o["y"] - diry * lag
            ld.polygon(ellipse_pts(cx, cy, o["along"], o["perp"], o["angle"]),
                       fill=(255, 255, 255, PROTON_GHOST_ALPHA[i]))
            if blur > 0.05:
                layer = layer.filter(ImageFilter.GaussianBlur(sc(blur)))
            buf = Image.alpha_composite(buf, layer)
    return buf


def scale_pts(image, factor):
    """Uniformly scale an RGBA image about its center, keeping canvas size."""
    if factor == 1.0:
        return image
    nw = max(1, int(round(D * factor)))
    small = image.resize((nw, nw), Image.LANCZOS)
    canvas = Image.new("RGBA", (D, D), (255, 255, 255, 0))
    off = (D - nw) // 2
    canvas.paste(small, (off, off), small)
    return canvas


def render_frame(tau):
    orbits = [m.orbit_state(k, tau) for k in range(3)]
    spin, atom_scale, sspeed = spin_state(tau)
    atom = draw_atom(orbits)

    # Transparent base. RGB is white everywhere (only alpha carries the shape) so the
    # blur/downscale can't bleed black into the anti-aliased edges.
    out = Image.new("RGBA", (D, D), (255, 255, 255, 0))
    # Stack 3 whole-atom copies back-to-front: trailing (blurriest, faintest) first,
    # sharp lead on top. Lag + blur scale with spin speed -> collapse to one when still.
    for i in range(len(GHOST_ALPHA) - 1, -1, -1):
        lag = i * GHOST_LAG * sspeed
        blur = i * GHOST_BLUR * sspeed
        copy = atom
        if blur > 0.05:
            copy = copy.filter(ImageFilter.GaussianBlur(sc(blur)))
        copy = copy.rotate(-math.degrees(spin - lag), resample=Image.BICUBIC,
                           center=(D / 2.0, D / 2.0), fillcolor=(255, 255, 255, 0))
        copy = scale_pts(copy, atom_scale)
        alpha = copy.split()[3].point(lambda v, a=GHOST_ALPHA[i]: v * a // 255)
        copy.putalpha(alpha)
        out = Image.alpha_composite(out, copy)

    return out                                       # full-res RGBA (cropped later)


def union_crop_box(rendered):
    """A single square crop box (in working-res px) that tightly holds the atom's
    ink across ALL frames. Centered on the true rotation center (D/2, D/2) so the
    animation stays registered -- per-frame crops would make it jitter. The union
    of every frame's alpha bbox already includes the blur/trail spread, so the only
    padding is a rounding guard."""
    cx = cy = D / 2.0
    half = 0.0
    for img in rendered:
        bb = img.getchannel("A").getbbox()           # alpha only (RGB is all-white)
        if bb is None:
            continue
        left, top, right, bottom = bb
        half = max(half, cx - left, right - cx, cy - top, bottom - cy)
    half = min(half + SS, D / 2.0)                    # +1px guard, clamped to canvas
    return (int(round(cx - half)), int(round(cy - half)),
            int(round(cx + half)), int(round(cy + half)))


def render_variant(fps):
    """Render one frame-density variant to atom-spinner-v4-<fps>.webp."""
    n_frames = m.VARIANTS[fps]
    out_path = os.path.join(m.HERE, "atom-spinner-v4-{}.webp".format(fps))
    rendered = [render_frame(f / n_frames) for f in range(n_frames)]

    box = union_crop_box(rendered)
    crop_px = box[2] - box[0]
    frames = [img.crop(box).resize((OUT_SIZE, OUT_SIZE), Image.LANCZOS) for img in rendered]
    print("cropped {}px -> {}px working canvas (trimmed {:.0%} margin), then -> {}px".format(
        D, crop_px, 1.0 - crop_px / D, OUT_SIZE))

    duration = round(m.LOOP_MS / n_frames)            # ms per frame (67 / 33 / 17)
    # RGB is uniform white -> the RGB plane costs almost nothing; the file size is
    # dominated by the ALPHA plane (the soft motion-blur gradients). alpha_quality is
    # therefore the real size lever: 100 -> ~1MB, 70 -> ~450KB with clean soft edges.
    frames[0].save(out_path, format="WEBP", save_all=True, append_images=frames[1:],
                   duration=duration, loop=0, method=6, quality=82, alpha_quality=70)
    print("wrote {}  ({} frames, {}px, {}ms/frame = {}fps, {}s loop, looped)".format(
        out_path, n_frames, OUT_SIZE, duration, fps, m.LOOP_MS // 1000))


def main():
    fps_list = [int(a) for a in sys.argv[1:] if a.isdigit()] or sorted(m.VARIANTS)
    m.build_schedule()
    m.measure_speeds()
    for fps in fps_list:
        if fps not in m.VARIANTS:
            raise SystemExit("fps must be one of {}".format(sorted(m.VARIANTS)))
        render_variant(fps)


if __name__ == "__main__":
    main()
