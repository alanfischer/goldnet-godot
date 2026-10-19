# goldnet-godot

A Godot 4 GDExtension that provides **`GoldNetMultiplayer`**, a drop-in
`MultiplayerAPI` implementing **GoldSrc/Quake-style netcode** — an unreliable,
delta-against-acked-baseline snapshot stream — served **behind the stock
`MultiplayerSynchronizer` / `MultiplayerSpawner` API**. You keep your ordinary
synchronizers, spawners, and `@rpc` methods; installing goldnet swaps only the
*state-replication* layer underneath them.

It is the netcode sibling to [`goldsrc-godot`](https://github.com/alanfischer/goldsrc-godot) (the asset
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

**An engine-agnostic core, adapted to Godot.** The protocol itself lives in
`core/include/goldnet/` as header-only C++ with no Godot dependency: `goldnet::Server`
(change tracking, per-peer deltas against the acked frame, acks, spawns, leaves, the
packet budget) and `goldnet::Client` (baseline reconstruction and apply), over the
byte-level wire in `buffer.h` / `codec.h` / `protocol.h`, plus `goldnet::NetSim`, the
send-side loss / latency / spike simulator. Both are templated on the
host's value type and reach the engine only through a small host interface — read a
slot, test visibility, encode a value, send bytes — so Godot's `Variant`s pass through
unconverted. Everything Godot-specific (synchronizers, spawners, `Variant` encoding, the
inner `SceneMultiplayer`) is the adapter in `src/`.

**Composition, not replacement.** `GoldNetMultiplayer extends
MultiplayerAPIExtension` and holds an inner stock `SceneMultiplayer`
(`MultiplayerAPI::create_default_interface()`). Handshake, auth and peer lifecycle
are delegated to the inner, which also carries goldnet's own packets; goldnet overrides the
object-configuration + poll path that drives state replication, and the `@rpc` path (see
**RPCs on the netchan**). It relays the inner's lifecycle signals up to itself so the consuming
game listens on one object.

**Path-hash identity, no handshake.** Every owned node is keyed by
`net_id = hash(scene_path)`. The same node lives at the same path on server and
client, so ids match with no negotiation and no path-cache to poison — a client
simply drops a `net_id` it doesn't yet hold and self-heals.

**Two registries, by lifecycle — not by entity kind.** goldnet is fully
entity-agnostic; players, bots, projectiles, and map movers all flow through one
pipeline. A node lands in:

- **the core server's entities** when its `MultiplayerSynchronizer` enters the tree with at
  least one per-tick property (`_object_configuration_add`). This is the
  per-tick delta stream, and it covers *both* map-static movers and spawned
  entities.

  Per-tick means **`ALWAYS` or `ON_CHANGE`** — Godot exposes them as separate flags
  (`property_get_sync` / `property_get_watch`), but the snapshot serves both the same
  way, since a slot equal to the peer's acked baseline is skipped anyway. That *is*
  `ON_CHANGE`'s intent, so it needs no separate path. `NEVER` carries no per-tick
  state and stays out (its value still rides the spawn payload).

  One semantic difference to know: stock sends each `ON_CHANGE` write reliably, so
  every transition is observed. goldnet converges instead — if a property goes
  A→B→A between two acked frames, the peer sees only the final A. That holds for
  every slot; goldnet carries *state*, not events. Put events on `@rpc`.

**Commands, the other way.** A client's input to the server (Quake's usercmds) is a stream
in which every element matters, so it can't converge the way state does. Mark the `@rpc` a
client calls once per command with `CommandStream.mark(node, &"method")`. Its contract is a
reliable RPC's — every call reaches the server once, in order — and goldnet keeps it the way
Quake does instead of with ENet's reliable channel: each unreliable uplink packet carries the
commands not sent yet, two backups, and any unacked one out longer than a round trip and a half,
delta-coded against the command before it. The server's ack rides each snapshot header, and the
client's snapshot ack rides each uplink packet. Without goldnet the marked method is just the
reliable RPC it was declared as. See **Usage** and [`docs/protocol.md`](docs/protocol.md).

