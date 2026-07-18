// Atom-spinner rendered live in p5.js.
//
// Ported from build_font.py, which baked this same animation into a TTF font.
// A font glyph needs filled outlines, so the Python version used shapely boolean
// ops (buffer / union / difference) to turn stroked orbits and the clear ring
// around each electron into solid contours. p5 doesn't need any of that: it draws
// the orbits as strokes, then paints a background-coloured "halo" disk over each
// electron to erase the orbit beneath it, then draws the electron on top. Same
// look, no geometry library.
//
// Motion (identical to the Python):
//   Electron k rides orbit k (rotated 60*k deg) at parameter t_k = tau(phi)+180*k.
//   The 180*k offset makes sum_k position_k == 0 for every phi, so the mass stays
//   exactly centered and never wobbles. All orbits pulse by the same factor.

// --- spinner geometry (same coordinate space as the SVG/font: 0..SIZE, y-down) ---
const SIZE = 512;
const C = SIZE / 2.0;          // center (256)
const A = 200.0;               // orbit semi-major axis
const B = 96.0;                // orbit semi-minor axis
const NUCLEUS_R = 50.0;
const ELECTRON_R = 24.0;
const STROKE = 13.0;           // orbit line thickness
const HALO_GAP = 10.0;         // width of the clear ring around each electron

const N_FRAMES = 48;
const PULSE_AMP = 0.14;        // orbits scale between (1-amp) and (1+amp)
const PULSE_CYCLES = 2;        // breaths per loop; MUST be even (motion is coupled)
const EASE_STRENGTH = 0.7;     // 0 = ellipse-only easing; 1 = electron stalls at tips
const ORBITS = [0.0, 60.0, 120.0]; // orbit rotations in degrees

// --- look ---
const LOOP_MS = 2000;          // one full 48-frame loop takes this long
const FG = 255;                // spinner colour (white)
const BG = 17;                 // background colour (near-black)

function pulse(phi) {
  // Synchronized breathing factor, same for every orbit (preserves balance).
  return 1.0 + PULSE_AMP * Math.sin(PULSE_CYCLES * phi);
}

function ease(u) {
  // Ease-in-out on [0,1], blended with linear by EASE_STRENGTH (endpoints exact).
  const smooth = u * u * (3.0 - 2.0 * u);
  return (1.0 - EASE_STRENGTH) * u + EASE_STRENGTH * smooth;
}

function tau(phi) {
  // Pulse-coupled orbit-parameter phase.
  const psi = PULSE_CYCLES * phi;
  const chi = psi - Math.PI / 2.0;       // chi = 0 at the first major-axis tip
  const seg = Math.floor(chi / Math.PI); // which tip-to-tip segment
  const u = chi / Math.PI - seg;         // progress within the segment, in [0,1)
  return seg * (Math.PI / 2.0) + (Math.PI / 2.0) * ease(u);
}

function electronPos(k, phi, s) {
  // Screen position of electron k on its (pulsed) orbit.
  const th = (60.0 * k) * Math.PI / 180.0;
  const t = tau(phi) + (180.0 * k) * Math.PI / 180.0;
  const lx = A * s * Math.cos(t);
  const ly = B * s * Math.sin(t);
  const x = C + Math.cos(th) * lx - Math.sin(th) * ly;
  const y = C + Math.sin(th) * lx + Math.cos(th) * ly;
  return { x, y };
}

function setup() {
  createCanvas(SIZE, SIZE);
  ellipseMode(RADIUS); // circle(x, y, r) uses r as radius, matching the Python
}

function draw() {
  background(BG);

  // Continuous, seamless loop. phi runs 0..2*pi over LOOP_MS.
  const phi = (millis() % LOOP_MS) / LOOP_MS * TWO_PI;
  const s = pulse(phi);
  const rx = A * s;
  const ry = B * s;
  const elec = [0, 1, 2].map((k) => electronPos(k, phi, s));

  // 1) orbits: three stroked, rotated ellipses.
  noFill();
  stroke(FG);
  strokeWeight(STROKE);
  strokeCap(ROUND);
  for (const deg of ORBITS) {
    push();
    translate(C, C);
    rotate(radians(deg));
    ellipse(0, 0, rx, ry); // RADIUS mode -> rx, ry are the semi-axes
    pop();
  }

  // 2) clear ring: erase the orbit under each electron by painting a bg halo.
  noStroke();
  fill(BG);
  for (const p of elec) {
    circle(p.x, p.y, ELECTRON_R + HALO_GAP);
  }

  // 3) nucleus + electrons: solid foreground disks.
  fill(FG);
  circle(C, C, NUCLEUS_R);
  for (const p of elec) {
    circle(p.x, p.y, ELECTRON_R);
  }
}
