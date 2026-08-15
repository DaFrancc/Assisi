# Suspected bugs found during the ENG-117 comment sweep

Provenance: ENG-117 swept every comment in the engine against the code beside it.
That sweep was comment-only — **nothing here was fixed, and nothing here was
changed by that commit** (`041f703`). These are the code defects that surfaced as
a side effect of reading every comment in the tree against its implementation.

**Status: unverified leads.** Each entry was found by reading the code, not by
reproducing the failure. None has a failing test behind it. Treat every line
number as a starting point and re-check the claim before acting — the sweep
itself demonstrated that confidently-worded claims about this codebase go stale.

Two entries are explicitly *disproven* claims, recorded at the bottom so nobody
re-investigates them.

---

## 1. Data races and undefined behaviour

### `Chiara/src/Serializer.cpp:610` — unsynchronised read of `session.active`
`PumpSession`'s fast path reads `session.active` without holding `session.mutex`.
`Session::active` (declared at `:425`) is a plain `bool`, and `BeginSession` /
`EndSession` write it under the lock. That is a data race in the formal sense,
not a benign one.

The header compounds it: `Serializer.hpp` describes the fast path as costing "a
few atomic loads", which is a synchronisation the code does not actually have.
The comment was corrected during the sweep; the race was not.

### `Core/src/BitStream.cpp:139` — UB in `WriteFloatQuantized` at 32 bits
With `bits == 32`, `scaled + 0.5f` reaches ~`4294967295.5f`. That value is not
representable in `uint32_t`, so the conversion is undefined at the top of the
range. Every other bit width is fine — the bug is specific to the maximum.

### `Net/src/NetTransport.cpp:289-304` — release builds proceed on an uninitialised library
When `GameNetworkingSockets_Init` fails, the only thing stopping construction is
`ASSISI_ASSERT`, which compiles to nothing in release (`Core/Assert.hpp:78`).
Construction then falls through to `SteamNetworkingSockets()->CreatePollGroup()`
against a library that never initialised.

### `Physics/src/PhysicsWorld.cpp:930` — `GetBodyTransform` has no `IsAdded` guard
It queries `bodies.GetPosition` / `GetRotation` directly. Every sibling accessor
— `GetBodyVelocity`, `IsBodyCCDEnabled`, `RemoveBody` — guards on `IsAdded`
first. An invalid or already-removed `RigidBody` handle therefore reaches Jolt's
body interface, which is undefined rather than refused.
`Physics/tests/TestBodyLifetime.cpp`'s own header comment says as much.

### `Editor/src/EditorAssetBrowser.cpp:315` — `reinterpret_cast` keyed on the wrong discriminator
`SelectAsset` writes an `AssetId` through a `reinterpret_cast` whenever
`_assetBrowserVectorSlot < 0`. But `EditComponentFields` also arms the browser
from the `FieldType::AssetPath` case (`EditorInspector.cpp:409`), so picking a
file for an `AssetPath` field would stamp a GUID over a fixed-capacity string.

**Latent today** — no reflected component currently declares an `AssetPath`
field, which is exactly why the header's "plain AssetPath" wording had gone
unnoticed for so long. It becomes live the moment one does.

---

## 2. Unbounded growth

### `Runtime/src/SceneRenderer.cpp:180` — six containers grow forever if init failed partway
`Render()` returns before any per-frame queue is cleared when `_meshPass` is
invalid. `_submittedIcons`, `_submittedIconOutlines`, both `_overlayLines`
batches, `_outlineGroups` and `_iconSuppressed` all accumulate. Reachable when
`Initialize` fails at the mesh pass but *after* `_editorVisuals` is set
(`:63`) — the submit entry points only gate on `_editorVisuals`.

### `Runtime/src/SceneRenderer.cpp:341` — same shape, narrower
`DrawEditorIcons` returns early on an invalid `_iconPass`, ahead of the
`_submittedIcons.clear()` at `:395`. An editor submitting instance-root icons
every frame grows the vector without bound.

