# Networking — design notes & implementation plan

Captured 2026-07-20. **v2, revised 2026-07-22** after a multi-agent research
review (4 independent researchers — engine case studies, transport-library
survey, replication theory, codebase fit/ops — cross-checked by 2 adversarial
reviewers, then synthesized; full cited reports in `docs/research/networking/`).

**Implementation status (2026-07-22): stages 0-5 are built, tested and
committed; stage 6 is half done** (headless `--host`/`--connect` work between
two processes over UDP; the windowed client, client-side interpolation, the
listen server, and the Host/Join UI are not wired up). The plan below is left
as written — it is the design record, and the stage text is what the code was
built against. Where the implementation diverged, the divergence is noted
inline. See `remaining-work.md` §1 for the current gap list, which is the
authoritative status.

The review's headline: **the architecture survives scrutiny unchanged** —
server-authoritative delta-snapshot replication over a two-module
transport/replication split is independently re-derived by O3DE
(AzNetworking + Multiplayer Gem) and the Bevy ecosystem (renet/replicon), and
the acked-baseline delta model is the Quake 3 / Source / Tribes / Fiedler
consensus. The revisions below are refinements: one re-order (change-tick
stamping moves before replication), one format decision made early (bit-level
codec), corrected transport reasoning, and previously-missing seams named
(headless link DAG, clock sync, input hardening, join-burst flow control).

## Decisions

- **Model: server-authoritative state replication with snapshot interpolation**
  (Quake/Overwatch lineage). *Not* deterministic lockstep: the build ships
  `-ffast-math` in Release (`CMakeLists.txt` `ASSISI_ENABLE_FAST_MATH`), forces
  AVX2+FMA, and builds Jolt without `CROSS_PLATFORM_DETERMINISTIC` — three
  deliberate perf choices that lockstep would have to fight. State replication
  needs none of them reverted. Review verdict: strongly confirmed — the
  deterministic alternative (Photon Quantum, Rocket League) demands
  fixed-point/determinism-tax everywhere; Overwatch demonstrates quantized
  agreement without determinism is production-proven.
- **Topologies: dedicated server and listen server, where the listen server is
  a dedicated server embedded in the client process.** One server codepath. P2P
  lockstep is out of scope permanently; P2P *topology* (player-hosted) is just
  the listen server.
- **Transport: Valve GameNetworkingSockets (GNS)** — confirmed against a full
  2026 field survey (enet6, yojimbo/netcode.io, KCP, nbnet, RakNet/SLikeNet,
  MsQuic/quiche/lsquic QUIC datagrams, Steam Sockets, EOS; see
  `docs/research/networking/r2-transport-libraries.md`). **Corrected
  rationale:** the original "enet6/yojimbo rejected: no NAT traversal" reasoning
  was unsound — standalone GNS's ICE *also* requires our own signaling service
  (Stage 7 says so itself). GNS wins on what it actually uniquely bundles:
  built-in authenticated encryption (AES-GCM + 25519, no BYO-crypto), lanes
  (no head-of-line blocking between snapshot and bulk), real congestion
  control, the in-process `CreateSocketPair` loopback (listen-server enabler),
  and a **verified** same-API relink path to Steam Sockets / Steam Datagram
  Relay. Fallback if its build chain ever becomes untenable: enet6 + DTLS
  behind the same `Assisi::Net` pimpl (the pimpl is the hedge — keep it).
  **Pin: a recent `master` SHA, not the v1.6.0 tag.** The last tagged release
  is 2021-06-03 (five years stale); `master` is actively maintained by Valve
  through 2026 and carries the protobuf-compat and native-ICE work the tag
  predates. The old "GNS drags in WebRTC" concern is obsolete (native ICE since
  v1.4.1; TURN in v1.6.0).
- **Replication layer: built in-engine**, on top of the primitives that already
  exist for it: stable `ComponentId` wire identity, `ComponentRegistry` /
  `ComponentMeta` type-erased hooks, per-component change ticks
  (`ACOMP(tracked)`), `Registry::ReviveAt`, and the raw-handle EntityRef mode.
  No off-the-shelf replication framework fits this ECS. Review verdict: the
  change-tick substrate is complete and tested (`SparseSet` tick lane,
  `Scene::GetMut`/`Add`/`MarkChanged` stamping, `Changed(since)`,
  `TestChangeDetection.cpp`); exactly one hole remains (queries — see
  Stage 3½).

## Module layering — two modules, not one

Networking splits into two concerns with very different dependency needs:

```
Assisi::Net      (modules/Net)      — transport. Depends on Core + GNS only.
Assisi::NetSync  (modules/NetSync)  — replication. Depends on Core + ECS + Net.
```

