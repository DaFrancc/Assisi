// Atom-spinner v2 — orbital (Kepler-style) motion + three-phase stagger.
//
// Differences from v1:
//   1. SPEED VARIES WITH DISTANCE. Each particle moves slow at the far ends of its
//      orbit and fast as it swings close to the nucleus, like a real orbit obeying
//      conservation of angular momentum. v1 did the opposite (it eased so particles
//      lingered at *both* tips). Here the eccentric-angle advance rate is
//      dt/dtau proportional to 1 / r^KEPLER_EXP, so larger r (far out) => slower,
//      smaller r (close in) => faster. The schedule is integrated once in setup()
//      into a lookup table, then sampled each frame.
//   2. THREE-PHASE STAGGER. The three particles ride the SAME schedule offset in
//      loop-time by k/3. Because an ellipse has two far ends (reached every half
//      loop), a k/3 offset lands the six "at a far end" moments evenly spaced at
//      tau = 0, 1/6, 1/3, 1/2, 2/3, 5/6 — so they cleanly take turns swinging out.
//   3. STATIC ORBITS, GROWING PARTICLES. The ellipses stay a fixed size; instead
//      each particle grows with its distance from the nucleus, up to PARTICLE_GROWTH
//      bigger when it's out at a far tip.

// --- spinner geometry (0..SIZE, y-down, same space as the SVG/font) ---
const SIZE = 512;
const C = SIZE / 2.0;          // center (256)
const A = 200.0;               // orbit semi-major axis
const B = 96.0;                // orbit semi-minor axis
const NUCLEUS_R = 50.0;
const ELECTRON_R = 18.0;
const STROKE = 13.0;           // orbit line thickness
const HALO_GAP = 10.0;         // width of the clear ring around each particle

const ORBITS = [0.0, 60.0, 120.0]; // orbit rotations in degrees (orbit k <-> particle k)

// --- new v2 tunables ---
const KEPLER_EXP = 2.2;        // how strongly speed varies with distance.
                               //   0   = mild (geometry only, ~2x near:far)
                               //   1.5 = pronounced orbital feel (~6x)
                               //   2.2 = fast dive past the nucleus, long dwell at tips
const PHASE_STEP = 1.0 / 3.0;  // loop-time offset between particles (1/3 = three-phase)
const PARTICLE_GROWTH = 0.5;   // particle radius gain from closest (0) to farthest (this);
                               // 0.5 => 50% bigger out at the far tips
const STRETCH = 1.8;           // motion-blur elongation along travel at top speed.
                               // 0 = none; 1 = the along-travel axis doubles.
                               // Scales with speed, so it's 0 at the (slow) far tips.

// --- look ---
const LOOP_MS = 2000;          // one full loop of the internal particle motion
const SPIN_START_MS = 250;     // whole-atom 360 spin runs across [SPIN_START_MS, SPIN_END_MS];
const SPIN_END_MS = 1750;      // outside that window the atom is perfectly still.
const SPIN_EASE = 5.0;         // ease-in-out steepness of the spin within its window.
const SPIN_SHRINK = 0.3;       // how much the whole atom shrinks at peak spin speed.
                               // 0.3 => scales down to 70% when spinning fastest.

// --- motion blur: 3 stacked copies of the atom, each lagging + blurrier than the last ---
const GHOST_ALPHA = [255, 190, 130]; // opacity of copies [lead (sharp), mid, trailing]
const GHOST_LAG = 0.18;        // radians each copy trails the previous one, at peak spin
const GHOST_BLUR = 5.0;        // px of blur added per copy, at peak spin
                               // (both LAG and BLUR scale with spin speed -> 0 when still)

// --- the same trail effect on each proton as it whips past the nucleus ---
const PROTON_GHOST_ALPHA = [255, 150, 90]; // per-proton copies [lead (sharp), mid, trailing]
const PROTON_LAG = 12.0;       // px each proton copy trails back along travel, at top speed
const PROTON_BLUR = 4.0;       // px of blur added per proton copy, at top speed
                               // (both scale with the proton's speed -> 0 at the slow tips)
const FG = 255;                // spinner colour (white)
const BG = 17;                 // background colour (near-black)

// --- Kepler schedule lookup table: tau (loop fraction) -> eccentric angle t ---
const TABLE_N = 2048;
let tauToT = [];

function rBase(t) {
  // Distance from center on the base (unpulsed) ellipse at eccentric angle t.
  const x = A * Math.cos(t);
  const y = B * Math.sin(t);
  return Math.sqrt(x * x + y * y);
}