The `SubmitEditorIcons` header comment ("Consumed and cleared each `Render()`")
describes the intent, not this path.

### `Render/src/AssetCache.cpp:361` — a refused bindless slot is retried forever
When `writeDescriptorTable` refuses a slot, `_nextBindlessSlot` is not advanced.
Every subsequent texture retries the same rejected slot and re-logs the same
error. Benign in effect, unbounded in volume.

### `Chiara/src/Chiara.cpp:314` — failed buffer allocation burns a thread slot permanently
If `new Event[capacity]` fails, `RegisterThreadBuffer` returns null having
already consumed a `g_threadCount` index and leaked the `ThreadBuffer`. Because
`t_buffer` stays null, every later emit on that thread retries the slow path.

---

## 3. Resource leaks

### `Render/src/AssetCache.cpp:1076` — `Clear()` bypasses `ThumbnailReleaseFn`
`Clear()` drops `_thumbnails` without invoking any `ThumbnailReleaseFn`. Called
on the editor's thumbnail cache, the ImGui descriptor-set bindings leak and can
alias a later texture allocated at the same address — precisely the hazard that
`ClearThumbnails` / `ThumbnailReleaseFn` exist to prevent.

The comment there asserts this cache is always the scene one and therefore always
empty. That is an assumption the code does not enforce.

---

## 4. Wrong logic

### `NetSync/src/NetClock.cpp:44` — the overfull-buffer shrink inverts its own intent
The shrink is guarded with `target > excess` rather than a floor on `target`. A
*deeply* overfull queue (large `excess`) therefore skips the decrement entirely;
the lead only shrinks when the buffer is barely over target. The adjacent comment
describes "shrink one tick at a time", which is the opposite of what happens in
the case that matters most.

### `NetSync/src/InputCommand.cpp:164` — lookahead cannot reject on a fresh connection
With nothing applied yet, `horizon = command.tick`, so `kMaxQueueLookahead` has
nothing to reject against. The first command a client sends fixes the queue's
tick base at whatever value it claims.

### `Core/src/MessageRegistry.cpp:52` — duplicates are diagnosed but not removed
`EnsureFinalized` logs and asserts on duplicate message names, then keeps them.
`ComponentRegistry` handles the same situation with `std::unique` + `erase`. So a
release build retains both entries and `Find` / `IdOf(name)` silently resolves to
whichever the unstable sort happened to place first.

### `Core/src/ComponentRegistry.cpp:191` — `Count()` is the one accessor that skips finalize
Every other accessor calls `EnsureFinalized()` first. `Count()` does not, so
before the first finalize it reports a count still including the duplicates that
finalize will drop. `MessageRegistry::Count()` does finalize, so the two
registries disagree.

### `Core/src/AssetDatabase.cpp:39` — an empty file reads as an I/O error
`ReadWholeFile` does `buffer << stream.rdbuf()`, which sets `failbit` when it
inserts zero characters. The `!good() && !eof()` check therefore treats a
zero-byte file as unreadable. A zero-byte `.aast` is logged "cannot read sidecar"
and its asset skipped, rather than reported as malformed.

### `Geometry/include/Assisi/Geometry/MeshData.hpp:88`, `:129` — indices are range-checked, not bounds-checked
`ComputeAabb` and `ComputeBoundingSphere` validate the *index range* against
`Indices.size()`, but never check `Indices[i] < Vertices.size()`. A glTF carrying
an out-of-range index reads past the vertex array.

### `Editor/src/EditorPlay.cpp:806` — a tooltip escapes its `#if` guard
The net-mode combo's `ImGui::IsItemHovered` tooltip block sits outside the
`#if defined(ASSISI_NETWORKING)` guard that draws the combo. In a non-networking
build it attaches to the preceding item — the Run button — and shows "Host + N"
there.

### `apps/sandbox/src/DemoSystems.cpp:67,72` — misattributed log prefix
`BouncerSpawnSystem` logs under the `"InputDemo:"` prefix, attributing its
messages to the other demo system.

---