- **`Net` sits low, next to Core**, as a dumb pipe: connections, messages,
  lanes, events. Anything may link it later (asset streaming from a remote
  cache, editor collaboration, telemetry) without dragging in ECS.
- **`NetSync` owns the game-state protocol**: snapshots, deltas, NetIds, input
  commands. It mirrors how `Physics` slots in (Core + ECS, render-free).
- **Neither module may depend on `Runtime`** — Runtime links Render
  (`modules/Runtime/CMakeLists.txt`), which would poison the headless server
  link. Consequence: the EntityRef remap contexts used for network-facing
  serialization (`ScopedRawEntityContext`, today in Runtime's
  `SceneSerializer.hpp`) must move down to Core/Reflect or ECS when NetSync
  needs them (small, self-contained relocation; disk-level-file serialization
  stays in Runtime).
- `App` links both and owns the glue (loop integration, input sampling).
- **Caveat the review surfaced: the two-module story is true for NetSync's own
  code but NOT yet for the server *binary*.** `Assisi::App` links
  Render/Window/Debug PUBLIC (`modules/App/CMakeLists.txt:24-32`) and
  `assisi_link_reflections` force-links every `-Generated` object lib globally
  (`cmake/AssisiReflect.cmake:113-118`) — including `Runtime-Generated`, whose
  symbols resolve through Render → nvrhi/Vulkan/GLFW. So any server built on
  `Application` links the whole graphics stack today. This is a *decision*,
  not an accident to discover later — see Stage 2.

Root `CMakeLists.txt` add order: `modules/Net` after Core (it only needs Core),
`modules/NetSync` after ECS; both before App.

## Stage 0 — build integration (the risk stage)

