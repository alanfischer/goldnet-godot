# goldnet example

A ~140-line, standalone drop-in demo. It uses **only stock Godot nodes** — a
`MultiplayerSpawner`, `MultiplayerSynchronizer`s and an `@rpc` — and the goldnet-specific
lines are installing the API:

```gdscript
get_tree().set_multiplayer(ClassDB.instantiate(&"GoldNetMultiplayer"))
```

and marking the input RPC as a command stream, which is inert without goldnet:

```gdscript
CommandStream.mark(self, &"submit_input")
```

The server spawns 3 cubes and moves them every frame; the client connects and receives
the cubes plus their moving positions over the GoldSrc ack-delta snapshot stream — no
replication code, no per-entity RPCs.

The other way, the client calls `submit_input` every physics tick with a 5-byte command (its
number and a height), the way Quake sends usercmds. goldnet carries those calls over its uplink
instead of ENet's reliable channel, and the server sets Cube0's height from the newest one, so
the client sees its own input come back in the snapshots.

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
[client] t=1 received 3 cubes: Cube2=(-3.89, 0.00, -0.92) Cube1=(-2.16, 0.00, 2.09) Cube0=(0.39, 0.95, 1.96)
[client] t=2 received 3 cubes: Cube2=(-1.20, 0.00, -3.81) Cube1=(-2.90, 0.00, -0.79) Cube0=(-1.48, -0.55, 1.34)
```

— the positions change each second, proving spawns **and** per-frame deltas arrive, and
Cube0's height follows the client's commands. The server prints how many commands arrived and
whether any came out of order:

```
[server] t=3 cube0.pos=(-1.98, -0.94, 0.28), 72 commands/s, 0 out of order
```

Set `GOLDNET_DEBUG=1` to also see per-snapshot `spawn/despawn/changed/bytes` stats, and
`GOLDNET_LOSS=20` on both ends to watch the commands survive packet loss: a second's count
dips and the next catches up, still in order.

## Notes

- `addons/goldnet` is a symlink to the repo's `../addons/goldnet` so the demo shares the
  one built library. `.godot/extension_list.cfg` registers the GDExtension so it loads
  headless without first opening the editor.
- The server defers its initial `spawn()` by one frame (`await get_tree().process_frame`)
  so goldnet has wrapped the spawner before entities are created. In a real game, entities
  spawn well after setup, so this only matters when spawning in the spawner's own first frame.
- Client-side smoothing (`InterpolationBuffer`) is shipped in `addons/goldnet/` as an optional
  helper; this demo keeps things minimal and just applies the replicated state directly. Local
  prediction is not part of this addon — it lives in pmove-godot, which owns the movement it
  reconciles.