**RPCs on the netchan.** Every other `@rpc` between the server and a client rides goldnet's
packets too, the way GoldSrc's user messages rode its netchan: a reliable RPC in the packet's
reliable block, an unreliable one in its datagram, in both directions. One reliable block is in
flight at a time and goes again only when an ack proves it lost; a call bigger than a block moves
in fragments, up to 32 in flight, one per packet (GoldSrc sent one per round trip); the datagram
goes whole or not at all, and never plays after a newer packet's. So state and events share one ordered stream: a death RPC can't be overtaken by
the snapshot that hides the body, and the peer's rate counts every byte. `call_local` and
`get_remote_sender_id()` behave as with the inner. What stays on the inner: a client's call to
another client, an unreliable call bigger than 512 bytes, and any method a node lists in its
`gn_bypass` meta.
- **a spawn record** when it is born from a wrapped `MultiplayerSpawner.spawn()`.
  Map-static entities load with the map (both peers already have them) so they need
  no spawn record; runtime entities do, because the client has no other way to know
  they should exist.

**The snapshot protocol (delta against acked baseline).** Each tick, per peer, the
server emits one unreliable snapshot:

```
[u8 version]     wire version — a mismatched build is dropped, not misparsed
[u16 seq][u16 base_seq][u32 server_time][u16 command_ack]
[u16 spawn_ct]   { [u32 net_id][u32 spawner_net_id][var data] } *   (reliable-until-acked)
[u16 despawn_ct] { [u32 net_id] } *                                 (reliable-until-acked)
[u16 leave_ct]   { [u32 net_id] } *                                 (reliable-until-acked, opt-in — see PVS below)
[u16 changed]    { [u32 net_id][uvarint changed_mask]{ value } per set bit } *
```

The byte-level contract — primitives, every value tag, the ack, and what each end does with a
snapshot — is in [`docs/protocol.md`](docs/protocol.md).

- `base_seq` is the frame this peer last acked (0 = full state). The server keeps a
  per-peer ring of recent frames; the client keeps a matching ring and acks the
  newest frame it applied. On loss the ack stalls, the server keeps diffing against
  the same acked baseline, and the next snapshot re-carries the accumulated delta —
  no retransmit, no desync. If the baseline ages out of the ring (~1 s of loss), the
  server falls back to a full frame.
