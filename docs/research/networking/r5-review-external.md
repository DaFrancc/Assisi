# R5 — Adversarial External-Verification Review

*Angle: verify the externally-checkable claims across R1–R4 with independent web
research. Each finding is CONFIRMED / REFUTED / UNVERIFIABLE with a source and an
impact rating on the N0–N7 plan. This review deliberately does not re-audit the
codebase (R4's turf); it checks the outside-world facts the four reports lean on.*

---

## 1. Verified-claims table (CONFIRMED)

| # | Report | Claim | Verdict | Source |
|---|---|---|---|---|
| V1 | R2 | GNS `master` is actively developed through 2025–2026 (zpostfacto / Fletcher Dunn) | **CONFIRMED** — commits run into June 2026, all by zpostfacto | github.com/ValveSoftware/GameNetworkingSockets/commits/master |
| V2 | R2 | GNS ships a **native ICE client** so WebRTC is no longer a dependency | **CONFIRMED** — native ICE landed as beta in v1.4.1 (2020), TURN/RFC 5766 in v1.6.0 | github releases; search |
| V3 | R2 | GNS `CreateSocketPair` gives an in-process loopback pair; default bypasses the wire | **CONFIRMED** — default uses internal buffers (no encrypt/lag/loss) | partner.steamgames.com/doc/api/ISteamnetworkingSockets |
| V4 | R2 | Steam Sockets exposes the *same* `ISteamNetworkingSockets` API as standalone GNS (relink-to-swap) | **CONFIRMED** | Steamworks multiplayer/networking docs |
| V5 | R2 | protobuf+abseil is a real CMake build sore spot (C++-standard/version lockstep) | **CONFIRMED** — `ABSL_PROPAGATE_CXX_STD`, `protobuf_ABSL_PROVIDER`, std-mismatch link errors are widely reported | protobuf issues #12637/#17539; oneuptime OTel-cpp guide |
| V6 | R2 | yojimbo is dormant; last release v1.7.0 (2024-07-18), a nonce-reuse security fix | **CONFIRMED** — no 2025–2026 releases; v1.7.0 fixes AEAD nonce reuse on server restart | github.com/mas-bandwidth/yojimbo/releases |
| V7 | R2 | enet6 is single-dev-maintained, IPv6 + an encryption *hook* (BYO cipher), last tag ~Oct 2024 (v6.1.0) | **CONFIRMED** | github.com/SirLynix/enet6/releases |
| V8 | R2 | RFC 9221 (QUIC unreliable DATAGRAM) exists and is apt-but-not-standard for games | **CONFIRMED** — published March 2022; games named as a use case | rfc-editor.org/info/rfc9221 |
| V9 | R2/R4 | GNS `CreateSocketPair(bUseNetworkLoopback=true)` routes via 127.0.0.1 so fake-lag/loss apply; default pair does **not** simulate | **CONFIRMED** verbatim | Steamworks ISteamNetworkingSockets docs |
| V10 | R1/R3 | Source default snapshot rate ~20 Hz; `cl_interp` 0.1 s (100 ms); tick 66 (30 for L4D) | **CONFIRMED** (with a wording nit — it's `cl_updaterate` 20, client-requested, capped at tick; see C-nit R1) | Valve wiki; edgegamers/AlliedModders rate threads |
| V11 | R1/R3 | Source/Quake use delta snapshots against the **last acked** update; degrade-to-bigger-delta not desync | **CONFIRMED** | Valve wiki; Sanglard Q3 review |
| V12 | R1/R3 | Overwatch: client runs ahead ½·RTT + 1 command frame; **time dilation to ~15.2 ms** on input starvation; predicts everything by default | **CONFIRMED** | Ford GDC 2017; Edgegap deep-dive |
| V13 | R1/R3 | Smallest-three quaternion: drop largest component, others in ±0.707, 2+9+9+9 = 29 bits, reconstruct via sqrt | **CONFIRMED** (see contradiction X4 on the *bit count* R1 vs R3) | Gaffer snapshot_compression; StagPoint gist |
| V14 | R1/R3 | bevy_replicon drives replication off Bevy change ticks and "does not distinguish modification from re-insertion" | **CONFIRMED** | docs.rs/bevy_replicon |
| V15 | R1/R3 | Rocket League uses Bullet for deterministic networked physics (the determinism-tax counterexample) | **CONFIRMED** (minor nuance: Bullet was also just the chosen engine; determinism is the well-attested motive) | Cone GDC 2018; pybullet.org; Wikipedia Bullet |
| V16 | R4 | GNS is poll-driven with an internal service thread; coarse internal locking shows as contention under load | **CONFIRMED** (issues #50/#307 exist); non-issue at 2–32 players | GNS issues; Steamworks docs |

**Convergence signal (independent, not echo):** the two-module transport/replication
split is reached separately by O3DE (AzNetworking + Multiplayer Gem) and Bevy
(renet + replicon) — different codebases, same shape. The acked-baseline delta is
reached independently by Q3, Source, O3DE, and Fiedler. Change-detection-driven
sends are reached by Bevy/replicon, Unreal Iris ("objects notify… rather than the
system polling"), and O3DE. These are genuine independent corroborations, not four
reports citing one blog.

---

## 2. Refuted / corrected claims (with evidence)

### R2-A — GNS release dates are wrong by three years. **Impact: MEDIUM.**
R2 states the "one factual correction for Stage 0" as: last tag **v1.6.0 dated
2024-06-03**, v1.5.1 2024-05-04, v1.5.0 2024-04-28, and calls the tag "~2-year-old
code." **The actual dates are 2021:** v1.6.0 = **2021-06-03**, v1.5.1 = 2021-05-04,
v1.5.0 = 2021-04-28, v1.4.1 = 2020-06-16 (source: GitHub releases page). The
day/month align exactly — R2 shifted every year +3. Consequence: the pinned tag is
**~5 years old (2026)**, not ~2. This does not flip R2's recommendation — it
*strengthens* "pin a recent `master` commit, not the tag" — but R2's headline
"factual correction" is itself factually wrong, and the "~2-year-old" framing
understates staleness. The plan should record the real dates and lean harder on the
master-pin recommendation. (Also minor: R2 implies native ICE arrived "since
v1.4.1→v1.6.0"; the ICE beta actually shipped *in* v1.4.1, June 2020.)

### R1-B — "Of ~150 systems, only three touch netcode." **Impact: LOW (credibility).**
The Overwatch figure is **~46 client-side systems and 103 component types**, of
which 3 systems (movement, weapons, state-script) do netcode. R1's "~150 systems"
conflates the count (likely with component types, or invented). **R3 states it
correctly** ("3 systems out of 46"). No recommendation depends on this, but it is a
factual miss in R1's flagship case study and a mark against taking R1's numbers on
trust. Source: Edgegap Overwatch deep-dive; are.na notes of the GDC talk.

### R1-nit — "Source sends snapshots at ~20/sec by default (`sv_updaterate`)." **Impact: LOW.**
The default is **`cl_updaterate` = 20** — a *client-requested* rate; the server
sends `min(tick, requested)` and never more than one update per simulated tick.
`sv_updaterate` is not the knob named. The *number* (20 Hz) and the lesson (snapshot
< sim rate) are correct; the variable attribution is loose. Source: Valve wiki.

### R2-nit — enet6 "v6.1.0 adds an encryption hook." **Impact: LOW — CONFIRMED, not refuted**, but worth stating the limitation R2 already flags: it is a *transform callback*, not key exchange/authentication — you still bring a handshake (Godot bolts on DTLS/mbedTLS). R2 states this correctly; flagged only so the plan does not read "enet6 has encryption" as parity with GNS's built-in AEAD.

### General — GDC-Vault primary talks are **UNVERIFIABLE** directly.
The Ford (Overwatch), Cone (Rocket League), and Aldridge (Halo) talks sit behind
GDC Vault; the Valve wiki 403'd this session. Every specific number attributed to
them (15.2 ms dilation, ½RTT+1 frame, Halo's "ragdoll cut → 20% bandwidth",
"favor-the-shooter ≤220 ms cutoff") was corroborated **only through secondary
write-ups** (Edgegap, Wolfire, gamedev.net). The load-bearing ones (dilation,
command-frame lead, smallest-three, Source rates) reproduce consistently across
independent secondaries and are safe. The Halo "20% bandwidth after cutting
ragdolls" and the exact "220 ms" cutoff rest on single-secondary paraphrase — treat
as directionally-true illustrations, not hard figures. **Impact: LOW** (they
motivate deferrals, they don't set parameters).

---

## 3. Cross-report contradictions

- **X1 — Overwatch system count.** R1 "~150 systems" vs R3 "3 of 46." R3 is correct
  (§R1-B). Signals R1's numbers need spot-checking; R3's are more reliable here.

- **X2 — Snapshot codec base type (R3 vs the plan, and R3 vs R4).** R3 argues Stage 4
  must be **bit-level (`BitWriter`) from day one** ("byte-aligned first will force a
  painful migration"). R4 repeatedly writes `ByteWriter`/`ByteReader` as the Stage-4
  primitive without challenging it, and frames fuzzing around a `ByteReader`. Not a
  factual contradiction, but the two reviews give the implementer *different*
  Stage-4 marching orders (bit-capable vs byte-aligned). The plan must pick; R3's
  case (1-bit "unchanged" flag and smallest-three are impossible byte-aligned) is
  the technically correct one and should win. **Impact: MEDIUM** — it's a format
  decision that's expensive to change late.

- **X3 — Priority accumulator: pull-forward vs defer.** R1 and R3 both say add it (or
  at least the sortable-send-loop seam) at N5; R1 is more insistent ("the single
  most valuable pattern the plan is missing"). R3 softens to "don't *implement* it
  yet, just don't hardcode a for-loop." These are compatible but differently
  weighted; the reconciled reading is R3's (build the seam, defer the mechanism).
  **Impact: LOW** (agreement on the seam; only urgency differs).

- **X4 — Smallest-three bit budget.** R1 says "smallest-three quaternion, **15
  bits/component**" (attributed to Fiedler's networked-physics/state-sync); R3 says
  "**9 bits/component**, 29 bits total" (Fiedler's *Snapshot Compression*). Both are
  real Fiedler figures from *different* articles (he uses 9 in snapshot compression;
  higher per-component precision appears in the physics-sync context). Not a
  contradiction of fact, but R1's "15" is the looser citation. **Impact: LOW** —
  either is a tuning starting point.

- **X5 — NAT-traversal rationale.** R2 explicitly refutes the design doc's
  "enet6/yojimbo: no NAT traversal" justification (standalone GNS also needs your
  own signaling; NAT is deferred to N7). R1/R3/R4 don't touch NAT and implicitly
  accept the doc. No inter-report conflict, but R2's correction stands *against the
  plan* and is well-evidenced — adopt it (re-anchor GNS on encryption/lanes/CC/
  loopback/Steam, not NAT). **Impact: MEDIUM** for the doc's stated reasoning; **LOW**
  for the outcome (GNS still wins).

---

## 4. Topics all four under- or un-covered

1. **Movement / input-plausibility validation (anti-cheat).** Server authority is
   necessary but not sufficient: a server that blindly applies client `InputCommand`
   frames is still exploitable (speed/teleport hacks via forged inputs, impossible
   look/move deltas). R4 covers *rate* limiting and codec fuzzing but nobody covers
   *semantic* input validation (clamp per-tick movement to the controller's max, bound
   command timestamps to the server's accepted window). For a publicly-released engine
   this is a real gap. **Impact: MEDIUM** — should be a line in the N5/hardening scope.

2. **Join-time / late-join bandwidth burst.** All four endorse "empty-baseline delta =
   late join" (good), but none address that the *initial full state* of a populated
   world sent reliably to a joining client (or to 32 clients after a level load) is a
   burst that can saturate the reliable lane and stall snapshots. Production systems
   **flow-control the baseline download** (paginate the keyframe, throttle spawns).
   Godot's "late-join is fiddly" (R1 §7) is the symptom; the bandwidth-burst mechanism
   is the unnamed cause. **Impact: MEDIUM.**

3. **Reconnection / client state repair.** None cover a client dropping and rejoining
   mid-session (re-sync without a full teardown). This is a known, separately-solved
   problem — `bevy_replicon_repair` exists *specifically* for reconnect state repair —
   which is direct evidence the base replicon model (that R1/R3 lean on) doesn't handle
   it for free. **Impact: LOW–MEDIUM** (deferrable, but name it).

4. **Actual 32-player bandwidth budget.** R1/R3 cite Fiedler's 900-cube demo
   (17 Mbps → 256 kbps) but nobody computes an Assisi-scale envelope (N replicated
   entities × 32 clients × 20–30 Hz × delta size) to confirm the "trivial at this
   scale" assertion. It's *probably* fine, but it's asserted, not shown. **Impact: LOW.**

5. **UDP source-address validation / amplification on connect.** R2 §8 and R4 §B8
   gesture at "DoS hardening" and rate limiting; neither states that GNS's handshake
   already does connection-request validation (so the engine shouldn't re-invent it)
   nor where the residual app-level exposure is. Minor, mostly covered. **Impact: LOW.**

No missed *library*: R2's transport survey is comprehensive for 2026 (GNS, enet/enet6,
yojimbo/netcode.io/reliable.io, KCP, nbnet, RakNet/SLikeNet, MsQuic/quiche/lsquic,
Steam Sockets, EOS, libjuice). Nothing in this class has displaced them; no serious
omission found.

---

## 5. Final verdict — can each report's "Implications for Assisi" be trusted?

- **R1 (case studies): TRUST, with a factual-precision caveat.** Its architectural
  conclusions (server-authoritative snapshot + interpolation, no lockstep; NetId;
  acked-baseline delta; per-field change mask; 20–30 Hz snapshots; priority
  accumulator gap) are all independently corroborated and correct. But R1 is the
  loosest with *numbers* (the ~150-systems error, the `sv_updaterate` mislabel, the
  15-bit quat). Trust the *recommendations*; re-check any *figure* before it becomes a
  constant. The "Implications" section is sound.

- **R2 (transport): TRUST the destination, correct the dates.** The GNS decision and
  the pimpl-hedge are right, and the NAT-rationale correction is a genuine improvement
  over the design doc. But the section's own headline "factual correction" (the v1.6.0
  date) is wrong by three years — fix it to 2021 and the conclusion (pin a recent
  `master`, not the 5-year-old tag) actually gets *stronger*. Everything else in R2's
  Implications verifies.

- **R3 (replication theory): TRUST — the strongest of the four on external facts.**
  Every load-bearing theory claim checks out (acked baseline, Source 100 ms/20 Hz,
  Overwatch command-frame/dilation, smallest-three 29 bits, bit-packing necessity,
  bevy_replicon change-ticks). It also carries the correct Overwatch system count that
  R1 fumbled. Its two headline "CHANGE" items (bit-level codec from day one; default
  20–30 Hz) are well-grounded. Adopt its Implications as written.

- **R4 (codebase fit + ops): TRUST on the external/ops facts.** Its web-checkable
  claims — GNS poll-model + coarse-lock contention, the `bUseNetworkLoopback` fake-lag
  distinction, Fiedler *Fix Your Timestep*, non-deterministic → convergence-within-ε
  test oracle — all verify. (Its codebase line-number audit is outside this review's
  scope but is internally self-consistent and self-flags drift.) One cross-report seam:
  R4 keeps writing `ByteReader`/`ByteWriter` where R3 argues for `BitWriter` — resolve
  toward R3. Implications trustworthy.

**Overall:** No finding overturns the N0–N7 plan or the GNS choice. The corrections
are (a) fix R2's GNS dates to 2021 and re-anchor its NAT rationale, (b) treat R1's raw
numbers as needing a second look while trusting its architecture, (c) resolve the
byte-vs-bit codec seam toward R3's bit-level, and (d) add the four under-covered
topics — input-plausibility validation, join-time bandwidth burst, reconnect repair,
and a real 32-player bandwidth estimate. The convergence across O3DE, Bevy, Unreal
Iris, Quake/Source and Fiedler on the core shape is independent and real, so the
architecture itself is on solid external footing.
