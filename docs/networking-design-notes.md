# Networking — design notes & implementation plan

Captured 2026-07-20. **Nothing here is built yet.** This records the decided
networking architecture and the staged path to it, so the work can start (and
pause) at well-defined seams. Feasibility survey conclusions are baked in rather
than re-argued; the short version of each decision is in "Decisions" below.

## Decisions

- **Model: server-authoritative state replication with snapshot interpolation**
  (Quake/Overwatch lineage). *Not* deterministic lockstep: the build ships
  `-ffast-math` in Release (`CMakeLists.txt` `ASSISI_ENABLE_FAST_MATH`), forces
  AVX2+FMA, and builds Jolt without `CROSS_PLATFORM_DETERMINISTIC` — three
  deliberate perf choices that lockstep would have to fight. State replication
  needs none of them reverted.
- **Topologies: dedicated server and listen server, where the listen server is
  a dedicated server embedded in the client process.** One server codepath. P2P
  lockstep is out of scope permanently; P2P *topology* (player-hosted) is just
  the listen server.
- **Transport: Valve GameNetworkingSockets (GNS)**, open-source build, pinned
  release (v1.6.0 at time of writing). Chosen over enet6 (no crypto, no NAT
  traversal) and yojimbo (no NAT traversal, token model assumes a backend).
  GNS uniquely covers the listen-server-behind-home-NAT case (ICE) and gives a
  zero-code-change upgrade path to Steam Datagram Relay by relinking against
  Steamworks. Cost: protobuf + crypto transitive deps (Stage 0 is the risk
  stage; contingencies listed there).
- **Replication layer: built in-engine**, on top of the primitives that already
  exist for it: stable `ComponentId` wire identity, `ComponentRegistry` /
  `ComponentMeta` type-erased hooks, per-component change ticks
  (`ACOMP(tracked)`), `Registry::ReviveAt`, and the raw-handle EntityRef mode.
  No off-the-shelf replication framework fits this ECS.

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
   pinned tag). Options: `protobuf_BUILD_TESTS OFF`, `protobuf_INSTALL OFF`,
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
     verified against `BUILDING.md` at the pinned tag before writing the block
     — do not trust these notes over the tag.
3. **GNS itself**: `FetchContent_Declare` at the pinned release tag,
   `GIT_SHALLOW TRUE`, `SYSTEM`. Force `BUILD_STATIC_LIB ON` / shared off
   (global `BUILD_SHARED_LIBS OFF` should handle most of it, but GNS has its
   own options — force them explicitly). Link `GameNetworkingSockets::static`
   directly from `modules/Net` (the Jolt pattern — not via `Assisi::Deps`).

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
(d) worst case, vcpkg *for GNS only* — grudging, but contained.

**Definition of done:** a `modules/Net/tests` doctest spins up a GNS listen
socket + client on loopback, echoes one reliable and one unreliable message,
and passes in CI on both platforms.

## Stage 1 — `Assisi::Net`: the transport wrapper

Shape copied from `PhysicsWorld`: one class, **pimpl over the GNS headers** so
third-party types never appear in module headers (keeps `-Werror` hygiene and
makes the transport swappable in principle).

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
  `Poll` call site, nothing else changes).
- GNS global init/teardown (`GameNetworkingSockets_Init/Kill`) refcounted in
  the transport ctor/dtor, same pattern as Jolt's globals in `PhysicsWorld`.
- Explicit-width ints throughout (project convention).
- `CreateLoopbackPair` is the listen-server enabler: the embedded server and
  the local client talk through GNS's in-process socket pair — same codepath as
  a remote client, zero latency, no port bound.

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
  former.
- `Run()` gains a headless path: loop condition becomes an internal
  `_closeRequested` flag (`RequestClose()` stops dereferencing `_window`
  unconditionally); skip `PollEvents`/`_input->Poll()`/`RenderFrame()`; pace
  the loop with the existing self-tuning `SleepUntil` (`Application.cpp:247`)
  to the next fixed tick instead of vsync/frame-cap. The fixed-step accumulator
  block is unchanged — it is already presentation-independent.
- `OnRender` stops being required: give it a default no-op body (headless apps
  simply never get called), keep `OnFixedUpdate`/`OnUpdate` as the sim hooks.
- **`RenderSystem`'s static singleton needs no promotion for this** — headless
  simply never calls `RenderSystem::Initialize`. The singleton's own header
  already documents when to promote it (a headless *device*, e.g. GPU tests);
  that is explicitly not this stage.