- `changed_mask` indexes the config's sync-property "slots"; an entity whose every
  slot equals the baseline is **skipped entirely** (idle movers/entities cost zero
  bytes — provided the game doesn't rewrite a field every tick). The mask is a
  uvarint, so the usual handful of low bits costs one byte rather than four.
- The entity section is **budgeted**: it gets whatever a safe UDP payload (1200 B)
  has left after the header and the spawn/despawn/leave sections, so a first full
  baseline or a burst of simultaneous changes can't silently overrun the MTU. What
  doesn't fit is simply left out of the peer's next baseline, so the ordinary
  changed-since-baseline compare re-offers it next tick — no retransmit bookkeeping.
  Which entities win the budget is ordered by how long each has been waiting for
  *this* peer, times an optional per-entity `gn_priority` weight (see **Usage**), so
  the most-overdue entity goes first and nothing starves.
- A snapshot with nothing to report — no changed entity, no spawn, despawn, or leave
  — **isn't sent at all**, so a fully static world costs nothing per tick. The one
  exception is the clock feed below.
- Spawns/despawns are **reliable-until-acked**, resent on loss rather than every
  frame: sent with two backups, then again once overdue (a round trip and a half),
  until the peer acks a frame carrying them. A spawn's source record outlives
  delivery, so each peer keeps a durable `spawn_acked` set — a spawn is delivered
  exactly once, never re-armed frame after frame.
- The **leave** section is the opt-in PVS-relevance channel (off by default;
  `leave_ct` is 0 and costs 2 bytes when disabled): net_ids that dropped out of the
  peer's PVS since last tick, delivered reliable-until-acked so the client can hide
  them. Re-entry needs no event — the entity simply reappears in `changed` with a
  full baseline. See **Per-peer visibility** below.
- `server_time` is the server's send-time (ms), carried by every snapshot, making it
  an **always-on server-clock feed** — the client re-emits it as the
  `server_time_received(server_time_ms)` signal, which you feed to the `ServerClock`
  helper for interpolation timing. No separate server-time beacon RPC is needed on
  the goldnet path. "Always-on" is maintained deliberately rather than falling out of
  entity traffic: the empty-snapshot skip above is bounded at 250 ms, past which a
  header-only snapshot goes out anyway. Otherwise the feed would go quiet exactly when
  the world is idle, and `ServerClock` would free-run on local time — a drift with no
  error message attached.

**Compact value encoding.** Each changed slot is written with a 1-byte type tag
plus a tight payload — `f32` for floats, zig-zag varint for ints, 3×`f32` for
`Vector3`, a byte for bool, and a `put_var` fallback for anything else. This keeps
the stream self-delimiting (any `Variant` still round-trips) while roughly halving
per-entity bytes versus stock `put_var` (which tags every value with a 4-byte type
header and stores floats/ints at 64-bit width): a moving player's ~8 props drop
from ~112 B to ~48 B. Opt-in lossy tags (`gn_quant`, see **Usage**) go further:
`angle16`/`angle8` fold a radian onto 2 or 1 bytes, `half`/`vec3_half` drop floats
to binary16, and `time_delta` encodes an int ms stamp as a varint offset from the
packet's own header time, so a same-tick stamp stays ~1 byte however long the server
has been up. Every tag is self-describing, so the decoder needs no matching config.

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

### The snapshot stream alone

Same scenario on a 4-client run, measured as goldnet's own `dbg_bytes` under `GOLDNET_DEBUG=1`
rather than at the socket, where ENet acks and the game's own `@rpc` traffic make up about half of
egress. The snapshot stream averages **~6 KB/s in total (1.8–10.5 across runs)**, and its cost
tracks change: a peer whose PVS is busy costs the most, an idle one **0.2–0.3 KB/s**, which is the
clock keepalive and nothing else (17 B header x 4 Hz x 4 peers = 272 B/s). Game-side quantization
hints (`vec3_half`/`angle8`/`time_delta`) did not separate from that at this sample size. The entity
budget keeps every send under the MTU, including a joining client's first full baseline.

## What we wish Godot had

Every item here is a workaround in this repo, not a wishlist. They're recorded so the next
person hitting one knows it's the engine, not the design — and so they can be deleted if
upstream ever closes the gap.

**1. A change notification for replicated properties.** There is no way to learn that a
property was written: no `NOTIFICATION_PROPERTY_CHANGED`, no per-property signal, and no way
to install a write barrier on another object's property from GDExtension (`_set`/setters only
fire for the script that declares them). So the only way to discover what changed is to read
everything and compare. Godot's own `property_get_watch` works the same way — it re-reads at
`replication_interval` — so this isn't a mechanism being withheld, it doesn't exist.

*Forces:* the `mark_dirty()` API, where the game promises to announce
its own writes. On a 390-entity map that turned a 2.8 ms/tick poll into 0.21 ms. The promise is
scoped per slot rather than per entity — a sync's `gn_push` meta lists the properties it covers,
and every property it doesn't name is polled every tick — so the mode is bounded: forgetting to
mark a property nobody declared costs a read, not a silent desync. That's the whole reason the
poll can't be dropped outright, and it's why there is no audit mode.
*Would fix it:* a `property_changed` notification an object can opt into, or a synchronizer mode
where the setter marks the sync dirty instead of a poll discovering it — what Unreal's property
dirtying and Source's `SendProp` change flags do. Either would let `gn_push` cover every slot.

**2. `MultiplayerSynchronizer::is_visible_to()` bound to GDExtension.** The engine already has
per-peer visibility with filter callbacks, but the query isn't exposed, so an extension can't ask
"is this sync visible to peer N" through the engine's own filter chain.

*Forces:* goldnet reads `is_visibility_public()` / `get_visibility_for()` directly, which means
the game must push per-peer visibility itself every net tick (WizardWars does this in
`NetworkManager.push_pvs_visibility`) rather than registering a filter and letting the engine
answer. Visibility *filters* are effectively unusable from an extension.

**3. A spawner you can see before its first spawn.** `MultiplayerSpawner` is only revealed to a
MultiplayerAPI via `object_configuration_add` — which fires on the first spawn, by which point
the spawn data is already gone.

*Forces:* `_wrap_spawner` hooks `SceneTree.node_added` plus a one-time scan of the existing tree,
and replaces the spawner's `spawn_function` with a trampoline purely to capture reconstruction
data. A `spawner_registered` hook, or spawn data available on the configuration callback, would
delete all of it.

