# R2 — Transport-Library Survey for Assisi::Net (2026)

Audit of the transport choice recorded in `docs/networking-design-notes.md` (Stage
0 picks Valve **GameNetworkingSockets / GNS**, pinned v1.6.0, over enet6 and
yojimbo). Engine constraints: C++23, CMake FetchContent for all deps, Linux +
Windows, open-source-friendly license (public release intended), performance-first,
small-scale (2–32 players), **server-authoritative snapshot replication built
in-engine**. The transport needs only: connection-oriented UDP, reliable +
unreliable messages, lanes (so snapshots don't head-of-line-block behind bulk
reliable), encryption (ideally), decent congestion control, IPv6, and an
in-process loopback pair for the listen server.

---

## TL;DR verdict

- **Confirm GNS as the primary transport** — it is still the only single library
  that satisfies *every* hard requirement (message + connection oriented,
  lanes, authenticated encryption, congestion control, IPv6, in-process
  `CreateSocketPair` loopback, plus a near-free Steam Datagram Relay upgrade
  path). Nothing else is close on coverage.
- **But the design doc's *justification* is wrong in one specific way.** The
  one-line rejections of enet6/yojimbo ("no NAT traversal") do **not** hold up as
  the deciding reason (see §7). Standalone GNS's ICE also needs *your own*
  signaling service, and Assisi defers NAT to Stage 7 anyway. The genuine reasons
  to pick GNS are **built-in authenticated encryption + lanes + congestion
  control + the loopback pair + the Steam path** — re-baseline the doc on those.
- **One factual correction for Stage 0:** GNS's last *tagged release* is **v1.6.0,
  dated June 3 2024** — not 2026. `master` is actively developed through 2026, but
  pinning "the newest release" pins ~2-year-old code. Decide explicitly: pin
  v1.6.0, or pin a recent `master` commit (recommended — carries the ICE/TURN and
  protobuf-compat fixes).
- **Keep the `Assisi::Net` pimpl abstraction.** It's cheap and it is the hedge
  against the one real risk here: Stage 0's protobuf/abseil build integration.

---

## 1. GameNetworkingSockets (GNS) — deep dive

**Repo / license:** <https://github.com/ValveSoftware/GameNetworkingSockets> —
**BSD-3-Clause**. Public-release-safe.

