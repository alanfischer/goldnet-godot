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
- **`spawn_records`** when it is born from a wrapped `MultiplayerSpawner.spawn()`.
  Map-static entities load with the map (both peers already have them) so they need
  no spawn record; runtime entities do, because the client has no other way to know
  they should exist.

**The snapshot protocol (delta against acked baseline).** Each tick, per peer, the
server emits one unreliable snapshot:

```
[u8 version]     wire version — a mismatched build is dropped, not misparsed
[u16 seq][u16 base_seq][u32 server_time]
[u16 spawn_ct]   { [u32 net_id][u32 spawner_net_id][var data] } *   (reliable-until-acked)
[u16 despawn_ct] { [u32 net_id] } *                                 (reliable-until-acked)
[u16 leave_ct]   { [u32 net_id] } *                                 (reliable-until-acked, opt-in — see PVS below)
[u16 changed]    { [u32 net_id][uvarint changed_mask]{ value } per set bit } *
```

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
  the most-overdue entity goes first and nothing starves. `bandwidth_bps` tightens
  the budget further; it never relaxes the MTU ceiling.
- A snapshot with nothing to report — no changed entity, no spawn, despawn, or leave
  — **isn't sent at all**, so a fully static world costs nothing per tick. The one
  exception is the clock feed below.
- Spawns/despawns are **reliable-until-acked**: re-sent each frame until the peer
  acks a frame carrying them, then retired. A spawn's source record outlives
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

> **The table predates the wire-format v4 work** (uvarint mask, empty-snapshot skip,
> entity budget, `angle8`/`time_delta`), so read it as goldnet's floor rather than its
> current cost. What that work bought is measured separately below — the shape this
> table is really for (parity on bandwidth, divergence under loss) is unaffected.

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

### What the v4 wire work bought

Same WizardWars scenario (ww_2fort, 4 AI bots, 4 stationary headless clients, 190 s
runs), comparing this build against the pre-v4 one with the *game* held constant. The
figure below is the **snapshot stream alone** — goldnet's own `dbg_bytes` under
`GOLDNET_DEBUG=1`, not the ENet socket total. That distinction matters: snapshots are
only about half of server egress here, the rest being ENet acks and the game's own
`@rpc` traffic, so measuring at the socket dilutes the effect by roughly half.

| | snapshot stream | total socket egress | over-MTU sends |
| --- | --- | --- | --- |
| pre-v4 | 12.4 KB/s (11.3–13.2) | 26.9 KB/s | 7 of 7 runs |
| v4 | 6.2 KB/s (1.8–10.5) | ~21.8 KB/s | 0 of 14 runs |

n=5 runs pre-v4, n=10 v4, averaged over each run's steady-state tail. **Roughly half
the snapshot bytes**, and every v4 run came in below every pre-v4 run (10.5 < 11.3).

Read the spread, not just the mean. Pre-v4 is tight because it pays a floor: a snapshot
per peer per tick whether or not anything changed. v4's cost tracks actual change, so it
ranges from near the pre-v4 figure when a peer's PVS is busy down to **0.2–0.3 KB/s when
it is idle** — which is the clock keepalive and nothing else (17 B header x 4 Hz x 4
peers = 272 B/s, and that is what the log shows). Most of the win is that floor
disappearing; the rest is the uvarint mask. A busy server should expect the low end of
the improvement, an idle or lightly-populated one the high end.

Two caveats worth keeping. Adopting the *game-side* quantization hints on top
(`vec3_half`/`angle8`/`time_delta` — the consumer's own change, not goldnet's) did not
separate from goldnet's schema-agnostic wins at this sample size: 6.2 vs 6.1 KB/s, well
inside the run-to-run spread. And this ran on a 4-core container with server, bots, and
clients on one box, so the absolute numbers are not comparable to the table above —
only the paired comparison is.

The over-MTU column is the entity budget doing its job, and it is the least ambiguous
result here: pre-v4 emitted `Sending 1991 bytes unreliably which is above the MTU (1392)`
on a joining client's first full baseline in **every** run. With the budget, never.

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

