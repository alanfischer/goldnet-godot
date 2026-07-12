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
this proves the composition boundary before any custom protocol exists. Verified
in WizardWars: headless host + client, synchronizers/spawners/RPCs/connect/
disconnect all behave identically, zero errors.

Later phases replace only the *state-replication* layer (intercept
synchronizer/spawner configs in `_object_configuration_add`, drive them in
`_poll` as ack-delta snapshots) while still delegating handshake, auth, peer
lifecycle, and all `@rpc` traffic to the inner `SceneMultiplayer`.

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