**Maintenance status (verified 2026):**
- Last *tagged release*: **v1.6.0, 2024-06-03** (native ICE client out of beta;
  P2P enabled by default; **TURN RFC 5766** + IPv6 added). Prior: v1.5.1
  (2024-05-04), v1.5.0 (2024-04-28, which explicitly *"fixed compatibility issues
  with newer versions of protobuf and abseil"*).
  Source: <https://github.com/ValveSoftware/GameNetworkingSockets/releases>
- `master` is **actively developed**: the commits view shows ongoing 2025–2026
  work by `zpostfacto` (Fletcher Dunn, Valve) — recent ICE client / TURN
  long-term-credential auth, SNP ("stop waiting") improvements, more connection
  tests. Source:
  <https://github.com/ValveSoftware/GameNetworkingSockets/commits/master>
- **Interpretation:** the open-source repo is *maintained*, not abandoned — but on
  a slow release cadence (no tag in ~2 years). The standalone (non-Steam) path is
  genuinely supported: it's the same maintainer who owns the Steam version, and
  ICE/TURN work is happening *in the open-source tree*. This is materially better
  maintenance health than yojimbo (dormant since 2024) or the RakNet lineage
  (dead).

**API fit — best in class for this engine:**
- Message-oriented **and** connection-oriented (`ISteamNetworkingSockets`,
  handle-based connections). ✔
- **Lanes** (`ConfigureConnectionLanes`) — exactly the snapshot-vs-bulk HOL
  separation the doc's `Lane` enum maps to. ✔ (Few other libs have this.)
- Reliable + unreliable messages, message fragmentation/reassembly. ✔
- **Encryption built in and on by default:** AES-GCM-256 per packet, Curve25519
  key exchange + cert signatures, per-packet IV derived à la Google QUIC. ✔
  Source (feature summary):
  <https://github.com/ValveSoftware/GameNetworkingSockets>
- **Congestion control:** its own SNP layer (QUIC-derived), tuned by Valve for
  real game traffic at scale — the strongest of any option here. ✔
- **IPv6:** ✔ (dual-stack).
- **Loopback pair:** `CreateSocketPair` gives two connected in-process
  `HSteamNetConnection`s with no real socket — this *is* the listen-server
  enabler the doc relies on, and it is a first-class API, not a hack. ✔

**Known problems / build pain (well-documented, real):**
- The protobuf + abseil chain is the sore spot. Documented failure modes:
  - `abseil_dll` link errors / unresolved abseil symbols when protobuf and
    consumer disagree on C++ standard —
    <https://github.com/protocolbuffers/protobuf/issues/17257>,
    <https://github.com/protocolbuffers/protobuf/issues/12637>
  - "Protobuf only supports Abseil 20230125.3 and newer" version-lockstep —
    <https://github.com/protocolbuffers/protobuf/issues/14561>
  - Runtime `Invalid file descriptor data passed to
    EncodedDescriptorDatabase::Add()` from mismatched protobuf builds (recurs via
    vcpkg into 2025) —
    <https://github.com/ValveSoftware/GameNetworkingSockets/issues/380>
  - xmake/vcpkg packaging breakages tracking the same chain —
    <https://github.com/xmake-io/xmake-repo/issues/3710>
  - GNS locates protobuf via `find_package(Protobuf)`; making that resolve to a
    FetchContent'd subproject is the exact edge Stage 0 already flags. GNS ≥1.5
    improved this but it remains the untested part.
- **WebRTC/ICE story:** GNS historically used Google's WebRTC for ICE (huge,
  painful dep). Since v1.4.1→v1.6.0 it ships a **native ICE client** (no WebRTC
  needed) and it's now the default. So the old "GNS drags in WebRTC" complaint is
  **obsolete** — you get ICE/TURN without the WebRTC monster. Good.
- **Reliability/CC quality:** high. Used in Steam-scale production; the SNP
  reliability layer and bandwidth estimation are more battle-tested than
  yojimbo/reliable.io for large connection counts.

**Steam Datagram Relay upgrade path — verified, with an asterisk:**
- The Steamworks doc confirms the standalone lib exposes *"the same names for
  everything, the same semantics, the same behavioral quirks"* as
  `ISteamNetworkingSockets` in the Steamworks SDK, and states the open-source
  release exists *specifically* so devs code to the identical API. Sources:
  <https://partner.steamgames.com/doc/features/multiplayer/networking>,
  <https://partner.steamgames.com/doc/api/ISteamnetworkingSockets>
- **Asterisk:** the *open-source* code **cannot access the SDR relay network**.
  To get SDR you link the **Steamworks** build and ship on Steam (Valve provides a
  console/other-platform build on request). Source:
  <https://partner.steamgames.com/doc/features/multiplayer/steamdatagramrelay>
- **Net:** the "zero-code-change swap to SDR" claim is **API-accurate** — you swap
  the linked library, not your calls — but it is *not* free of process: it's a
  Steam-distribution decision, not a pure code toggle. Fair to keep as a genuine
  strategic advantage; just don't oversell it as costless.

---

## 2. yojimbo + netcode.io + reliable.io (Glenn Fiedler / Más Bandwidth)

**Repos:** <https://github.com/mas-bandwidth/yojimbo> (built on
`mas-bandwidth/netcode`, `mas-bandwidth/reliable`, `serialize`).
**License:** BSD-3-Clause. Public-release-safe.

**Maintenance (verified):** latest release **v1.7.0, 2024-07-18** (security fix:
AEAD nonce reuse across server restart); v1.6.0 did the CMake migration.
**No releases in 2025–2026** — effectively mature/dormant. Source:
<https://github.com/mas-bandwidth/yojimbo/releases>. Glenn Fiedler's active focus
is the commercial **Network Next**, not these libs.

