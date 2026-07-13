# GoldSrc-style netcode as a Godot MultiplayerAPI — implementation plan

> Name: **goldnet** (verified clear in the Godot/game-netcode namespace). Sibling to
> `goldsrc-godot` (the asset loader); this is the *netcode* counterpart. Phases 0–5 build
> it as a single Godot GDExtension; Phase 6 splits it into the engine-agnostic **goldnet**
> library + the **goldnet-godot** binding, mirroring how `hop-godot` wraps `hop`.
> (Earlier drafts used the placeholder "SnapNet" — treat any remaining "SnapNet"/"SnapNetMultiplayer"
> below as "goldnet"/"GoldNetMultiplayer".)

## 1. Goal

Build a **reusable, standalone Godot GDExtension** that provides a drop-in
`MultiplayerAPI` implementing **GoldSrc/Quake-style netcode**: an *unreliable,
delta-against-acked-baseline snapshot* stream served **behind the stock
`MultiplayerSynchronizer` / `MultiplayerSpawner` API**, plus **client-side helper
nodes** (interpolation buffer + prediction/reconciliation) so a consuming project
gets the GoldSrc *feel* turnkey — not just the wire format.

Decisions (fixed):
- **Scope:** reusable standalone module; WizardWars is its first consumer.
- **Language:** C++ GDExtension from the start (mirror `goldsrc-godot`'s toolchain).
- **Depth:** transport **+** client helper nodes.

### Why this is possible at all
`MultiplayerAPIExtension` is the real "swap the network engine" seam. When a
`MultiplayerSynchronizer` or `MultiplayerSpawner` enters the tree it calls
`get_multiplayer().object_configuration_add(node, config)`. Whoever *is* the
MultiplayerAPI fulfills that however it likes. The default (`SceneMultiplayer`)
fulfills it with two blunt modes:

| | skips unchanged | unreliable / no HoL stall | self-heals on loss |
|---|---|---|---|
| `ALWAYS`    | ✗ | ✓ | ✓ (resend) |
| `ON_CHANGE` | ✓ | ✗ (reliable) | ✓ (retransmit) |
| **GoldSrc ack-delta (this module)** | ✓ | ✓ | ✓ (ack baseline) |

The GoldSrc snapshot is the *top row of both columns at once*, which is why it
needs no static-vs-moving differentiation. That single primitive is the whole
point of this module.

Verified extension surface (godot-cpp headers):
- `MultiplayerAPIExtension`: `_poll`, `_rpc`, `_object_configuration_add/remove`,
  `_set_multiplayer_peer`, `_get_peer_ids`, `_get_unique_id`, `_get_remote_sender_id`.
- `MultiplayerSynchronizer` exposes enough to serve it: `get_replication_config()`,
  `get_root_path()`, `get_replication_interval()`, `is_visibility_public()`,
  `get_visibility_for(peer)`, `update_visibility()`, visibility filters.

## 2. Architecture

```
                 consuming game (WizardWars, others)
   stock nodes:  MultiplayerSynchronizer   MultiplayerSpawner   @rpc calls
        │ object_configuration_add / _rpc          │
        ▼                                          ▼
  ┌───────────────────────────────────────────────────────────┐
  │  SnapNetMultiplayer  (extends MultiplayerAPIExtension, C++) │
  │                                                             │
  │  ├─ inner SceneMultiplayer  ← delegate: handshake, auth,    │
  │  │     peer connect/disconnect, ALL @rpc transport          │
  │  │                                                          │
  │  ├─ SyncRegistry   ← synchronizer + spawner configs we keep │
  │  ├─ SnapshotWriter (server): per-client frame ring, acks,   │
  │  │     visibility, delta-vs-acked, bit-pack, unreliable send │
  │  └─ SnapshotReader (client): apply, ack, feed interp buffer  │
  └───────────────────────────────────────────────────────────┘
                 client helper nodes (this module too):
                   InterpolationBuffer   PredictedBody (reconcile)
```

**Composition, not full reimplementation.** We hold an inner `SceneMultiplayer`
(`MultiplayerAPI.create_default_interface()`) and forward connection lifecycle,
peer management, and every `@rpc` to it. We intercept only the *replication*
configs (synchronizers/spawners) and drive those ourselves in `_poll`. This keeps
the ENet handshake, authentication, relay, and reliable event RPCs (deaths, hits,
chat, …) working unchanged — we only replace the state-replication layer, which is
exactly the layer GoldSrc does differently.

### Wire format (server → client snapshot, unreliable)
```
header:   server_frame_seq (u16, wrapping)
          baseline_frame_seq (u16)   # what this delta is encoded against; == acked frame
          server_time_ms (u32)
spawns:   count, [ spawner_id, entity_net_id, spawn_data(var_to_bytes) ]   # reliable-until-acked
per-ent:  entity_net_id (delta-varint)
          changed_field_bitmask (varbits over the config's property list)
          [ changed field values, type-compact ]
despawns: count, [ entity_net_id ]                                          # reliable-until-acked
```
- **Baseline = client's last-acked frame.** Server keeps a per-client ring of the
  last N sent frames; encodes each new frame as the diff from the client's acked
  one. A dropped packet self-heals: the next frame still diffs against the same
  acked baseline and re-carries whatever changed since.
- **Field encoding v1 (generic):** type-aware compact per Variant — Vector3=12B,
  float=4B, int=varint, bool=1 bit. Already beats `ALWAYS` (skips unchanged) and
  beats `ON_CHANGE` (no reliability tax).
- **Field encoding v2 (optional, GoldSrc SendProp equivalent):** per-property
  quantization hints (angle→1B, world-pos→fixed-point over map bounds) supplied via
  a companion resource / node metadata. Extension point; not v1.

### Client → server ack (unreliable_ordered, newest wins)
`last_received_frame_seq` (u16). Small and frequent. May piggyback the input
stream if the game has one, but the module ships a standalone ack packet so it
works with no game input channel.

### Visibility
Honor the synchronizer's existing visibility API verbatim (`is_visibility_public`,
per-peer `get_visibility_for`, filters). WizardWars' BSP-PVS filters keep working
with zero change — the module reads the same veto the stock system reads.