*Forces:* the `push_dirty` mode and its `mark_dirty()` API, where the game promises to announce
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

**4. Runtime-added synchronizers that actually pair.** A `MultiplayerSynchronizer` created in
code gets no replication-ID handshake and never pairs with its remote counterpart — it has to be
baked into the `.tscn`.

*Forces:* consumers must ship scenes with synchronizers pre-baked (see WizardWars'
`remote_player.tscn`), so anything assembled at runtime needs a scene file it would not otherwise
need. Only the parts that can't live in a scene — a per-peer visibility `Callable`, receive hooks
— get wired in code.

**5. Somewhere to put per-property metadata.** `SceneReplicationConfig` has no per-property
metadata, so an extension that wants a hint per slot (goldnet's quantization tags: `angle16`,
`half`, `vec3_half`) has nowhere in the config to put it.

*Forces:* the hints ride a node `meta` dictionary (`gn_quant`) keyed by property leaf name, which
is a second source of truth that has to be kept aligned with the config by hand.

**Not a gap, just the floor:** `get_indexed` across the GDExtension boundary costs ~0.8 µs per
slot, and after caching the resolved read plan that's essentially all the read loop is. Reading
fewer properties is the only way past it — hence (1).

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
`src/goldnet_codec.h`: the zig-zag and unsigned varint codecs (round-trip, encoded
widths, and malformed over-long streams, which are reachable from the wire), the
angle16/angle8 quantizers (round-trip, wraparound, non-finite input), uint16 sequence
comparison across rollover, the reliable-until-acked bookkeeping, and the seeded loss
PRNG.

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
6. (Optional) Weight an entity for the snapshot's entity budget. When more entities
   change in a tick than fit in one packet, candidates are ordered by ticks-waited x
   this weight, so a heavier entity wins the budget more often — without goldnet
   knowing what any of them are. Default 1.0; read **once**, on the entity's first
   tick, so set it before the sync enters the tree. Ticks-waited keeps rising for
   whatever loses, so a weight biases the order without starving anything.
   ```gdscript
   sync.set_meta("gn_priority", 4.0)   # players over debris
   ```
7. (Optional) Cap per-peer entity-delta bandwidth in bytes/sec, converted to a
   per-packet allowance via that peer's snapshot interval. Only ever tightens the
   built-in MTU ceiling. 0 (default) leaves the MTU ceiling as the only limit.
   ```gdscript
   gn.bandwidth_bps = 32000              # every peer
   gn.set_peer_bandwidth_bps(peer, 8000) # or per peer, which wins where set
   ```
   Note this budgets the entity-delta section only — the header and the
   spawn/despawn/leave sections are accounted against the MTU ceiling but are not
   rate-limited, and `@rpc` traffic goes through the inner `SceneMultiplayer`
   untouched. It is a replication throttle, not a link-wide one.

8. (Optional) Skip the per-tick poll for properties your game already knows it wrote.
   `set_push_dirty(true)` makes goldnet read an entity only when the game announced it
   with `mark_dirty(node_or_sync)`; on a 390-entity map that is a 2.8 ms/tick poll down
   to 0.21 ms.

   The promise is **per property, not per entity**. A sync's `gn_push` meta lists the
   leaf names the game commits to announcing; every property it does not name is polled
   every tick as if push mode were off. So the mode is bounded — the worst a wrong
   declaration can do is stall the properties it explicitly named, and a property added
   to the config later can never silently go stale because nobody promised for it.
   ```gdscript
   gn.set_push_dirty(true)
   # position is written by one publish funnel that calls mark_dirty; anything else on
   # this sync is left undeclared and keeps being polled.
   sync.set_meta("gn_push", ["position"])
   ...
   node.position = p
   gn.mark_dirty(node)   # accepts the sync, or the node whose state it replicates
   ```
   Read **once**, on the entity's first tick, like `gn_quant` and `gn_priority`.

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