- Destructor guards: teardown of DebugUI/PostProcess/RenderSystem is already
  gated on `_initialized`; gate it on "presentation initialized" instead.
- Sandbox: `apps/sandbox --server` boots headless, loads a level, ticks
  physics at 60 Hz, logs. No networking yet — this stage is proven by a server
  that simulates nothing-connected, and by the normal windowed build still
  working identically.

This refactor is independently valuable (headless CI sim tests) and lands
before any protocol work so it never blocks on Stage 0's build wrangling.

## Stage 3 — sim tick & input commands

- **`std::uint64_t simTick`** on `Application`, incremented once per iteration
  of the fixed-step `while (accumulator >= physicsStep)` loop
  (`Application.cpp:311-316`), exposed through `SystemContext` alongside `dt`.
  This is the network clock: snapshots are stamped with it, inputs target it.
- **`InputCommand`** — the serializable per-tick input frame (defined in
  NetSync; it's protocol, not windowing):

  ```cpp
  struct InputCommand {
      std::uint64_t tick;
      std::uint32_t actionBits;      // pressed state of bound digital actions
      // analog axes (move/look) quantized later; float for v1
  };
  ```

  Client side (App layer): sampled **once per fixed tick** from `ActionMap` —
  the layer whose own doc comment calls it "a clean injection point for
  networked input" — into a ring buffer, sent to the server (redundantly, last
  N commands per packet, unreliable — input loss is re-covered by the next
  packet). Server side: per-connection command queue consumed in
  `FixedUpdate`; gameplay systems read `InputCommand` from `SystemContext` (or
  a per-player component) instead of touching `ActionMap` directly. **Gameplay
  systems migrating off direct `input.IsKeyDown` polling onto actions/commands
  is the behavioral change this stage forces** — do it for player-controlling
  systems only; editor/debug bindings stay direct.

## Stage 4 — binary codec over FieldMeta

Wire format for component state. JSON stays for level files; the network (and
eventually save games) get a compact little-endian binary codec driven by
reflection metadata that already exists (`FieldMeta::type` enum + byte
`offset`, `modules/Core/.../FieldMeta.hpp`):

- `Core::ByteWriter`/`ByteReader` (bounds-checked, explicit-width, LE).
- Generic `WriteComponent(meta, ptr, writer)` / `ReadComponent(...)` walking
  `FieldMeta` — handles `Float/Vec3/Quat/Mat4/Enum/EntityRef/AssetId/...`.
  `EntityRef` fields go through the NetId map (Stage 5), the binary analogue of
  the JSON serializer's remap.
- **Protocol hash**: at startup, hash the sorted component names + per-field
  `(name, type, size)` layout into a `std::uint64_t`. Exchanged at handshake;
  mismatch → reject connection. This is what makes "same component set on both
  ends" (the precondition for `ComponentId` as wire identity) *checked* rather
  than assumed.
- Quantization (pos/quat/velocity bit-packing) is a later optimization pass —
  the codec API takes the component, not the bits, so it can change underneath.

Unit-testable in isolation (round-trip every reflected component; fuzz the
reader against truncated buffers).

## Stage 5 — `Assisi::NetSync`: replication core

The heart. Server and client halves of one protocol; both live in NetSync and
are driven from App's loop.

**Server (`ReplicationServer`)**, runs in `FixedUpdate` after simulation:

- **NetId map**: server-allocated dense `std::uint32_t NetId` ⇄ local `Entity`.
  Local `(index, generation)` handles are *not* cross-machine stable (slot
  reuse depends on local history), so NetId is the only identity on the wire.
- **Spawn/despawn**: spawns detected against the map (replicated-marker
  component `ACOMP` `Replicated{}` gates what networks at all); despawns hook
  the deferred-destroy drain (`Scene::FlushDestroyed`). Sent on the reliable
  Control lane.
- **Delta replication off change ticks**: per connection, remember
  `lastAckedTick` (scene change tick at last acked snapshot). Each net tick,
  for each replicated component pool: `Changed<T>(entity, lastAckedTick)` →
  include in snapshot. Baseline = last acked state per entity, so late-join and
  packet loss degrade to "resend more", never "desync".
- **Snapshot packet**: `simTick` + spawn/despawn section + per-entity changed
  component blocks (ComponentId + codec bytes). Unreliable, Snapshot lane, sent
  at tick rate (configurable divisor of `physicsHz` later).