function buildSchedule() {
  // "Time cost" to advance the eccentric angle is r^KEPLER_EXP (more cost where r
  // is large => the particle spends more real time far out => moves slower there).
  // Integrate c(t) = ∫_0^t r^KEPLER_EXP dt' over one revolution, normalize to a
  // uniform tau grid, and invert so we can look up t for any tau.
  const M = 4096;
  const dt = (2 * Math.PI) / M;
  const cSamples = new Array(M + 1);
  let c = 0.0;
  cSamples[0] = 0.0;
  for (let i = 0; i < M; i++) {
    const t = i * dt;
    const w = 0.5 * (Math.pow(rBase(t), KEPLER_EXP) + Math.pow(rBase(t + dt), KEPLER_EXP));
    c += w * dt;
    cSamples[i + 1] = c;
  }
  const total = cSamples[M];

  tauToT = new Array(TABLE_N);
  let j = 0;
  for (let i = 0; i < TABLE_N; i++) {
    const target = (i / TABLE_N) * total;
    while (j < M && cSamples[j + 1] < target) j++;
    const c0 = cSamples[j];
    const c1 = cSamples[j + 1];
    const frac = c1 > c0 ? (target - c0) / (c1 - c0) : 0.0;
    tauToT[i] = (j + frac) * dt;
  }
}

function eccentricAngle(tau) {
  // Map a loop fraction (any real; wrapped to [0,1)) to the scheduled angle t.
  const f = tau - Math.floor(tau);
  const x = f * TABLE_N;
  const i = Math.floor(x);
  const frac = x - i;
  const a = tauToT[i % TABLE_N];
  let b = tauToT[(i + 1) % TABLE_N];
  if (i + 1 >= TABLE_N) b += 2 * Math.PI; // continue past 2*pi across the wrap
  return a + (b - a) * frac;
}

function localVel(tau) {
  // Velocity of a particle in its own (unrotated) orbit frame, via finite
  // difference of the scheduled position. Direction and magnitude both matter.
  const h = 1e-4;
  const t0 = eccentricAngle(tau);
  const t1 = eccentricAngle(tau + h);
  const vx = (A * Math.cos(t1) - A * Math.cos(t0)) / h;
  const vy = (B * Math.sin(t1) - B * Math.sin(t0)) / h;
  return { vx, vy };
}

// Speed range over one loop, used to normalize the stretch (same profile for all
// three particles, so one is enough). Filled in setup() after the schedule exists.
let SPEED_MIN = 0.0;
let SPEED_MAX = 1.0;

function measureSpeeds() {
  const N = 1024;
  let lo = Infinity;
  let hi = 0.0;
  for (let i = 0; i < N; i++) {
    const { vx, vy } = localVel(i / N);
    const sp = Math.hypot(vx, vy);
    if (sp < lo) lo = sp;
    if (sp > hi) hi = sp;
  }
  SPEED_MIN = lo;
  SPEED_MAX = hi;
}

function orbitState(k, tau) {
  // Particle k rides orbit k (rotated 60*k deg) on the Kepler schedule, staggered
  // in loop-time by k * PHASE_STEP. The orbit is a fixed size; the particle grows
  // with its distance u from the nucleus (0 at closest, 1 at a far tip) and
  // stretches along its travel direction in proportion to how fast it's moving.
  const p = tau + k * PHASE_STEP;
  const t = eccentricAngle(p);
  const u = (rBase(t) - B) / (A - B);                 // 0 at closest, 1 at farthest
  const r = ELECTRON_R * (1.0 + PARTICLE_GROWTH * u); // base particle radius

  const th = (60.0 * k) * Math.PI / 180.0;
  const ct = Math.cos(th);
  const st = Math.sin(th);
  const lx = A * Math.cos(t);
  const ly = B * Math.sin(t);
  const x = C + ct * lx - st * ly;
  const y = C + st * lx + ct * ly;

  // Normalized speed: 1 at the fast closest approach, 0 at the slow far tips
  // (so there's exactly no stretch at the peaks).
  const { vx, vy } = localVel(p);
  const speed = Math.hypot(vx, vy);
  const sN = (speed - SPEED_MIN) / Math.max(SPEED_MAX - SPEED_MIN, 1e-9);

  // Travel direction in screen space (rotate the local velocity by the orbit).
  const angle = Math.atan2(st * vx + ct * vy, ct * vx - st * vy);
  const along = r * (1.0 + STRETCH * sN); // semi-axis along travel
  const perp = r;                          // semi-axis across travel

  return { x, y, deg: 60.0 * k, angle, along, perp, speedN: sN };
}

function spinEase(w) {
  // Symmetric power ease-in-out on [0,1]. Larger SPIN_EASE => flatter near the
  // ends and steeper through the middle => a more pronounced whip.
  return w < 0.5
    ? 0.5 * Math.pow(2.0 * w, SPIN_EASE)
    : 1.0 - 0.5 * Math.pow(2.0 * (1.0 - w), SPIN_EASE);
}

function spinSpeedNorm(w) {
  // Normalized angular speed of the eased spin: 0 at the window ends, 1 at the
  // middle (the derivative of spinEase, scaled to peak at 1).
  const e = w < 0.5 ? 2.0 * w : 2.0 * (1.0 - w);
  return Math.pow(e, SPIN_EASE - 1.0);
}