## 3. Phasing

Each phase ends at a **playable, verifiable** state; risk is front-loaded.

### Phase 0 — Repo + build skeleton + **pass-through spike** (de-risk)
- New repo `snapnet`, `godot-cpp` submodule, build scripts cloned from
  `goldsrc-godot` (incl. the android-arm64 matrix — WizardWars ships to Quest).
- `SnapNetMultiplayer` that delegates **100%** to an inner `SceneMultiplayer`
  (pure pass-through — no custom protocol yet).
- Wire into WizardWars: `get_tree().set_multiplayer(SnapNetMultiplayer.new(), root)`.
- **Exit criterion:** WizardWars plays *identically* with our custom API installed —
  existing synchronizers, spawners, RPCs, connect/disconnect all unchanged. This
  proves the composition boundary and surfaces any spot where a synchronizer
  secretly assumes the concrete `SceneMultiplayer` **before** any protocol work.
- **Key risk answered here:** is the config `Variant` from `_object_configuration_add`
  introspectable enough to tell synchronizer / spawner / RPC apart and read what we need?

### Phase 1 — Registry + full-state snapshot (parity, our path)
- Intercept synchronizer/spawner configs into `SyncRegistry`; forward RPC config to inner.
- `_poll`: naive **full-state** snapshot each tick (no delta yet), unreliable, honoring
  visibility. Client applies to nodes.
- **Exit criterion:** replication parity with stock, but flowing through *our*
  writer/reader. This is "old netcode on the new API," functionally.

### Phase 2 — Delta + ack (**the GoldSrc core**)
- Server per-client frame ring + last-acked tracking; client acks last-received frame.
- Delta-encode vs. acked baseline; changed-field bitmask; skip unchanged entities.
- **Exit criterion:** static entities cost ~0 with **no reliability tax and no
  differentiation**. Bandwidth measured against (a) our committed `ON_CHANGE` build
  and (b) stock `ALWAYS`; target beats both. Correctness verified under simulated loss.

### Phase 3 — Spawn / despawn
- `MultiplayerSpawner` create/destroy folded into the stream; spawn carries custom
  spawn data and is **reliable-until-acked** (resend the spawn/despawn record until
  the client acks a frame that contains it — GoldSrc baseline/create semantics).
- **Exit criterion:** projectiles/corpses spawn and free correctly through our path
  under packet loss (no ghosts, no missed spawns).

### Phase 4 — Client helper nodes (the "+ helpers" deliverable)
- `InterpolationBuffer` — generalize WizardWars' clock-synced interp buffer into a
  reusable node/component (render at `now − interp_delay`, teleport detection, gap fill).
- `PredictedBody` — generalize local prediction + server reconciliation (apply input
  now, keep input history, replay unacked on correction).
- WizardWars migrates its bespoke interp/prediction onto these (dogfood).
- **Exit criterion:** WizardWars uses only module-provided helpers; feel unchanged or better.