## 5. Codegen and tooling

### `tools/reflectgen/reflectgen.py:499` — generated file names a flag that does not exist
The empty-registration file reflectgen writes tells the reader handlers are bound
by `--message-handlers`. argparse defines no such flag; the real one is
`--check-handlers`. It lives inside a Python string literal — generated output,
not a comment — so the comments-only rule left it alone. One-word fix.

### `tools/reflectgen/reflect_codegen.py:40-46` — `NUMERIC_BOUND_RANGES` is wrong at both ends
It carries an `'Int'` key that is not a `FieldType` value (dead entry), and omits
`Int64` / `UInt64`. So `AFIELD(min=/max=)` on an `int64_t` or `uint64_t` field is
rejected with the misleading message "bounds only apply to numeric fields".

### `tools/reflectgen/blueprint_views.py:531` — duplicate sources are not rejected
`generate` rejects duplicate *type names*, but not two type names pointing at the
same `.abp` source. `kGeneratedInstanceViews` is keyed by source, so two such
entries would make `TestInstanceViews` check the same blueprint twice under
different views. Low severity.

---

## 6. Tests that do not test what they claim

These are test-integrity problems, not product bugs. Each one currently passes.

### `Core/tests/TestJobSystem.cpp:403` — the precondition is destroyed by the previous case
"A JobSystem built with the capture runtime down is harmless" asserts behaviour
with Chiara uninitialised. But the case at `:371` calls `Chiara::Initialize()`
and never `Shutdown()`, so in a normal in-order run the capture runtime is *up*
and this case exercises nothing it names.

### `Geometry/tests/TestAssetImport.cpp:464` — the two roots are one directory
`root2` resolves to the same path as `root`, because `WriteMaterialAssets` always
uses the fixed directory `assisi_assetimport_test`. The "reverse" half of the
Added/Removed test therefore reuses the recreated first root, and both
`remove_all` calls target one path. The assertions still hold; the isolation the
test reads as having is not real.

### `NetSync/tests/TestRelevancy.cpp:117` — dead harness affordance
`Harness::dropServerMessages` is documented and implemented, and set by no test.

### `NetSync/tests/TestRelevancy.cpp:600` — narrower than its name
"priority does not climb for entities outside the set" asserts only convergence,
never the accumulator. The comment at `:621` was corrected during the sweep to
say so rather than imply otherwise; the coverage gap stands.

---

## 7. Fragility, not yet a bug

### `Core/include/Assisi/Core/AssetSystem.hpp:201` — missing `<optional>`
`ExecutablePath()` returns `std::optional<std::filesystem::path>`, but the header
includes no `<optional>`. It compiles only through a transitive include and will
break on an unrelated include change.

### `Runtime/src/Hierarchy.cpp:81` — detach leaves a stale `worldMatrix`
The existing comment already admits that detaching via `Remove<Parent>` stamps
nothing, so the world matrix goes stale. It claims "no such site today". That
claim was **not** verified across the editor during the sweep.

---

## Disproven — do not re-investigate

Recorded because both were believed true going in, and both cost investigation
time before being ruled out.

| Claim | Verdict |
|---|---|
| `ServerApp` exits 0 on five failed-start paths | **Already fixed.** All five refusal paths set `_startupFailed` (`ServerApp.cpp:92`, `:100`, `:121`, `:154`, `:178`, plus the `BuildJoinedWorld` funnel at `:198`); `OnStart()` runs inside `Application::Run()` (`App/src/Application.cpp:354`); `apps/sandbox/src/main.cpp:281` returns `StartupFailed() ? EXIT_FAILURE : EXIT_SUCCESS`. |
| `Core/src/ContentHash.cpp:17-19`'s read-error check is dead code | **Not dead.** The function no longer uses an `istreambuf_iterator` pair — it reads through `std::ifstream::read` in a chunk loop and checks `bad()`. `istream::read`'s sentry turns libstdc++'s `underflow` failure into `badbit`, so the check is live, and `TestContentHash.cpp:99` exercises it. |
