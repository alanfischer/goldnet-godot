# goldnet-godot

A Godot 4 GDExtension that provides **`GoldNetMultiplayer`**, a drop-in
`MultiplayerAPI` implementing **GoldSrc/Quake-style netcode** — an unreliable,
delta-against-acked-baseline snapshot stream — served **behind the stock
`MultiplayerSynchronizer` / `MultiplayerSpawner` API**. You keep your ordinary
synchronizers, spawners, and `@rpc` methods; installing goldnet swaps only the
*state-replication* layer underneath them.

It is the netcode sibling to [`goldsrc-godot`](../goldsrc-godot) (the asset
loader) and mirrors the [`hop`](../hop-godot) split: a single GDExtension today,
with an engine-agnostic **goldnet** core and this **goldnet-godot** binding as the
eventual factoring. See `../../docs/goldsrc_netcode_plan.md` for the full plan.

## Why

Godot's built-in replication leans on reliable/ordered channels for spawns and
much of its property sync. That is simple and correct, but on a lossy network the
retransmit pressure builds until ENet times peers out. GoldSrc-style netcode takes
the opposite stance: send an **unreliable** snapshot every tick, delta-compressed
against the last frame the client *acked*. A lost packet is never retransmitted —
the next snapshot simply re-carries whatever changed since the acked baseline, so
the stream self-heals. The result is steady bandwidth and connections that survive
loss levels where reliable replication drops clients (see **Performance** below).

## How it works

**Composition, not replacement.** `GoldNetMultiplayer extends
MultiplayerAPIExtension` and holds an inner stock `SceneMultiplayer`
(`MultiplayerAPI::create_default_interface()`). Handshake, auth, peer lifecycle,
and **all `@rpc` traffic** are delegated to the inner; goldnet overrides only the
object-configuration + poll path that drives state replication. It relays the
inner's lifecycle signals up to itself so the consuming game listens on one object.

**Path-hash identity, no handshake.** Every owned node is keyed by
`net_id = hash(scene_path)`. The same node lives at the same path on server and
client, so ids match with no negotiation and no path-cache to poison — a client
simply drops a `net_id` it doesn't yet hold and self-heals.

**Two registries, by lifecycle — not by entity kind.** goldnet is fully
entity-agnostic; players, bots, projectiles, and map movers all flow through one
pipeline. A node lands in:

- **`owned_syncs`** when its `MultiplayerSynchronizer` enters the tree with at
  least one sync-marked property (`_object_configuration_add`). This is the
  per-tick delta stream, and it covers *both* map-static movers and spawned
  entities.
- **`spawn_records`** when it is born from a wrapped `MultiplayerSpawner.spawn()`.
  Map-static entities load with the map (both peers already have them) so they need
  no spawn record; runtime entities do, because the client has no other way to know
  they should exist.

**The snapshot protocol (delta against acked baseline).** Each tick, per peer, the
server emits one unreliable snapshot:

```
[u16 seq][u16 base_seq][u32 server_time]
[u16 spawn_ct]   { [u32 net_id][u32 spawner_net_id][var data] } *   (reliable-until-acked)
[u16 despawn_ct] { [u32 net_id] } *                                 (reliable-until-acked)
[u16 changed]    { [u32 net_id][u32 changed_mask]{ value } per set bit } *
```

- `base_seq` is the frame this peer last acked (0 = full state). The server keeps a
  per-peer ring of recent frames; the client keeps a matching ring and acks the
  newest frame it applied. On loss the ack stalls, the server keeps diffing against
  the same acked baseline, and the next snapshot re-carries the accumulated delta —
  no retransmit, no desync. If the baseline ages out of the ring (~1 s of loss), the
  server falls back to a full frame.