**4. Somewhere to put per-property metadata.** `SceneReplicationConfig` has no per-property
metadata, so an extension that wants a hint per slot (goldnet's quantization tags: `angle16`,
`half`, `vec3_half`) has nowhere in the config to put it.

*Forces:* the hints ride a node `meta` dictionary (`gn_quant`) keyed by property leaf name, which
is a second source of truth that has to be kept aligned with the config by hand. The alignment
part is closable without upstream: `SyncSpec` takes one table and derives the config *and* the
metas from it, so the two cannot drift and a consumer never writes a meta key by hand. What
stays is the keying — leaf name, not path, because the meta is a flat dictionary hung off the
node — so two replicated properties that share a leaf name still cannot be hinted apart, and
`SyncSpec` can only report that rather than fix it.

**Not a gap, just the floor:** `get_indexed` across the GDExtension boundary costs ~0.8 µs per
slot, and after caching the resolved read plan that's essentially all the read loop is. Reading
fewer properties is the only way past it — hence (1).

## Related projects

Part of a set of Godot 4 libraries for GoldSrc-style games:

- [goldsrc-godot](https://github.com/alanfischer/goldsrc-godot): BSP/MDL/SPR/WAD loading, PVS, hull data
- [hop-godot](https://github.com/alanfischer/hop-godot): hop physics as a `PhysicsServer3D` backend, with BSP hull traces
- [pmove-godot](https://github.com/alanfischer/pmove-godot): GoldSrc player movement and client prediction
- **goldnet-godot**: snapshot replication behind Godot's `MultiplayerAPI`

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

Standalone — no godot-cpp, no engine, runs in about a second. It covers the core in
`core/include/goldnet/`:

- `test_codec`: the zig-zag and unsigned varint codecs (round-trip, encoded widths, and
  malformed over-long streams, which are reachable from the wire), the angle16/angle8
  quantizers (round-trip, wraparound, non-finite input), the half-float codec (every
  non-NaN value, and the round-to-nearest-even edges), the bounds-checked `Reader`, and
  the seeded loss PRNG.
- `test_seq`: uint16 sequence comparison across rollover and the reliable-until-acked
  bookkeeping.
- `test_protocol`: a real `goldnet::Server` feeding real `goldnet::Client`s over a fake
  link that drops snapshots or acks, with plain ints as values — deltas against the
  acked baseline, self-heal after a lost snapshot or ack, ack aging into a full frame,
  lazy and reliable spawns, despawns scoped to holders, derived PVS leaves (including
  re-entry before the leave is acked), the packet budget and its priority order,
  per-peer cadence, push-declared slots, the clock keepalive, warmup defer, and
  reordered, truncated or mismatched packets.
- `test_netsim`: config clamping, one seed replaying loss and latency together, the
  latency range and loss rate, the in-order delay pipe, the spike cycle, and the link
  cap (serialization, overhead, per-link independence, the full-buffer drop rule).

Scope is deliberate. godot-cpp's engine classes call through GDExtension function
pointers that only exist inside a running Godot process, so the Godot adapter in `src/`
— synchronizers, spawners, `Variant` encoding — is covered by the integration suite
below instead.

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

Covers the three helpers the addon ships alongside the extension — the client-side
`InterpolationBuffer` and `ServerClock`, plus `SyncSpec`, which builds a synchronizer's config and
goldnet's metas from one declaration. They're pure GDScript with no
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
   # the tree. Recognized hints:
   #   "angle16"    radians → u16   (~0.0055° steps, 2 B)
   #   "angle8"     radians → u8    (~1.4° steps, 1 B — classic GoldSrc precision)
   #   "half"       float   → binary16
   #   "vec3_half"  Vector3 → 3x binary16
   #   "time_delta" int ms stamp → varint offset from the packet's header time. For
   #                absolute ms timestamps only; ignored on non-int slots, since
   #                truncating a float here would corrupt it rather than shrink it.
   sync.set_meta("gn_quant", { "yaw": "angle16", "pitch": "angle16" })
   ```
   The hints and the replication config are two lists that have to agree (see **What we
   wish Godot had** #4), so prefer declaring them together and letting `SyncSpec` — the
   addon's GDScript helper — derive both. A hint keyed to a property the config does not
   replicate is ignored in silence; a table cannot make that mistake, and `SyncSpec`
   reports the ones it cannot rule out (an unrecognised hint name, two properties sharing
   a leaf name):
   ```gdscript
   const SPEC := {
       "NetInterp:net_pos":   {"quant": "vec3_half", "push": true},
       "NetInterp:net_stamp": {"quant": "time_delta", "push": true},
       "NetInterp:net_anim":  {},   # replicated, full precision, polled every tick
   }
   node.add_child(SyncSpec.build("NetSync", SPEC))
   # or, where the synchronizer is baked into a .tscn and the scene owns the config:
   SyncSpec.apply($NetSync, SPEC)   # stamps the metas, reports drift from the config
   ```
6. (Optional) Weight an entity for the snapshot's entity budget. When more entities
   change in a tick than fit in one packet, candidates are ordered by ticks-waited x
   this weight, so a heavier entity wins the budget more often — without goldnet
   knowing what any of them are. Default 1.0; read **once**, on the entity's first
   tick, so set it before the sync enters the tree. Ticks-waited keeps rising for
   whatever loses, so a weight biases the order without starving anything.
   ```gdscript
   sync.set_meta("gn_priority", 4.0)   # players over debris
   ```
7. (Optional) Limit a peer's link with its **rate** (GoldSrc's `rate`): the most bytes per
   second to send it, snapshots and `@rpc` traffic both counted. Each send pushes the peer's
   clear time out by its size over the rate; while that's in the future the peer is
   **choked** — it gets no snapshot that tick, and the next one it does get deltas against
   its acked baseline, so it carries the latest state rather than a backlog. A slow link
   then gets fewer snapshots instead of a queue in front of every one of them. RPCs on the
   netchan wait for the peer's next snapshot like everything else; ones on the inner (see
   **RPCs on the netchan**) still go out when choked, but they count.
   ```gdscript
   gn.set_peer_rate(peer, 5000)      # bytes/sec; 0 = unlimited (the default)
   ```

   A peer can also be served less often than the server ticks (GoldSrc's `cl_updaterate`),
   without slowing anyone else: the tick still runs at full rate and only skips that peer until
   its interval has passed. It is dropped with the peer on disconnect, so set it again on
   reconnect.
   ```gdscript
   gn.set_peer_snapshot_interval_ms(peer, 50)  # 20 Hz for this peer; 0 = every tick (the default)
   ```

   By default snapshots go out on goldnet's own clock, from poll, at the synchronizers'
   `replication_interval`. A game with a fixed server tick can instead send them itself,
   once per tick after writing that tick's state — GoldSrc's `SV_SendClientMessages` at the
   end of `SV_Frame`. A timer that isn't locked to the tick drifts against it: some ticks go
   out twice (the second with nothing new) and some never go out on their own, which a
   client sees as uneven gaps. Per-peer intervals and rates still apply.
   ```gdscript
   gn.manual_send = true             # goldnet's timer sends nothing
   gn.send_snapshots()               # call once per server tick, after the tick's writes
   ```

8. (Optional) Skip the per-tick poll for properties your game already knows it wrote.
   Declaring a property in a sync's `gn_push` meta makes goldnet read it only when the
   game announced it with `mark_dirty(node_or_sync)`; on a 390-entity map that is a
   2.8 ms/tick poll down to 0.21 ms.

   There is no switch to flip: **the `gn_push` meta IS the opt-in**, and the promise is
   **per property, not per entity**. The meta lists the leaf names the game commits to
   announcing; every property it does not name is polled every tick, and a sync that
   declares nothing is polled entirely. So it is bounded — the worst a wrong declaration
   can do is stall the properties it explicitly named, and a property added to the config
   later can never silently go stale because nobody promised for it.
   ```gdscript
   # position is written by one publish funnel that calls mark_dirty; anything else on
   # this sync is left undeclared and keeps being polled.
   sync.set_meta("gn_push", ["position"])
   ...
   node.position = p
   gn.mark_dirty(node)   # accepts the sync, or the node whose state it replicates
   ```
   `SyncSpec` carries this in the same table as the quantization hints — `{"push": true}`
   per property — so the promise sits next to the property it is made about.
   Read **once**, on the entity's first tick, like `gn_quant` and `gn_priority`.

   Per-peer snapshot stats go to stdout via `GOLDNET_DEBUG=1` (env only — there is no
   runtime property for it). Network-condition
   simulation is one `sim_config` dictionary property — keys `loss_percent`, `seed`,
   `latency_min_ms`, `latency_max_ms`, `spike_ms`, `spike_interval_s`,
   `spike_duration_s`, and a slow link's `link_bps` / `link_queue_ms` / `link_overhead` —
   which **replaces** the whole config on write, so `sim_config = {}`
   is the off switch, a read-back matches what was written, and an unknown key is an
   error rather than a silent reset. To change one knob, read-modify-write; every
   setter is idempotent, so that disturbs nothing else (including the seed). To receive
   PVS leave events, `set_relevance_events(true)` and connect the
   `entity_relevance_lost(sync)` signal (see **How it works → Per-peer visibility**).

   The link cap models a modem or BLE connection rather than a lossy one: sending over
   `link_bps` queues, so delay grows, and only once a link has `link_queue_ms` of backlog
   are unreliable sends dropped (reliable ones keep queuing, as ENet would keep
   retransmitting them). Each endpoint caps its own send leg, per peer: a server's
   cap applies to each client's link separately, a client's to its one uplink. Headless,
   `GOLDNET_LINK=bytes_per_sec[,queue_ms]` sets it, alongside `GOLDNET_LOSS`,
   `GOLDNET_LATENCY=min,max`, `GOLDNET_SPIKE=ms,interval,duration` and `GOLDNET_SIM_SEED`.

   `sim_config` is this endpoint's send leg, so on its own a client can only slow its uplink.
   `recv_sim_config` takes the same keys and applies them to what a client **receives**, so a
   client alone can play over a slow, lossy downlink with nothing set on the server: loss
   drops unreliable packets, latency and spikes delay every packet, and `link_bps` queues
   them. goldnet puts a client's peer behind a wrapper that holds arriving packets in the
   same sim, and `multiplayer_peer` still returns the peer the game installed. It applies
   live at any point in a session, and passes packets straight through while it is off.
   ```gdscript
   # 28.8k modem, both legs, from the client alone
   gn.sim_config      = {"latency_min_ms": 80, "latency_max_ms": 120, "link_bps": 3000}
   gn.recv_sim_config = {"latency_min_ms": 80, "latency_max_ms": 120, "link_bps": 3000}
   ```
9. (Optional) Send client input as a **command stream**. Declare the method a client calls once
   per command as a reliable `@rpc`, and mark it before the first call:
   ```gdscript
   @rpc("any_peer", "call_remote", "reliable")
   func submit_input(cmd: PackedByteArray) -> void:
       var sender := multiplayer.get_remote_sender_id()
       ...

   func _ready() -> void:
       CommandStream.mark(self, &"submit_input")
   ```
   The game code is the same with goldnet or without: every `submit_input.rpc_id(1, cmd)` runs
   once on the server, in order, with the right sender. goldnet carries it over the uplink (see
   **How it works**) instead of ENet's reliable channel, so a lost packet costs nothing while
   the next one is on its way, and a long loss costs at most a round trip and a half. Only a
   client's calls to the server are carried this way. A command still unacked after
   `COMMAND_RING` (about a second at 60 Hz) is given up on, the one way it is weaker than the
   reliable RPC. A lone `PackedByteArray` argument goes as its own bytes; each command is
   delta-coded bytewise against the previous one, so a fixed layout compresses best.
   ```gdscript
   gn.command_rate = 30          # packets/sec (GoldSrc cl_cmdrate); 0 = as commands are made
   ```
10. (Optional) Keep an RPC off the netchan. A method a node lists in its `gn_bypass` meta goes
   through the inner `SceneMultiplayer`, with no ordering against the netchan's: for traffic with
   its own pacing, like voice, that shouldn't compete with the snapshot's budget.
   ```gdscript
   set_meta("gn_bypass", [&"sync_voice"])
   ```
11. Read a link's stats with `get_peer_stats(peer)` (a client passes 1). The values are running
   totals, so diff two reads for a rate; every key is present on both ends and 0 where it doesn't
   apply.
   ```gdscript
   gn.get_peer_stats(peer)
   # server: chokes (snapshots withheld by the rate), messages_queued, messages_delivered
   # client: snapshots_received, snapshots_lost, messages_queued, messages_delivered,
   #         commands_pending, commands_dropped, command_rtt_ms (current, not a total),
   #         recv_sim_bytes, recv_sim_packets (what recv_sim_config let through; count these, not
   #         the transport's counters, while it is on)
   ```

> **Headless note:** Godot registers `.gdextension` files via
> `.godot/extension_list.cfg`, refreshed by an editor project scan (or export).
> To add the extension without opening the editor, append
> `res://addons/goldnet/goldnet.gdextension` to that file manually. WizardWars gates
> the install behind the `--goldnet` command-line flag.

## Using the core without Godot

`core/include/goldnet/` is header-only C++17 with no dependency beyond the standard library: add
it to the include path. `goldnet::Server<V, X>` and `goldnet::Client<V>` are templates over your
engine's value type `V` (default-constructible, copyable, comparable with `!=`) and optional
per-entity storage `X`. They reach the engine only through a host object you pass in, whose
methods are listed at the top of `server.h` and `client.h`, and `server.h` also explains how a
runtime entity's spawn and its state stream fit together. `tests/test_protocol.cpp` has a
complete minimal pair, `TestServerHost` and `TestClientHost`, over plain ints.

`core/example/demo.cpp` is a runnable one over real UDP (macOS / Linux): a server bouncing
balls that spawn and despawn, and clients that draw them and send it a command every tick over
the uplink, with `NetSim` loss/latency on either end and a per-peer rate on the server.

```bash
cmake -S core/example -B core/example/build && cmake --build core/example/build
core/example/build/goldnet_demo server --loss 20 --latency 60
core/example/build/goldnet_demo client --loss 20 --latency 60   # another terminal; as many as you like
```

On the server:

```cpp
goldnet::Server<MyValue> server;
server.set_packet_overhead(28);     // what your link adds per packet, for rate budgets

server.add_entity(key, net_id);     // an entity starts replicating; remove_entity(key)
server.add_spawn(net_id, record);   // a runtime entity clients must create; despawn(net_id)
server.mark_dirty(key);             // a pushed slot was written (see gn_push)

server.tick(now_ms, peers, host);   // every tick: reads state, sends each due peer a snapshot
server.on_ack(peer, seq);           // each ack the client sends
server.remove_peer(peer);           // on disconnect
```

Per-peer cadence and rate are `set_peer_interval_ms` and `set_peer_rate`; anything else you send a peer counts against its rate through `charge()`.

On the client, `client.apply(bytes, size, host)` for each snapshot received and `client.reset()`
for a new session. The host's `send_ack(seq)` hands over the snapshot ack, which the next uplink
packet carries. The uplink (`uplink.h`) is the other direction:

```cpp
goldnet::CommandSender commands;              // client
commands.push(stream_id, bytes, size, now);   // one command
if (commands.due(now)) {                      // or whenever an ack is waiting
    goldnet::Writer w;
    commands.write(now, snapshot_ack, w);     // send w to the server
}
commands.on_ack(seq, now);                    // from the client host's command_ack(seq)

goldnet::CommandReceiver receiver;            // server
receiver.receive(peer, bytes, size, host);    // host gets snapshot_ack() and each command() in order
server.set_command_ack(peer, receiver.ack(peer));
```

Both directions expect an unreliable datagram link; the wire format is in
[`docs/protocol.md`](docs/protocol.md).
`goldnet::NetSim` (`netsim.h`) adds simulated loss, latency, spikes and a slow link to your send
path; its header shows the calls.

## Debugging

- `GOLDNET_DEBUG=1` — periodic per-peer snapshot stats (send KB/s; per-snapshot
  `spawn` / `despawn` / `changed` counts and byte size on the client).
- `GOLDNET_LOSS=<pct>` — drop that percentage of outbound snapshots server-side, to
  exercise the ack-stall self-heal without a real lossy network.
- `GOLDNET_SIM_SEED=<n>` — make the whole sim reproducible: both the loss rolls and the
  latency draws come from one seeded generator. Unset (or `0`) uses the engine RNG, so
  every run drops different packets and draws different delays — fine for poking at
  self-heal, useless for a regression test you need to re-run or bisect. Also settable as
  the `seed` key of the `sim_config` property.
- `GOLDNET_RPC_NETCHAN=0` — send this end's RPCs through the inner `SceneMultiplayer` instead of
  the netchan, to compare the two.

## License

MIT — see [LICENSE](LICENSE).
