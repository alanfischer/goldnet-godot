# goldnet example

A ~90-line, standalone drop-in demo. It uses **only stock Godot nodes** — a
`MultiplayerSpawner` and `MultiplayerSynchronizer`s — and the sole goldnet-specific line
is installing the API:

```gdscript
get_tree().set_multiplayer(ClassDB.instantiate(&"GoldNetMultiplayer"))
```

The server spawns 3 cubes and moves them every frame; the client connects and receives
the cubes plus their moving positions over the GoldSrc ack-delta snapshot stream — no
replication code, no per-entity RPCs.

## Run it

Build the addon first (from the repo root), so `addons/goldnet/bin/` has a library:

```bash
../build.sh macos        # or: ../build.sh   (all platforms)
```

Then launch a server and a client (two terminals), headless:

```bash
godot --headless --path . -- --server
godot --headless --path . -- --client
```

The client prints, once per second:

```
[client] connected
[client] t=1 received 3 cubes: Cube0=(0.30, 0.00, 1.98) Cube1=(-2.25, 0.00, 1.98) ...
[client] t=2 received 3 cubes: Cube0=(-1.47, 0.00, 1.35) ...
```

— the positions change each second, proving spawns **and** per-frame deltas arrive. Set
`GOLDNET_DEBUG=1` to also see per-snapshot `spawn/despawn/changed/bytes` stats.

## Notes

- `addons/goldnet` is a symlink to the repo's `../addons/goldnet` so the demo shares the
  one built library. `.godot/extension_list.cfg` registers the GDExtension so it loads
  headless without first opening the editor.
- The server defers its initial `spawn()` by one frame (`await get_tree().process_frame`)
  so goldnet has wrapped the spawner before entities are created. In a real game, entities
  spawn well after setup, so this only matters when spawning in the spawner's own first frame.
- Client-side smoothing (`InterpolationBuffer`) and local prediction (`PredictedBody`) are
  shipped in `addons/goldnet/` as optional helpers; this demo keeps things minimal and just
  applies the replicated state directly.