**Security model:** dedicated-server-first. `netcode.io` uses **connect tokens** —
short-lived, encrypted (libsodium AEAD) tokens minted by a trusted **web backend**
and handed to clients out-of-band; the game server validates them. This is a
strong, well-designed model *for FPS-style matchmade dedicated servers*.

**Fit gaps for Assisi (why the rejection stands, but for better reasons):**
- **Connect-token model assumes a backend web service** to issue tokens. Assisi's
  v1 is direct-IP / LAN / listen-server with self-signed certs — the token flow is
  overhead you'd stub out. GNS self-signed certs fit v1 better.
- **No in-process loopback pair** analogous to `CreateSocketPair`. The listen
  server (embedded server + local client, zero socket) is awkward to express —
  you'd loop over UDP on localhost or special-case it. This is a concrete
  ergonomic loss vs GNS for Assisi's *stated* topology.
- **No NAT traversal, no lanes** (single reliable-ordered + unreliable channel
  model; you'd build snapshot/bulk separation yourself).
- libsodium dependency (fetchable, MIT — lighter than protobuf+abseil, actually a
  point in its favor).
- **Verdict:** correctly rejected — but the sharpest reason is *listen-server
  loopback + backend-assumption*, not "no NAT traversal."

`reliable.io` note: also forked/used by CitizenFX/FiveM
(<https://github.com/citizenfx/reliable.io>) — the reliability layer alone is
reusable if a custom stack were ever chosen (see §8).

---

## 3. ENet / enet6

**ENet** (<http://enet.bespin.org>, `lsalzman/enet`): **MIT**, ~C89, zero deps,
tiny. Connection-oriented, reliable+unreliable **channels** (closest thing to
lanes here), sequencing, fragmentation/reassembly, basic flow control. **No
encryption, no IPv6** in upstream. Trivial FetchContent/CMake. The classic
"just works, read it in an afternoon" library.

**enet6** (<https://github.com/SirLynix/enet6>): fork adding **IPv6** while
staying ENet-protocol-compatible. **v6.1.0 adds an encryption hook** (bring-your-
own cipher via a peer-aware callback, like the compressor callback) + an ack
callback. License MIT. Last release ~Oct 2024; maintained by a single active
author (SirLynix / Nazara Engine). Sources:
<https://github.com/SirLynix/enet6/releases>, <https://github.com/SirLynix/enet6>

**How games layer crypto on ENet:** the encryption is a *transform hook*, not a
key-exchange/authentication protocol — you supply the cipher and must build the
handshake/key management yourself. **Godot** ships its own ENet fork with **DTLS**
(mbedTLS) for exactly this reason:
<https://godotengine.org/article/enet-dtls-encryption/>.

**Fit for Assisi:** attractive on weight/license/build-simplicity (the anti-thesis
of the protobuf chain, very much in the "performance-first, cost opt-in" spirit).
But to match GNS you'd bolt on: authenticated encryption + key exchange (DTLS via
mbedTLS, or libsodium), a real loopback path, and stronger congestion control.
That's re-implementing the exact things GNS gives free. **Best fallback if Stage 0
protobuf integration proves intolerable** — but a downgrade on crypto/CC/lanes.

---

## 4. KCP, nbnet, RakNet/SLikeNet

- **KCP** (<https://github.com/skywind3000/kcp>): **MIT**, single-file ARQ
  *algorithm only* — no sockets, no crypto, no connection management (you feed it
  a UDP send callback). Trades ~10–20% bandwidth for 30–40% lower latency vs TCP.
  Great as a *reliability primitive inside a custom stack*, not a transport. Not a
  fit as the whole layer.
- **nbnet** (<https://github.com/nathhB/nbnet>): **MIT**, single-header C99
  client-server; pluggable transport drivers incl. **WebRTC (browser clients)**;
  optional encryption. Lightweight and pleasant, but small user base, single
  maintainer, no lanes/congestion-control sophistication. Interesting *only* for
  the future web-client angle — parked.
- **RakNet** (public, `facebookarchive/RakNet`): **abandoned** (Oculus/Facebook
  archived it years ago). **SLikeNet** (SLikeSoft fork): also effectively dormant,
  last meaningful activity years back. **Do not adopt** for a new engine — dead
  code eating a security-sensitive network parser. Both firmly rejected.

Landscape cross-check: `MultiplayerNetworkingResources`
(<https://github.com/0xFA11/MultiplayerNetworkingResources>) and
<https://multiplayernetworking.com/> — no new C++ contender in this class has
displaced GNS/ENet/yojimbo as of 2026.

---

## 5. QUIC-based (MsQuic, quiche, lsquic) + WebTransport

- **MsQuic** (<https://github.com/microsoft/msquic>): **MIT**, Microsoft, C,
  actively maintained, Windows+Linux, pluggable TLS (schannel / OpenSSL/quictls).
  Supports **RFC 9221 unreliable DATAGRAM** frames.
- **quiche** (Cloudflare, Rust + C API, BoringSSL) and **lsquic** (LiteSpeed, C,
  BoringSSL) — both solid, both drag a TLS stack (BoringSSL/quictls) that is
  heavier and *more* CMake-hostile than protobuf+abseil.
- **QUIC datagrams for games (RFC 9221):** technically apt — encrypted,
  congestion-controlled, unretransmitted unreliable frames over one connection,
  multiplexed with reliable streams (solves TCP HOL). Sources:
  <https://datatracker.ietf.org/doc/html/rfc9221>,
  <https://quic-go.net/docs/quic/datagrams/>
- **Is "games over QUIC" viable/practiced in 2026?** *Emerging, not standard.*
  QUIC gives you encryption + CC + streams, but **not** game-shaped niceties
  (message lanes with independent reliability, an in-process loopback pair, a
  server-browser/relay story) — you'd rebuild the replication-facing plumbing
  anyway. Handshake is heavier than GNS's, and the TLS dependency is a bigger
  FetchContent liability than the very chain Stage 0 already worries about. Net:
  **not competitive with GNS for Assisi's v1 today.**
- **WebTransport** (HTTP/3 over QUIC): the real reason to keep QUIC on the radar —
  it is the path to **browser clients** later (datagrams + streams in-browser). If
  a web client ever becomes a goal, a *second* WebTransport transport behind the
  `Assisi::Net` pimpl is the move — not a replacement for the native GNS path.
  This is another argument to keep the abstraction.

---

## 6. Steam Sockets & Epic Online Services (distribution tier)

- **Steam Sockets (Steamworks SDK):** *the same API as standalone GNS* (§1). This
  is the payoff of choosing GNS: shipping on Steam = relink against Steamworks +
  gain SDR (DDoS-protected relay, IP hiding, NAT traversal handled by Valve's
  backbone) with **no change to the `ISteamNetworkingSockets` call sites**. The
  swap is genuinely API-seamless; the only non-code cost is being a Steam title.
  Verified against Steamworks docs (§1). **This is the single strongest reason to
  stay on GNS over enet6/yojimbo.**
- **Epic Online Services (EOS):** free, cross-platform, includes P2P + relay +
  matchmaking, but is a **closed-source SDK** with its own auth/identity coupling
  and account-portal requirements — heavier integration and against the
  fully-open-source spirit. Reasonable as a *later* distribution option behind the
  pimpl, not a base transport.

---

## 7. NAT-traversal reality check (the doc's justification is weak here)

The rejection lines "enet6: no NAT traversal / yojimbo: no NAT traversal" imply
GNS *solves* NAT for free. **It does not, in the standalone build:**

- GNS's ICE gets you candidate gathering + STUN + (v1.6) TURN, **but ICE needs a
  rendezvous/signaling channel to exchange candidates** — i.e. **you still run
  your own signaling service** (a small web service; the doc itself admits this in
  Stage 7: *"GNS ICE needs a rendezvous/signaling service … + STUN, optional
  TURN"*). So GNS ≠ turnkey NAT traversal outside Steam.
- What small engines actually do for 2–32 players, in practice:
  1. **Direct IP + manual port forwarding** and **LAN discovery** — covers most
     friends/LAN play, zero infra.
  2. **A relay/TURN server** you host, or **libjuice** (standalone ICE in C,
     no WebRTC bulk — <https://github.com/paullouisageneau/libjuice>) if you want
     P2P without pulling WebRTC. STUN is cheap-but-unreliable; TURN always works
     but costs bandwidth. Sources:
     <https://deepwiki.com/paullouisageneau/libjuice/5-turn-protocol-and-relay>,
     <https://dev.to/sayem_omer/stun-turn-and-ice-servers-nat-traversal-for-webrtc-5e29>
  3. **Steam Datagram Relay** — the one genuinely turnkey option, and *only* via
     the Steamworks link.
- **Conclusion:** NAT traversal is **mostly orthogonal to the transport-library
  choice** — every option needs a signaling and/or relay service you stand up
  yourself, *except* SDR-via-Steam. So NAT should **not** be the axis on which
  enet6/yojimbo are rejected. Re-justify GNS on encryption + lanes + CC + loopback
  + the Steam/SDR path, and treat NAT as the Stage-7, infra-driven problem it is.

---

## 8. Custom minimal reliable-UDP layer (Gaffer On Games) — honest cost/benefit

Since the replication layer is already custom, "just write the transport too" is
tempting. Honest accounting:

- **Cheap part (what the Gaffer articles actually cover):** packet
  sequencing + ack bitfield + RTT/loss estimation. A few hundred lines,
  well-understood. Reusable pieces exist (`reliable.io`, KCP) if you don't want to
  hand-roll even this.
- **Expensive part (what production needs and the articles gloss):**
  - **Authenticated encryption + key exchange** — you must **not** roll your own;
    that means integrating libsodium/DTLS anyway (so you don't escape a crypto
    dep).
  - **Connection handshake, keepalive, timeout, graceful close.**
  - **Fragmentation/reassembly, MTU discovery.**
  - **Congestion control** good enough not to melt under loss (GNS's SNP is years
    of Valve tuning).
  - **DoS/amplification hardening** on a UDP parser that eats hostile bytes.
  - **IPv6 dual-stack, loopback pair.**
- **Verdict:** the ack loop is cheap; *hardening it to GNS parity is not*, and it's
  security-sensitive surface for a publicly-released engine. The replication layer
  is worth owning (it's your game's differentiator); the transport is worth
  *reusing hardened code*. **Do not build a custom transport.** If GNS's build ever
  becomes untenable, drop to **enet6 + DTLS** (reuse two mature libs) rather than
  writing raw reliable-UDP from scratch.

---

## 9. Comparison matrix

| Library | Maint. (2026) | License | Dep weight | FetchContent ease | Msg/Conn oriented | Lanes | Reliable+Unrel | Encryption | IPv6 | Loopback pair | NAT/ICE | CC quality | Fit |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **GNS** | Active master; last tag v1.6.0 Jun-2024 | BSD-3 | **Heavy** (protobuf+abseil, +OpenSSL/BCrypt) | **Hard** (find_package(Protobuf) edge) | ✔ / ✔ | **✔** | ✔ | **✔ built-in AEAD** | ✔ | **✔ CreateSocketPair** | ICE+STUN+TURN (needs your signaling) | **Best** | **Primary** |
| enet6 | Active (1 dev), ~Oct-2024 | MIT | **Tiny** | **Trivial** | ✔ / ✔ | channels | ✔ | hook only (BYO cipher) | ✔ | ✗ (fake it) | ✗ | basic | Fallback |
| ENet (upstream) | Stable/slow | MIT | Tiny | Trivial | ✔ / ✔ | channels | ✔ | ✗ | ✗ | ✗ | basic | — |
| yojimbo | Dormant, v1.7.0 Jul-2024 | BSD-3 | Light (libsodium) | Easy | ✔ / ✔ | ✗ | ✔ | ✔ (connect tokens) | ✔ | ✗ | good (reliable.io) | No (needs backend, no loopback) |
| KCP | Active | MIT | None | Trivial | algo only | ✗ | reliable ARQ | ✗ | n/a | ✗ | latency-opt | Primitive only |
| nbnet | Low activity | MIT | Tiny | Easy | ✔ / ✔ | ✗ | ✔ | optional | ✔ | ✗ | basic | Web-client curio |
| RakNet/SLikeNet | **Dead** | BSD/mixed | Medium | — | ✔ | ✔ | ✔ | ✔ | partial | ✗ | dated | **Reject** |
| MsQuic | Active (MS) | MIT | **Heavy** (TLS) | Hard | streams + RFC9221 dgram | streams | ✔ | ✔ (TLS1.3) | ✔ | ✗ | excellent | Future/web only |
| quiche/lsquic | Active | BSD/MIT | **Heavy** (BoringSSL) | Hard | streams + dgram | streams | ✔ | ✔ | ✔ | ✗ | excellent | Future/web only |
| Steam Sockets | Active (Valve) | proprietary SDK | (SDK) | n/a | ✔ / ✔ (**== GNS API**) | ✔ | ✔ | ✔ | ✔ | **SDR turnkey** | Best | **Distribution tier** |
| Custom (Gaffer) | you | yours | +libsodium anyway | n/a | you build | you build | you build | you build | you build | you build | you build | **Don't** |

---

## Implications for Assisi

**On the transport choice:** **keep GNS.** No other single library covers lanes +
authenticated encryption + strong congestion control + `CreateSocketPair` loopback
+ the Steam/SDR upgrade path, and those — *not* NAT traversal — are the real
requirements. Fix the doc's rationale: the enet6/yojimbo "no NAT traversal"
one-liners are the weakest possible justification (standalone GNS doesn't give
turnkey NAT either; you run signaling regardless, and NAT is deferred to Stage 7).
Re-anchor the decision on encryption/lanes/CC/loopback/Steam.

**On Stage 0's dependency plan:**
- The **protobuf-current + abseil** decision is **sound** (security backports for a
  network-facing parser; newer protobuf carries the CMake hygiene GNS ≥1.5 relies
  on). The abseil cost (~130 targets, lockstep version pairing) is real but
  accepted correctly. Contingency (b) — fall back to protobuf 21.12 — remains a
  valid *temporary* escape, tracked as debt. Keep the escalation ladder as written.
- The **crypto ladder** (BCrypt on Windows, system OpenSSL on Linux, libsodium as
  the fully-self-contained escape hatch) is fine and matches GNS's supported
  backends. Verify exact `USE_CRYPTO`/`USE_CRYPTO25519` names at the pinned
  ref (the doc already says to) — don't trust notes over `BUILDING.md`.
- **Correct the pin:** "v1.6.0 (June 2024)" is a ~2-year-old tag. Recommend
  **pinning a recent `master` commit** instead — it carries native-ICE/TURN
  maturity and the protobuf/abseil compat fixes — and record the exact SHA. The
  old "GNS pulls in WebRTC" fear is obsolete: native ICE means **no WebRTC dep**.

**On the `Assisi::Net` pimpl:** **keep it, unchanged.** It's the cheap hedge
against the single real risk (Stage 0 protobuf integration): if that build fight is
lost, the fallback is **enet6 + DTLS/libsodium** behind the same interface, not a
rewrite. The pimpl is *also* the seam for a future **WebTransport/QUIC** transport
if browser clients ever matter. The `Lane`/`SendMode`/`ConnectionId`/loopback-pair
shape in the sketch maps cleanly onto GNS lanes and onto enet6 channels alike, so
the abstraction is already at the right altitude. No interface change needed —
only a one-line note that `CreateLoopbackPair` is the one primitive a fallback
transport would have to emulate rather than get for free.

*Bottom line: the destination (GNS) is right; tighten the map — correct the release
pin, drop the NAT-traversal rationale in favor of the real one, and keep the pimpl
as insurance.*
