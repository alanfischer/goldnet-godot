# goldnet-godot

A Godot 4 GDExtension that provides **`GoldNetMultiplayer`**, a drop-in
`MultiplayerAPI` implementing **GoldSrc/Quake-style netcode** — an unreliable,
delta-against-acked-baseline snapshot stream — served **behind the stock
`MultiplayerSynchronizer` / `MultiplayerSpawner` API**. You keep your ordinary
synchronizers, spawners, and `@rpc` methods; installing goldnet swaps only the
*state-replication* layer underneath them.

It is the netcode sibling to [`goldsrc-godot`](../goldsrc-godot) (the asset
loader), built as a single Godot GDExtension.

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
[u32 magic]      "GNS" + wire version — a mismatched build is dropped, not misparsed
[u16 seq][u16 base_seq][u32 server_time]
[u16 spawn_ct]   { [u32 net_id][u32 spawner_net_id][var data] } *   (reliable-until-acked)
[u16 despawn_ct] { [u32 net_id] } *                                 (reliable-until-acked)
[u16 leave_ct]   { [u32 net_id] } *                                 (reliable-until-acked, opt-in — see PVS below)
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
- The **leave** section is the opt-in PVS-relevance channel (off by default;
  `leave_ct` is 0 and costs 2 bytes when disabled): net_ids that dropped out of the
  peer's PVS since last tick, delivered reliable-until-acked so the client can hide
  them. Re-entry needs no event — the entity simply reappears in `changed` with a
  full baseline. See **Per-peer visibility** below.
- `server_time` is the server's send-time (ms). Since a snapshot goes to every peer
  every tick regardless of entity traffic, it's an **always-on server-clock feed** —
  the client re-emits it as the `server_time_received(server_time_ms)` signal, which
  you feed to the `ServerClock` helper for interpolation timing. No separate
  server-time beacon RPC is needed on the goldnet path.

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

> **Engine limitation — want to fix upstream.** The push detour exists only because
> `MultiplayerSynchronizer::is_visible_to(peer)` is not exposed to GDExtension (it isn't
> `ClassDB`-bound), so a GDExtension `MultiplayerAPI` has no way to *evaluate* the
> `add_visibility_filter()` callbacks the engine stores — it can read the resolved
> `get_visibility_for()` state but not run the filters that would populate it. A stock
> `SceneMultiplayer` (engine-internal C++) calls `is_visible_to` directly and thus
> honors `add_visibility_filter` transparently; goldnet cannot, which is the one place
> the stock replication API does **not** map through cleanly. The proper fix is upstream
> in Godot: bind `MultiplayerSynchronizer::is_visible_to` (and/or `get_visibility_filters`)
> so a custom `MultiplayerAPIExtension` can evaluate filters itself. With that in place
> goldnet could read `add_visibility_filter` natively and the `set_visibility_for` push
> loop below would become optional. Until then, use the push API.

Culling alone stops sending an out-of-PVS entity, but the client still holds its last
state and would render it frozen through a wall. So goldnet also exposes an **opt-in
relevance channel**: enable it with `set_relevance_events(true)`, and the server emits
reliable-until-acked *leave* markers (the section above) for owned syncs that drop out
of a peer's PVS, surfaced to the game as the `entity_relevance_lost(sync)` signal so it
can hide/despawn its view of them. It is off by default — with it off goldnet stays a
pure state-replication transport and games drive relevance however they like.
(WizardWars turns it on and connects `NetworkManager._on_entity_relevance_lost`.)

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

## Testing

```bash
cmake -S tests -B tests/build
cmake --build tests/build
ctest --test-dir tests/build --output-on-failure
```

Standalone — no godot-cpp, no engine, runs in under a second. The suite covers
`src/goldnet_codec.h`: the zig-zag varint and angle16 codecs (round-trip, encoded
widths, wraparound), uint16 sequence comparison across rollover, the
reliable-until-acked bookkeeping, and the seeded loss PRNG.

Scope is deliberate. godot-cpp's engine classes call through GDExtension function
pointers that only exist inside a running Godot process, so anything touching
`Variant` or `StreamPeerBuffer` can't be tested here — that's why the helpers in
`goldnet_codec.h` are templated over their buffer and map types. The protocol itself
is covered by the integration suite below.

### Integration tests

```bash
./build.sh macos                 # the suite needs a built extension
./tests/integration/run.sh       # all cases
./tests/integration/run.sh ring_expiry
```

Each case runs a **server and client process pair** over a real `ENetMultiplayerPeer`
with a real `GoldNetMultiplayer` installed, so it exercises the actual snapshot, ack,
and spawn paths. The client holds the assertions and its exit code is the verdict; the
server is a cooperative peer. Logs land in `tests/integration/.logs/`.

The cases live in `tests/integration/cases/`, one file each, and every one opens with a
docstring stating its timeline and — where it matters — the precise limits of what it
covers, verified by mutation. Read those rather than a list here; a second copy of the
scope caveats in this file would drift from the code, and a stale caveat is worse than
none. `ls tests/integration/cases/` for the current set.

Adding a case: drop `cases/case_<name>.gd` extending `test_case.gd`, override
`server_step`/`client_step`, call `finish()` when the assertions are done. `run.sh`
picks it up automatically. `test_case.gd` provides `spawn_once(t, n)` and
`at(t, when, key)` so a case reads as its documented timeline rather than a pile of
boolean latches.

Cases needing more than one client declare it **twice**: `## @clients N` for run.sh to
grep, and `required_clients = N` in `setup()`. The harness cross-checks them and fails if
they disagree — without that, a missed grep silently runs a multi-client case with one
client, where it passes while testing nothing.

### GDScript helper tests

```bash
./tests/gdscript/run.sh                      # all suites
./tests/gdscript/run.sh interpolation_buffer # just one
```

Covers the three client-side helpers the addon ships alongside the extension —
`InterpolationBuffer`, `PredictedBody` and `ServerClock`. They're pure GDScript with no
dependency on goldnet's wire protocol, so this needs no built extension, no peer and no
second process; the whole thing runs in one headless process in well under a second.

Suites live in `tests/gdscript/suites/suite_<name>.gd` extending `suite.gd`; override
`run()` and call the assertions. The runner discovers them automatically and **fails any
suite that reports zero checks**, so a file that's added but never wired up (a missing
`run()` override, an early return) shows up as a failure rather than as green.

`ServerClock` reads the wall clock directly, so its suite works around that rather than
against it: assertions that can be made independent of it are (the correction tests assert
on the *ratio* between successive steps), and the rest carry an explicit slop tolerance.

### Mutation

All three suites are worth running under mutation — break the thing a test claims to cover
and confirm it goes red. A test that can't fail is worse than no test, because it reads
like coverage.

Where a mutation reveals that a test *doesn't* cover what it looks like it covers, say so
in the test rather than deleting it or contorting it into passing. Several tests here carry
a `SCOPE` note recording exactly that, usually because the code in question is defensive
and unreachable through the public API. Those notes are load-bearing documentation: the
next person to look will otherwise redo the analysis, or "fix" the test by asserting
something false.

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
   `GOLDNET_LOSS`). To receive PVS leave events, `set_relevance_events(true)` and
   connect the `entity_relevance_lost(sync)` signal (see **How it works → Per-peer
   visibility**).

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
- `GOLDNET_SIM_SEED=<n>` — make the whole sim reproducible: both the loss rolls and the
  latency draws come from one seeded generator. Unset (or `0`) uses the engine RNG, so
  every run drops different packets and draws different delays — fine for poking at
  self-heal, useless for a regression test you need to re-run or bisect. Also exposed as
  the `sim_seed` property.