### Phase 5 — Generalize, document, package
- Config surface (tick rate, interp delay, transfer channels, redundancy).
- v2 per-property quantization hints extension point.
- Docs + a **minimal example project** (not WizardWars) proving drop-in reuse.
- Full cross-platform build matrix (macOS/Linux/Windows/**Android-arm64 for Quest**).
- **Exit criterion:** a fresh project adds the `.gdextension`, sets the multiplayer,
  drops in helper nodes, keeps its stock synchronizers, and gets GoldSrc netcode.

### Phase 6 — Extract the engine-agnostic core (**goldnet / goldnet-godot split**)
Done **last, after the coupled version is proven** — you learn the real seams by building
against the game first, so the abstractions are earned, not guessed. Mirrors `hop` / `hop-godot`.

- **`goldnet`** (pure C++ library, no Godot): the protocol and its math — snapshot ring,
  delta-against-acked encoding, ack tracking, wire format, the interpolation-buffer math, and
  the prediction/reconciliation replay framework. Operates on an *abstract* entity-state model.
  Unit-testable with zero engine: deterministic protocol tests, packet-loss/jitter sims,
  delta-correctness checks, all headless. Can also back a **non-Godot dedicated server** (ENet is
  a C library, engine-agnostic) via the same interfaces.
- **`goldnet-godot`** (godot-cpp GDExtension): the glue. The `MultiplayerAPIExtension` from
  Phases 0–5 becomes this — it delegates all protocol work to `goldnet` and supplies the
  engine-specific bindings.

The seams `goldnet` must define (this is the honest asymmetry vs. `hop`: physics has ~1 seam,
bodies in/out; netcode has ~4–5):
- **Transport** — `send(peer, channel, reliable, bytes)` + `poll()`. `goldnet-godot` backs it
  with Godot's `ENetMultiplayerPeer`; a headless server backs it with raw ENet.
- **Entity-state model** — field schema (types + quantization hints) + get/set over opaque
  handles. `goldnet-godot` marshals Godot node properties ↔ `goldnet` state.
- **Relevancy** — `is_relevant(entity, peer) -> bool`. `goldnet-godot` answers with BSP PVS.
- **Clock/tick source** — driven by the Godot frame loop in the binding.

- **Exit criterion:** `goldnet` builds and its protocol test-suite passes with no Godot present;
  WizardWars runs on `goldnet-godot` (which is only bindings) with no behavior change; the split
  is a refactor, not a rewrite.

## 4. Cross-cutting concerns
- **Server-authoritative** model assumed and documented; the module is transport +
  client helpers, not a game framework.
- **Reliable event RPCs** (deaths, hits, chat, spawns-with-side-effects) keep flowing
  through the inner `SceneMultiplayer` — unchanged, not our concern.
- **Lag compensation / rewind** stays game-side, but the module must expose per-frame
  server timestamps so games (like WizardWars' rewind ghosts) can do it. Interface requirement.
- **Testing:** headless loopback harness + packet-loss/jitter simulation (reuse the
  concept from WizardWars' `sim`); assert bandwidth *and* correctness under loss.
- **Quest/Android** is a first-class target from Phase 0, not an afterthought.

## 5. Open risks (ranked)
1. **Synchronizer introspection completeness** — does the public node API expose
   everything the writer needs, or does some behavior assume the concrete
   `SceneMultiplayer`? *Front-loaded to Phase 0.*
2. **`_object_configuration_add` config typing** — reliably distinguishing sync /
   spawner / RPC configs and reading their contents. *Phase 0–1.*
3. **Spawn-data reliability** under loss without a full reliable channel. *Phase 3.*
4. **C++ per-tick budget** on a 380-entity server (should be fine in C++; the reason
   we chose C++ over GDScript, but still profile). *Phase 2.*
5. **Path-cache / spawn-ordering** parity with the issues already hit in the stock
   migration (map-ready gating, deterministic ids). *Phase 1–3.*

## 6. Relationship to current work
The `multiplayer-synchronizer-migration` branch (stock synchronizers) is the safe
fallback and reaches **parity** with the old hand-rolled netcode on **`ALWAYS`**.

### Why the stock synchronizer forces the module (finding, 2026-07-11)
A playtest surfaced the core limitation directly. We tried tuning movers to
`ON_CHANGE` for a 2.8× bandwidth win, but it **breaks doors**: `ON_CHANGE` never
sends a *baseline* when a peer enters a mover's PVS after map load, and a delta
rejected during the `recv_nodes` path-cache warmup is **never resent** — so a door
first seen mid-match desyncs permanently (verified: late-visibility movers received
zero client syncs). `ALWAYS` is robust (re-sends full state every tick, so late
visibility self-heals) but re-sends hundreds of *static* map entities every tick —
the 2.8× waste.

The netcode must stay **entity-agnostic** — no per-entity mode choice, no
static-vs-mover gating. Under the stock synchronizer that leaves exactly one
agnostic + correct option, **`ALWAYS`**, and it is the wasteful one. There is **no
stock setting that is agnostic AND cheap AND correct.** That combination is exactly
what GoldSrc ack-delta provides (unreliable, skips-unchanged, self-heals via acked
baseline), and it is unreachable without this module. So the current branch ships on
`ALWAYS` (correct, wasteful); SnapNet is what makes it cheap without giving up
agnosticism or correctness. This finding is the concrete justification for the build.