Goal: `GameNetworkingSockets::static` links into a hello-world echo test on
Linux + Windows, static, warnings-clean. Everything follows the existing
FetchContent conventions (shared `out/deps` cache, `SYSTEM`, options forced via
`CACHE ... FORCE`, alias if the target isn't namespaced) — the FreeType and
Jolt blocks are the templates.

**Dependency chain to satisfy:**

1. **protobuf — current release line, with abseil** (v35.x-era at time of
   writing; pin the newest tag GNS's CI is known-good against). Rationale for
   eating abseil up front (decided 2026-07-20, reversing an earlier
   pin-21.12 plan): 21.x is ~4 years old and out of support — no security
   backports for a parser that eats network bytes — and old protobuf under
   current GCC/MSVC is its own build-pain source. Newer lines also carry the
   CMake/FetchContent hygiene the GNS ≥1.5 integration fixes rely on. Abseil
   costs ~130+ CMake targets and a protobuf↔abseil version pair that must bump
   in lockstep; accepted. Set `protobuf_ABSL_PROVIDER` appropriately (fetch
   abseil ourselves first, or let protobuf's build provide it — decide at the
   pinned SHA). Options: `protobuf_BUILD_TESTS OFF`, `protobuf_INSTALL OFF`,
   `protobuf_WITH_ZLIB OFF`, `protobuf_BUILD_PROTOC_BINARIES ON` (GNS codegen
   needs `protoc` at build time; FetchContent builds it as a host tool
   automatically). Abseil requires the same C++ standard/flags as its
   consumers — it inherits the tree's standard; don't special-case it.
2. **Crypto backend — per-platform, no new fetched dep:**
   - Windows: `USE_CRYPTO=BCryptOS` AES via the OS, 25519 via GNS's bundled
     reference implementation. Zero external deps.
   - Linux: system OpenSSL via `find_package(OpenSSL)`. This is a deliberate,
     documented exception to the "fully self-contained" rule — same class of
     exception as GLFW's system X11 libs, and OpenSSL is universally present on
     Linux dev machines. (Fully-self-contained alternative if this ever chafes:
     libsodium via a CMake-ported mirror.)
   - Exact option names/values (`USE_CRYPTO`, `USE_CRYPTO25519`) must be
     verified against `BUILDING.md` at the pinned SHA before writing the block
     — do not trust these notes over the tree.
3. **GNS itself**: `FetchContent_Declare` at a **pinned recent `master`
   commit SHA** (not the 2021 v1.6.0 tag — the protobuf-compat fixes on
   `master` are load-bearing for the current-line protobuf plan above),
   `SYSTEM`. Force `BUILD_STATIC_LIB ON` / shared off (global
   `BUILD_SHARED_LIBS OFF` should handle most of it, but GNS has its own
   options — force them explicitly). Link `GameNetworkingSockets::static`
   directly from `modules/Net` (the Jolt pattern — not via `Assisi::Deps`).
   Record the pinned SHA + date in the FetchContent block comment.

**Known risk & contingencies.** GNS's CMake locates protobuf via
`find_package`; making it resolve to a FetchContent'd subproject is the one
part that may fight back (GNS ≥1.5 explicitly improved this, but it's the
untested edge). Timebox it. Escalation ladder if it stalls:
(a) `FETCHCONTENT_TRY_FIND_PACKAGE_MODE` / CMake dependency-provider shim so
`find_package(Protobuf)` resolves to the fetched tree; (b) **fall back to
protobuf 21.12** — the last pre-abseil line, drastically fewer moving parts;
known-unsupported (Dec 2022, no security backports), so if taken it becomes a
tracked debt item, not a resting state; (c) build protobuf(+abseil)+GNS once
via `ExternalProject` into `out/deps/install` and `find_package` from there;
(d) worst case, vcpkg *for GNS only* — grudging, but contained. Terminal
fallback (new, from the transport survey): enet6 + DTLS behind the unchanged
`Assisi::Net` interface.

**Definition of done:** a `modules/Net/tests` doctest spins up a GNS listen
socket + client on loopback, echoes one reliable and one unreliable message,
and passes on both platforms. (There is no CI — see `remaining-work.md` §4c —
so "both platforms" means built and run by hand on each; the Windows side has
never been built at all, which makes this stage the first thing that would
surface it.)

## Stage 1 — `Assisi::Net`: the transport wrapper

Shape copied from `PhysicsWorld`: one class, **pimpl over the GNS headers** so
third-party types never appear in module headers (keeps `-Werror` hygiene and
makes the transport swappable in fact, not just in principle — the survey's
fallback path depends on this boundary staying clean).

```cpp
// modules/Net/include/Assisi/Net/NetTransport.hpp — sketch
namespace Assisi::Net {

using ConnectionId = std::uint32_t;              // dense handle, not GNS's HSteamNetConnection
inline constexpr ConnectionId InvalidConnection = 0;

enum class SendMode : std::uint8_t { Reliable, Unreliable };

// Lanes map to GNS lanes: snapshots must never head-of-line-block behind bulk
// reliable data. Fixed small set; extend as needed.
enum class Lane : std::uint8_t { Control = 0, Snapshot = 1, Bulk = 2 };

struct NetEvent {
    enum class Type : std::uint8_t { Connected, Disconnected, Message };
    Type type;
    ConnectionId connection;
    std::vector<std::byte> payload;              // Message only
    Lane lane;                                   // Message only
};

class NetTransport {
public:
    bool Listen(std::uint16_t port);             // server
    ConnectionId Connect(std::string_view address, std::uint16_t port); // client
    std::pair<ConnectionId, ConnectionId> CreateLoopbackPair();  // listen server:
                                                 // GNS CreateSocketPair, no real socket
    void Close(ConnectionId connection);
    bool Send(ConnectionId connection, std::span<const std::byte> data,
              SendMode mode, Lane lane);
    void Poll(std::vector<NetEvent> &outEvents); // pump RunCallbacks + drain messages
    // + connection stats accessor (RTT, quality) for debug UI later
};

} // namespace Assisi::Net
```

Notes:
- **Threading: none, initially.** GNS runs its own internal service thread for
  the wire; *our* interaction is `Poll()` once per frame on the main thread
  (client) or once per tick (server). This sidesteps the JobSystem entirely for
  v1 — no `Pool::IO` needed. If profiling ever shows `Poll` mattering, it moves
  to a worker with results marshaled through `DrainMain` (the seam is the
  `Poll` call site, nothing else changes). (GNS's internal locking is coarse —
  known contention reports exist at MMO scale — irrelevant at 2-32 players.)
- GNS global init/teardown (`GameNetworkingSockets_Init/Kill`) refcounted in
  the transport ctor/dtor, same pattern as Jolt's globals in `PhysicsWorld`.
- Explicit-width ints throughout (project convention).
- `CreateLoopbackPair` is the listen-server enabler: the embedded server and
  the local client talk through GNS's in-process socket pair — same codepath as
  a remote client, zero latency, no port bound.
- **Testing fixture (DoD teeth, from the ops review):** build the loopback
  pair as a *reusable test fixture* here, not a one-off. Two modes, verified
  against GNS docs: default `CreateSocketPair` = internal buffers (no
  encryption, no simulated conditions — the listen-server path);
  `bUseNetworkLoopback=true` = routed via 127.0.0.1 so GNS's fake-lag/loss
  config values apply — that mode is the latency/loss/jitter soak harness for
  every later stage. Both belong in the Stage-1 test suite.

## Stage 2 — headless Application (dedicated server prerequisite)

Today `Application::Initialize()` (`modules/App/src/Application.cpp:148-206`)
unconditionally brings up Window → RenderSystem (static singleton) → DebugUI →
InputContext → PostProcess, the loop condition is `_window->ShouldClose()`, and
`OnRender` is pure virtual. The split:

- `AppConfig::headless` (bool, default false; CLI `--server` flag and/or
  `game.json` override).
- `Initialize()` splits into `InitializeCore()` (AssetSystem, config, jobs,
  scene — everything sim) and `InitializePresentation()` (window, render,
  DebugUI, input, post-process — everything GPU/window). Headless runs only the
  former. In-place flag on `Application` — not a separate headless class —
  so sim hooks, `SystemRegistry`, and the listen-server embedding stay shared.
- `Run()` gains a headless path: loop condition becomes an internal
  `_closeRequested` flag (`RequestClose()` stops dereferencing `_window`
  unconditionally); skip `PollEvents`/`_input->Poll()`/`RenderFrame()`; pace
  the loop with the existing self-tuning `SleepUntil` (`Application.cpp:247`)
  to the next fixed tick instead of vsync/frame-cap. The fixed-step accumulator
  block is unchanged — it is already presentation-independent
  (`Application.cpp:323-328`, verified).
- `OnRender` stops being required: give it a default no-op body (headless apps
  simply never get called), keep `OnFixedUpdate`/`OnUpdate` as the sim hooks.
- **`RenderSystem`'s static singleton needs no promotion for this** — headless
  simply never calls `RenderSystem::Initialize`.
- Destructor guards: teardown of DebugUI/PostProcess/RenderSystem is already
  gated on `_initialized`; gate it on "presentation initialized" instead.

**Three prerequisite tasks the original plan omitted (codebase audit):**

1. **`SystemContext` input abstraction.** `SystemContext` hard-embeds
   `Window::InputContext &` and `Window::ActionMap &` by reference
   (`SystemRegistry.hpp:48-55`), and the header includes Window headers — the
   game-logic scheduler transitively depends on Window. A headless server has
   neither. This is a `SystemContext` *shape* change plus a
   `SystemRegistry`→Window *link* change, not merely "systems migrate off
   `input.IsKeyDown`". Entangled with Stage 3's input work — **sequence N2 and
   N3 together** around this seam.
2. **`LevelRuntime::LoadLevel` render-free core.** `LoadLevel` takes
   `Render::AssetCache&` + `SceneRenderer&` and calls `cache.Clear()` /
   `InvalidateAssetBindings()` (`LevelRuntime.cpp:41-55`). The render-free core
   (`SceneSerializer::LoadFromFile` + `RebuildSceneBodies`) is separable —
   split it out so the server loads levels without render types.
3. **The headless link-DAG decision (made now, staged):**
   - **v1 (Stages 2-6): accept the fat link.** The server binary links
     Vulkan/GLFW/nvrhi but never initializes them. Gate with a hard DoD:
     **the headless sandbox must boot and tick on a machine with no GPU and no
     `libvulkan` present** (loader is dlopen'd lazily in practice — *prove* it,
     don't assume it). **Verified as built:** under `LD_DEBUG=libs` with
     `DISPLAY` and `WAYLAND_DISPLAY` unset, `--server` loads six shared
     libraries, none of them Vulkan, GL, X11, Wayland or DRM, and `libvulkan`
     is dlopen'd rather than DT_NEEDED so its absence cannot fail startup.
   - **Committed pre-condition for shipping a container/dedicated server
     binary (not a v1 blocker):** (i) move the *pure-data* component structs
     (`MeshRenderer`, `Camera`, light components, `Name`, `Hierarchy`,
     `Lifecycle` — GUID/AssetId fields only, no Render types) out of
     Render-linked Runtime down to a render-free module, leaving
     `SceneRenderer`/binding caches in Runtime; (ii) split `Assisi-App` into a
     render-free core (loop/lifecycle/`SystemRegistry`) + presentation
     library — moving component structs alone is insufficient while App links
     Render PUBLIC; (iii) add a `SUBSET` argument to `assisi_link_reflections`
     so a server exe links only render-free `-Generated` objects.
- Sandbox: `apps/sandbox --server` boots headless, loads a level, ticks
  physics at 60 Hz, logs. No networking yet — this stage is proven by a server
  that simulates nothing-connected, by the no-GPU boot DoD above, and by the
  normal windowed build still working identically.

This refactor is independently valuable (headless sim tests) and lands
before any protocol work so it never blocks on Stage 0's build wrangling.

## Stage 3 — sim tick, input commands & the clock

- **`std::uint64_t simTick`** on `Application`, incremented once per iteration
  of the fixed-step `while (accumulator >= physicsStep)` loop, exposed through
  `SystemContext` alongside `dt`. This is the network clock: snapshots are
  stamped with it, inputs target it.
- **`InputCommand`** — the serializable per-tick input frame (defined in
  NetSync; it's protocol, not windowing):

  ```cpp
  struct InputCommand {
      std::uint64_t tick;
      std::uint32_t actionBits;      // pressed state of bound digital actions
      std::uint8_t  subTickFraction; // when within the tick the input occurred,
                                     // quantized 0-255. Reserved now so CS2-style
                                     // sub-tick evaluation is a later server-side
                                     // refinement with zero wire-format break.
                                     // v1 servers ignore it.
      // analog axes (move/look) quantized later; float for v1
  };
  ```

  Client side (App layer): sampled **once per fixed tick** from `ActionMap` —
  the layer whose own doc comment calls it "a clean injection point for
  networked input" — into a ring buffer, sent to the server (redundantly, last
  N commands per packet, unreliable — input loss is re-covered by the next
  packet; this is the Overwatch-verified pattern). Server side: per-connection
  command queue consumed in `FixedUpdate`; gameplay systems read `InputCommand`
  from `SystemContext` (or a per-player component) instead of touching
  `ActionMap` directly. **Gameplay systems migrating off direct
  `input.IsKeyDown` polling onto actions/commands is the behavioral change this
  stage forces** — do it for player-controlling systems only; editor/debug
  bindings stay direct. (Today `_input->Poll()` runs per *render* frame —
  per-tick sampling is a real behavioral change, shared with Stage 2's
  `SystemContext` work.)
- **Named clock-sync deliverable (was implicit; now explicit).** A small
  `NetClock`: client estimates server time from handshake + RTT and runs its
  input stream **ahead of the server by ½·RTT + one command frame** so commands
  arrive just before the tick that consumes them; server keeps a small
  per-connection input cushion (1-2 commands) and *reports buffer
  depth/starvation* back to the client in snapshot headers. v1 responds
  crudely (client snaps its lead forward/back on drift). **Adaptive time
  dilation** (Overwatch's ~±10% clock scaling to grow/shrink the buffer
  smoothly) is deferred to Stage 7 — but the buffer-depth telemetry that
  drives it is built here, so dilation later changes only the client's
  response, not the protocol.

## Stage 3½ — ECS: stamping queries (`QueryMut`) — **gates Stage 5, land early**

Re-ordered out of Stage 5 (where v1 of this doc parked it as "the change-tick
landmine") to a standalone ECS task right after Stage 3. Both the theory and
codebase research converged on the same resolution, and the adversarial review
settled the one disagreement:

- **The substrate already exists and is tested** — `SparseSet` per-entity tick
  lane (swap-remove keeps it aligned; untracked pools pay nothing),
  `Scene::_changeTick`, `GetMut`/`Add`/`MarkChanged` stamping,
  `Changed<T>(since)`, `TestChangeDetection.cpp`. The *only* hole:
  `Query::operator*` yields raw mutable `T&` with no stamp
  (`Query.hpp:62-65`), and the header's own docs invite in-place writes.
- **Fix: additive `Scene::QueryMut<Ts...>()`** whose iterator yields
  `Mut<T>` write-proxies that stamp the pool (via the existing
  `TracksChanges()` gate) on non-const access. **Plain `Query` stays exactly
  as it is** — reads and untracked writes keep the ergonomic path; only
  systems writing *tracked* components migrate. (The alternative — flipping
  plain `Query` to yield `const T&` — was explicitly REJECTED by the review:
  it breaks every mutating loop in the engine for a benefit `QueryMut` already
  delivers where it matters.) "GetMut discipline" as a rule is likewise
  rejected — silent desync vs. a compile error is no contest.
- Implementation notes from the code audit: the query iterator holds only pool
  pointers today — it needs a `Scene*`/tick back-pointer added (small; Scene
  already constructs all views). A `Mut`-wrapped read stamps too
  (over-reporting), which is *safe* by the engine's stated change-detection
  stance — wrap only the written types in `QueryMut<...>` and read the rest
  via a plain `Query` alongside.
- Estimate: 1-2 days / ~150-250 LOC for the mechanism. The call-site
  migration (systems that write tracked components through queries —
  `PropagateTransforms` first, which independently wants this) is budgeted
  separately.
- **DoD: a unit test proving a query-mutated tracked component reports
  `Changed(since)`** — plus the existing change-detection suite still green.

## Stage 4 — binary codec over FieldMeta

Wire format for component state. JSON stays for level files; the network (and
eventually save games) get a compact little-endian binary codec driven by
reflection metadata that already exists (`FieldMeta::type` enum + byte
`offset`, `modules/Core/.../FieldMeta.hpp` — which also carries
`hasMin/hasMax/minValue/maxValue` and enum sizing: free quantization
parameters when we want them).

- **`Core::BitWriter`/`BitReader` — bit-level from day one** (revised from v1's
  byte-aligned `ByteWriter`). The research is unanimous that the codec's two
  biggest wins — 1-bit per-field "unchanged" flags and smallest-three
  quaternions (2+9+9+9 = 29 bits) — are *impossible* on a byte-aligned writer,
  and retrofitting bit-level under a shipped format is a wire-format +
  protocol-hash rewrite at exactly the moment bandwidth starts hurting. v1
  field *encoders* stay simple (whole-value floats etc.); the *primitive* is
  bit-capable so quantizers slot in later with no format break.
  Bounds-checking discipline is per-read regardless of bit or byte granularity
  — the reader must be fuzz-hardened (below).
- Generic `WriteComponent(meta, ptr, writer)` / `ReadComponent(...)` walking
  `FieldMeta` — handles `Float/Vec3/Quat/Mat4/Enum/EntityRef/AssetId/...`.
  `EntityRef` fields go through the NetId map (Stage 5), the binary analogue of
  the JSON serializer's remap.
- **Wire identity is `ComponentId`, not name.** The JSON serializer keys by
  component *name* (right for disk); the wire codec keys by the dense
  `ComponentId` (resolved once — the registry's name-sorted determinism, locked
  in by the round-6 M4a fix, makes ids identical across same-build binaries;
  the protocol hash verifies it). Per-component block:
  `[ComponentId varint][field-changed bitmask][changed field payloads...]`.
- **One code path for spawn / delta / keyframe / late-join:** full state is
  simply a delta against the *empty baseline* (all-ones change mask) — the
  Quake 3 unification. No separate "full snapshot" format.
- **Protocol hash**: at startup, hash the sorted component names + per-field
  `(name, type, size, quantization-range, quantization-bits)` layout plus a
  codec-version byte into a `std::uint64_t`. Exchanged at handshake; mismatch →
  reject connection. (Extended from v1: layout agreement alone isn't enough —
  two builds quantizing a Vec3 differently corrupt *silently*, so quantization
  params must be inside the hash.) Also send a short human-readable
  build/protocol string alongside the hash for diagnosable rejections.

**DoD (upgraded to teeth):** round-trip every reflected component;
**fuzz the reader** against truncated/bit-flipped buffers (this codec eats
untrusted network bytes — the fuzz harness is committed test code, not
aspiration); protocol-hash mismatch rejects.

## Stage 5 — `Assisi::NetSync`: replication core

The heart. Server and client halves of one protocol; both live in NetSync and
are driven from App's loop.

**Server (`ReplicationServer`)**, runs in `FixedUpdate` after simulation:

- **NetId map**: server-allocated dense `std::uint32_t NetId` ⇄ local `Entity`.
  Local `(index, generation)` handles are *not* cross-machine stable (slot
  reuse depends on local history), so NetId is the only identity on the wire.
  Don't recycle NetIds within a session; 32 bits won't exhaust at this scale
  (generation/predicted bit-packing in NetId explicitly deferred — don't
  over-design v1).
- **Spawn/despawn**: spawns detected against the map (replicated-marker
  component `ACOMP` `Replicated{}` gates what networks at all); despawns hook
  the deferred-destroy drain (`Scene::FlushDestroyed`). Sent on the reliable
  Control lane.
- **Delta replication off change ticks**: per connection, remember
  `lastAckedTick` (scene change tick at last acked snapshot). Each net tick,
  for each replicated component pool: `Changed<T>(entity, lastAckedTick)` →
  include in snapshot. Baseline = last acked state per entity, so late-join and
  packet loss degrade to "resend more", never "desync". (Verified consensus:
  Quake 3, Source, O3DE, Fiedler all land here independently.)
- **Send loop shaped as a priority list from day one** (new): the send step is
  "collect changed entities → (optional sort by accumulated priority) → drain
  into packet up to a byte budget". v1 always sends everything (no accumulator
  implemented — explicitly deferred), but the loop is *structured* so the
  Tribes/Fiedler priority accumulator drops in later with zero protocol
  change. Do not hardcode a for-loop a budget can't interpose on.
- **Snapshot packet**: `simTick` + spawn/despawn section + per-entity changed
  component blocks. Unreliable, Snapshot lane. **Default send rate: 20-30 Hz —
  a configurable divisor of the 60 Hz sim** (revised from "start 60"): sim and
  input stay at 60; *state* at 20-30 Hz is the Source/Godot-normal sweet spot
  at this scale, halves diff-CPU + bandwidth, and interpolation hides it.
  Clamp the config to divisors of `physicsHz` so snapshots land on exact ticks.
- **Late-join burst flow control** (new; the gap behind "late join is fiddly"
  reports elsewhere): the initial full-world baseline for a joining client (or
  32 clients after level change) is a reliable-lane burst that can stall
  snapshots. Paginate the baseline over the Bulk lane at a byte budget per
  tick; the client is "joining" until the baseline drains, then switches live.
  **As built:** the byte budget exists and does spread a large initial world
  over several snapshots (an entity dropped for space is deliberately left out
  of the ack record, so it stays a spawn rather than being counted delivered),
  and a 40-entity world through a 120-byte budget is covered by test. The
  separate Bulk-lane pagination and the explicit "joining" state are not.
- **Untrusted-input hardening (named scope, not a footnote):**
  per-connection command-rate cap (reject floods before the codec runs);
  per-read bounds checks in the codec (Stage 4's fuzz hardening);
  **semantic input validation** — clamp per-tick movement/look deltas to the
  controller's physical maximums, bound command ticks to the server's accepted
  window (a server that blindly applies forged `InputCommand`s is speed-hack
  food; server authority is necessary, not sufficient). GNS's built-in
  encryption + connection handshake covers the wire itself — don't re-invent
  that layer.
- **Component *removal* is a known hole.** Change detection stamps writes, not
  removals (`Scene::Remove<T>`/`RemoveById` never stamp), so a component taken
  off a replicated entity is not replicated away and the client keeps a stale
  one. Surfaced during the Stage-3½ call-site migration. Entity despawn is
  unaffected — that falls out of the acked-set comparison.
- **Transient components are never on the wire** (`serializable=false`, e.g.
  `Physics::RigidBody` = live Jolt handle). Each side rebuilds them locally
  from replicated descriptors — the exact pattern `RebindSceneAssetsAndPhysics`
  already implements for play-mode restore.

**Client (`ReplicationClient`)**, runs in `OnUpdate`:

- Applies snapshots into the scene: NetId→Entity map (client creates local
  entities on spawn; `ReviveAt` is *not* needed here — it's reserved for the
  prediction/rollback stage), component blocks through the codec.
- **Deferred EntityRef resolve** (new, named hazard): a replicated component
  referencing a NetId the client hasn't spawned yet stores the NetId and
  patches the local `Entity` when that NetId arrives — classic ordering bug,
  design it in rather than discover it. (Ties to the `ScopedRawEntityContext`
  relocation noted in module layering.)
- **Interpolation buffer**: hold ~2-3 snapshots (~100 ms at 20-30 Hz — matches
  Source's canonical 100 ms interp), render entities interpolated between the
  two snapshots straddling `renderTime = serverTime - interpDelay`. The
  engine's existing alpha-interpolation machinery
  (`PhysicsWorld::InterpolateTransforms` / `_interpolationAlpha`) is the local
  analogue; remote entities get the same treatment from snapshot pairs instead
  of physics sub-steps. Remote entities have **no local Jolt bodies** in v1
  (kinematic ghosts at most) — the server owns physics.

**Bandwidth envelope (computed, not asserted):** worst case, 64 replicated
entities all moving, unquantized v1 transform delta ≈ 34 B (pos 12 + quat 16 +
~6 overhead): 64 × 34 B × 25 Hz ≈ 54 kB/s ≈ **435 kbps per client** — ~14 Mbps
server upstream at 32 clients. Fine for a hosted dedicated box; *borderline
for a 32-player listen server on home upload* (and fine at ≤8). Post-
quantization (~10 B/entity) it drops to ~130 kbps per client / ~4 Mbps at 32.
Conclusion: v1 unquantized is safe for the target scale; quantization is the
lever if 32-player listen servers matter.

**Definition of done:** headless server + windowed client on one machine;
client connects, receives world (through the paginated-baseline path), sees
server-simulated physics bodies moving smoothly; client input commands drive
an entity on the server; second client joins late and converges; a soak run
under `bUseNetworkLoopback` fake lag/loss (e.g. 150 ms / 5%) stays connected
and converges. **Convergence oracle is within-ε, never byte-exact** — the
engine is deliberately non-deterministic.

## Stage 6 — listen server & sandbox integration

- `ListenServer` = `ReplicationServer` + embedded sim running in the client
  process, local client connected via `CreateLoopbackPair`. **One scene, not
  two**: the host's scene *is* the server scene; the host client renders it
  directly and skips self-interpolation (it is at server time by definition).
  Remote clients connect to the same `ReplicationServer` over UDP.
- Sandbox UI: Host / Join (address field) / Disconnect in the existing ImGui
  chrome; net stats overlay (RTT, loss, snapshot size, input-buffer depth from
  Stage 3's telemetry) from transport stats — feeds the existing debug-UI
  habits.
- Play-mode interplay: hosting implies play mode (`IsSimulating()`); editor
  undo/history stays host-local and out of scope for replication.
- **Reconnect = rejoin** in v1: a dropped client tears down its replicated
  entities and re-enters through the late-join path (empty-baseline +
  paginated download). In-place mid-session state *repair* is deferred —
  named here because it's a known real gap (the Bevy ecosystem needed a
  dedicated add-on for it), not something the base model gives for free.

## Stage 7 — deferred (explicitly not v1)

- **Client-side prediction & reconciliation** for the local player (replay
  input ring vs. authoritative state; `ReviveAt` + raw-handle EntityRef mode
  earn their keep here). Predict *only* the local controller — never shared
  physics objects; that's the determinism tax this architecture exists to
  avoid.
- **Adaptive time dilation** (Overwatch model): client clock scales ~±10% on
  server-reported input-buffer starvation/overfill. Stage 3 already ships the
  telemetry; this is the smooth response.
- **Server-side lag compensation** — a lightweight **per-player transform
  history ring** for rewind hit-tests (cap the rewind window ~200-250 ms),
  *not* full Jolt `SaveState` per hitscan (too heavy); full
  `SaveState`/`RestoreState` through the `PhysicsWorld` pimpl stays the tool
  for rarer whole-world needs. Needs the missing `SetBodyVelocity` exposure.
- **Sub-tick input evaluation** (CS2 model): `InputCommand.subTickFraction` is
  already on the wire from Stage 3; using it (fractional movement integration,
  sub-tick lag-comp rewind) is a server-side refinement for precision-shooter
  feel. Lesson from CS2's rollout: partial sub-tick (hits precise, visuals
  tick-quantized) *feels* inconsistent — do it end-to-end or not at all.
- **Priority accumulator** filling the Stage-5 send-loop seam (per-connection
  accumulated priority, drain to budget) — the proven scaling mechanism if
  entity counts grow.
- **Interest management** (per-client relevance culling) — confirmed
  unnecessary at 2-32 players; the light-culling chunking work may share
  broad-phase structure eventually.
- **Mid-session reconnect repair** (see Stage 6).
- **NAT traversal in production**: GNS ICE needs a rendezvous/signaling
  service (a small web service; doubles as server browser) + STUN, optional
  TURN relay. Direct-IP and LAN work without any of it. Steam path: relink
  against Steamworks → SDR replaces all of this, zero code change (verified).
- Identity/auth (GNS self-signed certs are fine until there's matchmaking).
- Snapshot quantization (the BitWriter primitive from Stage 4 makes this a
  field-encoder change, not a format change: smallest-three quats, bounded
  positions off FieldMeta min/max, velocity ranges).

## Open decisions

Resolved by the 2026-07-22 research review:

1. ~~Change-tick stamping: variant vs. discipline~~ → **additive `QueryMut`**
   (Stage 3½); plain `Query` untouched; discipline-only and const-flip both
   rejected.
2. ~~Net tick rate: 60 vs divisor~~ → **default 20-30 Hz, configurable,
   clamped to divisors of `physicsHz`** (sim & input stay 60).

Still open (defer until the stage that needs them):

3. Where `ScopedRawEntityContext` lands when it leaves Runtime (Core/Reflect
   vs. ECS).
4. Linux crypto: system OpenSSL (planned) vs. fully-fetched libsodium mirror.
5. Whether `apps/sandbox --server` suffices long-term or a separate
   `apps/server` target earns its keep (start with the flag).
6. Which exact GNS `master` SHA to pin (decide at Stage 0 against its CI
   status; record SHA + date in the FetchContent block).

## Stage order & independence

Stages 0 and 2 are independent and can land in either order (0 is
fetch-and-CMake risk, 2 is engine refactor). 1 needs 0. **2 and 3 share the
`SystemContext`/input seam — sequence them together.** 3½ (QueryMut) needs
nothing but ECS and should land right after 3 (it gates 5, and
`PropagateTransforms` wants it independently). 4 needs nothing but Core. 5
needs all of 0-4 + 3½. 6 needs 5. Each stage is a committable, testable
increment; nothing requires a long-lived branch.

**Scope discipline (review verdict):** the additions above are folded into
existing stages as tasks and DoD teeth *deliberately* — testing, security,
clock, and link-DAG work do not get their own ceremonial stages. N0-N6 stays
a short, incrementally-committable plan.
