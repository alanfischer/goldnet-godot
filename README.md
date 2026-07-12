# goldnet-godot

A Godot 4 GDExtension that provides `GoldNetMultiplayer`, a drop-in
`MultiplayerAPI` implementing **GoldSrc/Quake-style netcode** — an unreliable,
delta-against-acked-baseline snapshot stream served **behind the stock
`MultiplayerSynchronizer` / `MultiplayerSpawner` API**, plus client-side
interpolation/prediction helper nodes.

It is the netcode sibling to [`goldsrc-godot`](../goldsrc-godot) (the asset
loader) and mirrors the [`hop`](../hop-godot) split: phases 0–5 build it as a
single GDExtension; phase 6 extracts an engine-agnostic **goldnet** core with
this repo as the **goldnet-godot** binding. See
`../../docs/goldsrc_netcode_plan.md` for the full plan.

## Status

**Phase 0 — pass-through spike (complete).** `GoldNetMultiplayer` extends
`MultiplayerAPIExtension` and forwards every virtual to an inner
`SceneMultiplayer` (`MultiplayerAPI::create_default_interface()`), relaying the
inner's lifecycle signals up to itself. Installing it changes nothing observable —
this proves the composition boundary before any custom protocol exists.

**Phase 1 — registry + full-state snapshot (complete).** We now intercept the
synchronizers we can fully own — *map-static* ones (doors, platforms), identified
by their owner node not being a direct child of any `MultiplayerSpawner`'s
spawn-parent — into a registry, and stream their complete state ourselves each
tick as one unreliable snapshot per peer (`GoldNetLink` RPC carrier). The client
applies it (`set_indexed` each property) and re-emits the synchronizer's
`synchronized` signal, which is how the consuming game reacts to replicated state.
Spawner-managed synchronizers (players, projectiles) and all `@rpc` still flow
through the inner. Verified in WizardWars: 381 map-entity synchronizers stream and
apply through our path, no path cache involved (entities are keyed by a hash of
their scene path, identical on both peers). This is "old netcode (ALWAYS
full-state) on our path" — parity, not yet the delta.

**Phase 2 — delta against acked baseline, the GoldSrc core (complete).** Each new
frame carries a sequence number and the baseline it is diffed against (the peer's
last-acked frame). The server keeps a per-peer ring of recent frames; the client
keeps a matching ring and acks the newest frame it applied (unreliable_ordered,
newest wins). An entity record is `[u32 net_id][u32 changed_mask]{[var value] per
set bit}` where the mask indexes the config's sync-property "slots"; an entity
whose every slot equals the baseline is skipped entirely. On packet loss the ack
stalls, so the server keeps diffing against the same acked baseline and re-carries
whatever changed since — the client reconstructs from its ring with no desync and
no retransmit. Verified in WizardWars: acks flow (baseline = previous frame),
per-slot skipping drops ~60→~20 B/entity (3.1x on the wire), and under `GOLDNET_LOSS=30`
the server falls back to older baselines (`seq=320 base=317`) with the client still
tracking correctly — zero crashes, zero desync.

> **Note on the full "static = 0 bytes" ideal:** goldnet skips any entity whose
> fields are unchanged, but WizardWars' movers write a fresh `mover_net_stamp =
> Time.get_ticks_msec()` *every* tick (a leftover of the stock ALWAYS design), so
> every mover always has one changed slot and is never skipped whole. The frame
> header already carries a server timestamp, making the per-entity stamp redundant;
> dropping it — and decoupling the mover *presence* watchdog from per-tick sync
> (idle movers currently deactivate after 200 ms of silence) — is the game-side
> dogfood change (Phase 4) that unlocks true idle-free replication. The module
> itself is complete and correct.

Two limitations that predate this phase and are resolved later:
- **No PVS cull yet.** Godot does not bind `MultiplayerSynchronizer::is_visible_to`,
  so a GDExtension `MultiplayerAPI` cannot evaluate `add_visibility_filter()`
  callbacks. We honor the *exposed* visibility API (`is_visibility_public()` +
  explicit `set_visibility_for`) and stream every publicly-visible owned entity.
  Correctness is safe (a client drops net_ids it hasn't registered and self-heals
  — there is no path cache to poison), but the game's BSP-PVS bandwidth cull is
  not applied. Phase 2's ack-delta makes the dominant static movers ~free
  regardless; restoring the cull needs either an engine/godot-cpp binding for
  `is_visible_to` or a game-side `set_visibility_for` bridge.
- **Full-state, not delta.** Every visible entity's full state ships every tick
  (~60 B/entity). That is the wasteful baseline Phase 2 replaces with
  delta-against-acked-baseline.

**Phase 3 — spawn / despawn, fully agnostic (complete).** goldnet now owns the
MultiplayerSpawners too, so runtime entities (players, projectiles) flow through the
exact same stream as map-static movers — there is no static-vs-spawnable split
anywhere downstream. A spawn is captured by wrapping each spawner's `spawn_function`
(hooked early via `SceneTree.node_added` + a one-time scan, since the spawn event
itself is the first time the API sees the spawner); the reconstruction data rides a
reliable-until-acked spawn record `[node_net_id][spawner_net_id][var data]`. The
client rebuilds the node with the spawner's real function, adds it under the
spawn_path, and fires the spawner's `spawned` signal so the game's per-node
bookkeeping runs; the node's child synchronizer then auto-registers and streams
state like any mover. Despawns are detected by polling (the spawned node was freed),
sent as `[node_net_id]` reliable-until-acked, and applied by freeing the node +
firing `despawned`. Spawn and state use independent hash-id spaces (node path vs.
synchronizer path) that both match across peers. Verified in WizardWars: 2 players +
a stream of projectiles spawn, replicate, and despawn through goldnet with no ghosts
and no leaks (`remaining_spawned` returns to just the live players), zero errors.

Remaining: Phase 4 client helper nodes (`InterpolationBuffer` / `PredictedBody`),
Phase 5 packaging + full build matrix, Phase 6 the engine-agnostic library split.
Set `GOLDNET_DEBUG=1` for periodic per-peer snapshot stats, `GOLDNET_LOSS=<pct>` to
drop snapshots.

## Building

```bash
./build.sh            # all platforms (macos linux windows android)
./build.sh macos      # just one
```

Outputs land in `addons/goldnet/bin/`. The build force-loads the whole godot-cpp
archive (see `src/CMakeLists.txt`): a `MultiplayerAPI` is handed arbitrary engine
objects through its virtuals, so every engine class's instance-binding callbacks
must be linked in, not just the ones this extension names directly.

In this monorepo, `extern/godot-cpp` is a symlink to the shared checkout under
`extern/hop-godot` so we don't vendor a second copy.

## Usage

1. Symlink or copy `addons/goldnet/` into your project's `addons/`.
2. Install it before any `MultiplayerSynchronizer`/`MultiplayerSpawner` enters
   the tree:
   ```gdscript
   get_tree().set_multiplayer(ClassDB.instantiate(&"GoldNetMultiplayer"))
   ```
3. Keep your stock synchronizers, spawners, and `@rpc` methods as-is.

> **Headless note:** Godot registers `.gdextension` files via
> `.godot/extension_list.cfg`, which is refreshed by an editor project scan (or
> the export step). When adding the extension without opening the editor, add
> `res://addons/goldnet/goldnet.gdextension` to that file manually. WizardWars
> gates the install behind the `--goldnet` command-line flag.