- **Transient components are never on the wire** (`serializable=false`, e.g.
  `Physics::RigidBody` = live Jolt handle). Each side rebuilds them locally
  from replicated descriptors — the exact pattern `SandboxPlay`'s
  `RebindSceneAssetsAndPhysics` already implements for play-mode restore.

**Client (`ReplicationClient`)**, runs in `OnUpdate`:

- Applies snapshots into the scene: NetId→Entity map (client creates local
  entities on spawn; `ReviveAt` is *not* needed here — it's reserved for the
  prediction/rollback stage), component blocks through the codec.
- **Interpolation buffer**: hold ~2-3 snapshots (~100 ms), render entities
  interpolated between the two snapshots straddling `renderTime = serverTime -
  interpDelay`. The engine's existing alpha-interpolation machinery
  (`PhysicsWorld::InterpolateTransforms` / `_interpolationAlpha`) is the local
  analogue; remote entities get the same treatment from snapshot pairs instead
  of physics sub-steps. Remote entities have **no local Jolt bodies** in v1
  (kinematic ghosts at most) — the server owns physics.

**The change-tick landmine (fix first, in ECS):** `Query` yields mutable
references without stamping, and plain `Get<T>` doesn't stamp — any system
mutating a tracked component through a query silently produces no delta.
Before replication ships, either (a) add a stamping query variant / stamp
tracked pools on mutable query access, or (b) establish and *enforce* (debug
assert / review rule) that replicated components are only written via
`GetMut`/`MarkChanged`. Decide at implementation time; (a) is safer, (b) is
cheaper. This is a Stage 5 *blocker*, listed here so it isn't discovered in a
desync hunt.

**Definition of done:** headless server + windowed client on one machine;
client connects, receives world, sees server-simulated physics bodies moving
smoothly; client input commands drive an entity on the server; second client
joins late and converges.

## Stage 6 — listen server & sandbox integration

- `ListenServer` = `ReplicationServer` + embedded sim running in the client
  process, local client connected via `CreateLoopbackPair`. **One scene, not
  two**: the host's scene *is* the server scene; the host client renders it
  directly and skips self-interpolation (it is at server time by definition).
  Remote clients connect to the same `ReplicationServer` over UDP.
- Sandbox UI: Host / Join (address field) / Disconnect in the existing ImGui
  chrome; net stats overlay (RTT, loss, snapshot size) from transport stats —
  feeds the existing debug-UI habits.
- Play-mode interplay: hosting implies play mode (`IsSimulating()`); editor
  undo/history stays host-local and out of scope for replication.

## Stage 7 — deferred (explicitly not v1)

- **Client-side prediction & reconciliation** for the local player (replay
  input ring vs. authoritative state; `ReviveAt` + raw-handle EntityRef mode
  earn their keep here).
- **Server-side lag compensation** — needs Jolt `SaveState`/`RestoreState`
  exposed through `PhysicsWorld` (pimpl extension) plus the missing
  `SetBodyVelocity`.
- **Interest management** (per-client relevance culling) — the light-culling
  chunking work may share broad-phase structure eventually.
- **NAT traversal in production**: GNS ICE needs a rendezvous/signaling
  service (a small web service; doubles as server browser) + STUN, optional
  TURN relay. Direct-IP and LAN work without any of it. Steam path: relink
  against Steamworks → SDR replaces all of this, zero code change.
- Identity/auth (GNS self-signed certs are fine until there's matchmaking).
- Snapshot quantization & bandwidth budgets.

## Open decisions (defer until the stage that needs them)

1. Change-tick stamping for queries: stamping query variant vs. GetMut
   discipline (Stage 5 blocker; leaning (a) stamping variant).
2. Net tick rate: replicate at full 60 Hz `physicsHz` or a divisor (start 60,
   make configurable).
3. Where `ScopedRawEntityContext` lands when it leaves Runtime (Core/Reflect
   vs. ECS).
4. Linux crypto: system OpenSSL (planned) vs. fully-fetched libsodium mirror.
5. Whether `apps/sandbox --server` suffices long-term or a separate
   `apps/server` target earns its keep (start with the flag).

## Stage order & independence

Stages 0, 2 are independent of each other and can land in either order (0 is
fetch-and-CMake risk, 2 is engine refactor). 1 needs 0. 3 and 4 need nothing
but are pointless before 5 is in sight. 5 needs all of 0-4. 6 needs 5. Each
stage is a committable, testable increment; nothing requires a long-lived
branch.