- `changed_mask` indexes the config's sync-property "slots"; an entity whose every
  slot equals the baseline is **skipped entirely** (idle movers/entities cost zero
  bytes — provided the game doesn't rewrite a field every tick).
- Spawns/despawns are **reliable-until-acked**: re-sent each frame until the peer
  acks a frame carrying them, then retired. A spawn's source record outlives
  delivery, so each peer keeps a durable `spawn_acked` set — a spawn is delivered
  exactly once, never re-armed frame after frame.

**Compact value encoding.** Each changed slot is written with a 1-byte type tag
plus a tight payload — `f32` for floats, zig-zag varint for ints, 3×`f32` for
`Vector3`, a byte for bool, and a `put_var` fallback for anything else. This keeps
the stream self-delimiting (any `Variant` still round-trips) while roughly halving
per-entity bytes versus stock `put_var` (which tags every value with a 4-byte type
header and stores floats/ints at 64-bit width): a moving player's ~8 props drop
from ~112 B to ~48 B.

**Per-peer visibility (PVS).** goldnet honors the synchronizer's *native*
visibility — an entity is sent to a peer when `is_visibility_public()` or
`get_visibility_for(peer)`. Because Godot does **not** bind
`MultiplayerSynchronizer::is_visible_to`, a GDExtension `MultiplayerAPI` cannot
evaluate `add_visibility_filter()` callbacks. The consuming game therefore drives
per-peer visibility through the *push* API instead: compute the PVS predicate and
call `set_visibility_for(peer, visible)` each net tick, with
`public_visibility = false`. One `peer_visibility` map then serves both backends —
the stock replicator reads it via `is_visible_to`, goldnet via
`get_visibility_for`. (WizardWars does this with a small `NetworkManager` registry;
see its `register_pvs_sync` / `push_pvs_visibility`.)

## Performance

Measured in WizardWars (ww_2fort + 4 bots, stationary headless clients, server→client
egress at the ENet socket), goldnet vs the **stock `MultiplayerSynchronizer`** path
on the same build with the same PVS:

| Condition | goldnet | stock |
| --- | --- | --- |
| 1 / 4 / 8 clients, no loss (per-client) | ~26–29 KB/s | ~27–29 KB/s |
| 4 clients, 10% loss (per-client) | ~30 KB/s | ~31 KB/s |
| 4 clients, **25% loss** | **4/4 held, ~30 KB/s** | **2/4 dropped**, survivors ~49 KB/s |

**On raw bandwidth goldnet is at parity with Godot's built-in synchronizers**
(a consistent few-percent edge, within noise), and both scale linearly with client
count. The divergence is **robustness under loss**: at 25% loss stock's reliable
channels thrash until ENet drops half the clients, while goldnet's unreliable
ack-delta stream holds every connection at flat bandwidth. That is the GoldSrc
tradeoff working as intended — eventually-consistent snapshots beat
reliable-ordered replication for real-time state on adverse networks.

> Bandwidth wins over a *hand-rolled* RPC baseline are a separate story: goldnet
> replaces per-entity full-state sends, PVS-culls, and compact-encodes, so against
> naive full-state replication the reduction is large.

## Status

Built as a single GDExtension; the engine-agnostic core/binding split is the final
step.

- **Phase 0 — pass-through composition** ✅ every virtual forwarded to the inner;
  installing it changes nothing observable.
- **Phase 1 — registry + full-state snapshot** ✅ own the map-static synchronizers,
  stream their state ourselves.
- **Phase 2 — delta against acked baseline** ✅ the GoldSrc core; per-slot skipping,
  per-peer rings, self-heal under loss.
- **Phase 3 — spawn / despawn in the stream** ✅ own the spawners too; players and
  projectiles are agnostic with map movers.
- **Phase 4 — client helpers + idle-free replication** ✅ reusable
  `InterpolationBuffer` (server-clock-synced jitter buffer) and `PredictedBody`
  (simulation-agnostic input prediction + reconciliation harness — owns the seq
  counter, unacked-input history, out-of-order rejection, chain-correction
  suppression, and replay ordering; the domain supplies snapshot/divergence/replay).
  Idle entities freeze their shadow state so unchanged frames are skipped whole.
  WizardWars' netmove movement dogfoods `PredictedBody`.
- **Per-peer PVS cull** ✅ via the `set_visibility_for` push bridge above — the
  former open limitation is resolved.
- **Compact wire encoding** ✅ tagged f32 / varint value codec.
- **Phase 5 — generalize / configure** (in progress): a config surface on the
  installed API (`snapshot_interval_ms`, `debug_enabled`, `loss_percent`) and
  opt-in per-property **quantization hints** — angles to a u16, floats/Vector3 to
  IEEE half — via a synchronizer's `gn_quant` meta (self-describing tags, so only
  the sender needs the hint). WizardWars quantizes player yaw/pitch.
- Remaining: full build-matrix packaging + a minimal example project, and the
  engine-agnostic `goldnet` core / `goldnet-godot` binding split (Phase 6).

## Building

```bash
./build.sh            # all platforms (macos linux windows android)
./build.sh macos      # just one
```

Outputs land in `addons/goldnet/bin/`. The build **force-loads the whole godot-cpp
archive** (see `src/CMakeLists.txt`): a `MultiplayerAPI` is handed arbitrary engine
objects through its virtuals, so every engine class's instance-binding callbacks
must be linked in, not just the ones this extension names directly. Rebuild after
any source change and relaunch the game — a running session won't pick up new native
code.

In this monorepo, `extern/godot-cpp` is a symlink to the shared checkout under
`extern/hop-godot` so we don't vendor a second copy.

## Usage

1. Symlink or copy `addons/goldnet/` into your project's `addons/`.
2. Install it **before** any `MultiplayerSynchronizer` / `MultiplayerSpawner` enters
   the tree:
   ```gdscript
   get_tree().set_multiplayer(ClassDB.instantiate(&"GoldNetMultiplayer"))
   ```
3. Keep your stock synchronizers, spawners, and `@rpc` methods as-is.
4. For PVS bandwidth culling, drive per-peer visibility through the push API rather
   than `add_visibility_filter` (goldnet can't read filters — see **How it works**):
   ```gdscript
   sync.set_visibility_public(false)
   # each net tick, per peer:
   sync.set_visibility_for(peer_id, my_pvs_predicate(peer_id))
   ```
5. (Optional) Tune the installed API and opt into lossy per-property quantization:
   ```gdscript
   var gn := ClassDB.instantiate(&"GoldNetMultiplayer")
   gn.snapshot_interval_ms = 33   # 0 = derive from synchronizers' replication_interval
   get_tree().set_multiplayer(gn)

   # Per property, keyed by the sync property's leaf name. Set before the sync enters
   # the tree. Recognized hints: "angle16" (radians→u16), "half", "vec3_half".
   sync.set_meta("gn_quant", { "yaw": "angle16", "pitch": "angle16" })
   ```
   `debug_enabled` / `loss_percent` are also settable (mirror `GOLDNET_DEBUG` /
   `GOLDNET_LOSS`).

> **Headless note:** Godot registers `.gdextension` files via
> `.godot/extension_list.cfg`, refreshed by an editor project scan (or export).
> To add the extension without opening the editor, append
> `res://addons/goldnet/goldnet.gdextension` to that file manually. WizardWars gates
> the install behind the `--goldnet` command-line flag.

## Debugging

- `GOLDNET_DEBUG=1` — periodic per-peer snapshot stats (send KB/s; per-snapshot
  `spawn` / `despawn` / `changed` counts and byte size on the client).
- `GOLDNET_LOSS=<pct>` — drop that percentage of outbound snapshots server-side, to
  exercise the ack-stall self-heal without a real lossy network.