let atomBuf; // offscreen: the atom drawn once per frame (white on transparent)

function drawAtom(g, orbits) {
  // Render the whole atom into buffer g: white shapes on a transparent background,
  // with the clear rings punched out as real transparency (via erase) so the buffer
  // composites cleanly when stacked/blurred. No spin or scale here — that's applied
  // per-copy when the buffer is blitted.
  g.clear();

  // orbits: three stroked, rotated ellipses at a fixed size.
  g.noFill();
  g.stroke(FG);
  g.strokeWeight(STROKE);
  g.strokeCap(ROUND);
  for (const o of orbits) {
    g.push();
    g.translate(C, C);
    g.rotate(radians(o.deg));
    g.ellipse(0, 0, A, B);
    g.pop();
  }

  // clear rings: punch transparent holes matching each particle's stretched shape.
  g.noStroke();
  g.fill(255);
  g.erase();
  for (const o of orbits) {
    g.push();
    g.translate(o.x, o.y);
    g.rotate(o.angle);
    g.ellipse(0, 0, o.along + HALO_GAP, o.perp + HALO_GAP);
    g.pop();
  }
  g.noErase();

  // nucleus: solid foreground disk (kept sharp), drawn after so it fills the holes.
  g.drawingContext.filter = 'none';
  g.fill(FG);
  g.circle(C, C, NUCLEUS_R);

  // protons: each drawn as a small stack of copies that trail backward along the
  // proton's own travel direction and blur, scaled by that proton's speed. So a
  // proton smears as it whips past the nucleus and is crisp out at the slow tips.
  // Drawn back-to-front (faint trailing copy first, sharp lead last).
  for (const o of orbits) {
    const dirx = Math.cos(o.angle);
    const diry = Math.sin(o.angle);
    for (let i = PROTON_GHOST_ALPHA.length - 1; i >= 0; i--) {
      const lagDist = i * PROTON_LAG * o.speedN;
      const blurPx = i * PROTON_BLUR * o.speedN;
      g.drawingContext.filter = blurPx > 0.05 ? `blur(${blurPx}px)` : 'none';
      g.push();
      g.translate(o.x - dirx * lagDist, o.y - diry * lagDist);
      g.rotate(o.angle);
      g.fill(FG, PROTON_GHOST_ALPHA[i]);
      g.ellipse(0, 0, o.along, o.perp);
      g.pop();
    }
  }
  g.drawingContext.filter = 'none';
}

function setup() {
  createCanvas(SIZE, SIZE);
  ellipseMode(RADIUS);
  atomBuf = createGraphics(SIZE, SIZE);
  atomBuf.ellipseMode(RADIUS);
  buildSchedule();
  measureSpeeds();
}

function draw() {
  background(BG);

  const tms = millis() % LOOP_MS;
  const tau = tms / LOOP_MS; // 0..1, internal-motion loop
  const orbits = [0, 1, 2].map((k) => orbitState(k, tau));

  // Whole-atom 360 spin during [SPIN_START_MS, SPIN_END_MS], eased in/out; perfectly
  // still the rest of the loop (a full 360 lands back at the original orientation).
  // The atom also shrinks in proportion to how fast it's spinning (smallest mid-spin).
  let spin = 0.0;
  let atomScale = 1.0;
  let spinSpeed = 0.0;
  if (tms >= SPIN_START_MS && tms <= SPIN_END_MS) {
    const w = (tms - SPIN_START_MS) / (SPIN_END_MS - SPIN_START_MS); // 0..1 across window
    spin = spinEase(w) * TWO_PI;
    spinSpeed = spinSpeedNorm(w);
    atomScale = 1.0 - SPIN_SHRINK * spinSpeed;
  }

  // Draw the atom once into the offscreen buffer.
  drawAtom(atomBuf, orbits);

  // Stack 3 copies: the lead copy is sharp and on top; each copy behind it lags a
  // little further back in rotation and gets blurrier, forming a rotational trail.
  // Lag and blur both scale with spin speed, so the copies collapse into one crisp
  // atom when it's not spinning. Drawn back-to-front (trailing copy first).
  const ctx = drawingContext;
  for (let i = GHOST_ALPHA.length - 1; i >= 0; i--) {
    const lag = i * GHOST_LAG * spinSpeed;
    const blurPx = i * GHOST_BLUR * spinSpeed;
    ctx.filter = blurPx > 0.05 ? `blur(${blurPx}px)` : 'none';
    push();
    translate(C, C);
    rotate(spin - lag);
    scale(atomScale);
    translate(-C, -C);
    tint(FG, GHOST_ALPHA[i]);
    image(atomBuf, 0, 0);
    pop();
  }
  ctx.filter = 'none';
  noTint();
}
