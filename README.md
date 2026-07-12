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

Two honest limitations at this phase, both resolved later:
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

Later phases replace the encoding (Phase 2 ack-delta), fold spawns/despawns into
the stream (Phase 3), and add client helper nodes (Phase 4). Set `GOLDNET_DEBUG=1`
for periodic per-peer snapshot stats (entities / bytes / matched-on-apply).

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
