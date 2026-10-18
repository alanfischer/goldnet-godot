# goldnet wire protocol (version 4)

The byte-level contract between `goldnet::Server` and `goldnet::Client`: what a snapshot and an
ack contain, how each value is encoded, and what each end does with them. The README's **How it
works** explains why the protocol is shaped this way; this page is what a second implementation
would need to interoperate. The source of truth is `core/include/goldnet/` — `protocol.h` for the
constants, `codec.h` and `buffer.h` for the encodings, `server.h` and `client.h` for the rules.

## Primitives

Everything is **little-endian**.

| Name      | Encoding |
|-----------|----------|
| `u8` `u16` `u32` | unsigned, 1 / 2 / 4 bytes |
| `f32`     | IEEE 754 binary32 |
| `f16`     | IEEE 754 binary16 |
| `uvarint` | unsigned, 7 bits per byte, low group first; the high bit of each byte means another byte follows. Decoded into 32 bits. |
| `varint`  | signed: zig-zag (`(v << 1) ^ (v >> 63)`) then the same 7-bit groups, decoded into 64 bits. Small magnitudes, negative or not, take one byte. |

A reader never runs past the end of a packet. An overrun marks the read failed and yields zeros
from then on, and the packet is treated as truncated (see **Receiving**). Over-long varints are
drained but stop adding bits once the type is full.

## Carriage

The core hands bytes to its host and leaves delivery to it: `send(peer, packet)` on the server,
`send_ack(seq)` on the client. The protocol assumes an **unreliable datagram** link, where packets
may be lost, duplicated or reordered, and it does its own recovery. A reliable link works, but the
recovery is then wasted and a loss stalls everything queued behind it.

The Godot binding carries both as RPCs on a `GoldNetLink` node at `/root/__GoldNetLink`, through
the inner `SceneMultiplayer` and therefore whatever `MultiplayerPeer` is installed, on channel 0:

| RPC        | Direction       | Transfer mode         | Argument |
|------------|-----------------|-----------------------|----------|
| `_gn_recv` | server → client | `UNRELIABLE`          | the snapshot, as a `PackedByteArray` |
| `_gn_ack`  | client → server | `UNRELIABLE_ORDERED`  | the acked `seq`, as an `int` |

## Snapshot

One per peer per send, server to client.

```
u8   version        SNAPSHOT_VERSION = 4
u16  seq            this frame's number for this peer
u16  base_seq       the frame this one is a delta against; 0 = full state
u32  server_time    the server's clock at send, ms
u16  spawn_ct       then spawn_ct  × spawn entry
u16  despawn_ct     then despawn_ct × u32 net_id
u16  leave_ct       then leave_ct   × u32 net_id
u16  changed_ct     then changed_ct × entity entry
```

Only the first 9 bytes sit at fixed offsets; each count is followed directly by its entries. The
fixed part (header plus the four counts) is `SNAPSHOT_HEADER_BYTES` = 17.

**Spawn entry** — a runtime entity the client must create before any delta can reach it:

```
u32  net_id           the new entity
u32  spawner_net_id   the spawner that creates it; 0 if none
u32  len
len bytes             spawn data, opaque to the protocol (Godot: var_to_bytes of the spawn's data)
```

**Despawn** and **leave** entries are a bare `u32 net_id`. A despawn destroys the entity; a leave
(only sent when relevance events are on) says it dropped out of the peer's PVS. At most
`MAX_LEAVES_PER_SNAPSHOT` = 32 leaves ride one snapshot.

**Entity entry** — the slots of one entity that changed since `base_seq`:

```
u32      net_id
uvarint  mask         bit s set = slot s follows
value    × popcount(mask), in ascending slot order
```

A slot is an index into the entity's replicated properties — in Godot, the order of its
synchronizer's `SceneReplicationConfig`. At most `MAX_SLOTS` = 32. Values are self-delimiting, so
a receiver can skip an entity it doesn't know without losing its place.

## Values

A value is a 1-byte tag followed by its payload. The receiver needs only the tag; which lossy tag
to use is the sender's choice (in Godot, the `gn_quant` meta).

| Tag | Name         | Payload | Decodes to |
|----:|--------------|---------|------------|
| 0   | `OPAQUE`     | `u32 len`, then `len` bytes in the host's own encoding (Godot: `var_to_bytes`) | whatever the host decodes |
| 1   | `FLOAT`      | `f32` | float |
| 2   | `INT`        | `varint` | int |
| 3   | `VEC3`       | 3 × `f32`, x y z | vector3 |
| 4   | `BOOL`       | `u8`, nonzero = true | bool |
| 5   | `ANGLE16`    | `u16` = radians wrapped into [0, τ) × 65536 / τ, truncated, mod 65536 | `u16 / 65536 × τ` |
| 6   | `HALF`       | `f16` | float |
| 7   | `VEC3_HALF`  | 3 × `f16`, x y z | vector3 |
| 8   | `ANGLE8`     | `u8` = radians wrapped into [0, τ) × 256 / τ, truncated, mod 256 | `u8 / 256 × τ` |
| 9   | `TIME_DELTA` | `varint` of (value − this snapshot's `server_time`) | `server_time` + delta, as int |

Angles take NaN as 0. `TIME_DELTA` applies only to integer ms timestamps; the sender falls back
to the type's own tag for anything else.

A decoder reads any tag it doesn't recognise as `OPAQUE`, so a new tag is a wire change: it bumps
`SNAPSHOT_VERSION`.

## Ack

The client acks a `u16 seq`: the newest frame it has **committed** (see below). Acks are not
cumulative records of everything received — only the newest one matters, which is why they travel
unreliable-ordered and a lost one is covered by the next.

## Sequence numbers

`seq` is per peer and counts up from 1. 0 is reserved for "no baseline", so the counter skips it
when it wraps. Two seqs compare on the circle — `a` is newer than `b` when `(int16)(a − b) > 0` —
which holds while they are within 32767 frames of each other.

Both ends keep the last `RING` = 32 frames, in slot `seq & 31`, and check the stored seq before
trusting a slot.

## Sending

Per tick, for each peer:

1. **Cadence.** Skip the peer if its own interval (`set_peer_snapshot_interval_ms`) hasn't elapsed
   since its last send, or if its `rate` hasn't cleared what was already sent to it (a *choke*).
   A peer's first snapshot is never skipped.
2. **Baseline.** `base_seq` is the peer's newest ack if that frame is still in the ring, else 0
   and the snapshot carries full state.
3. **Reliable-until-acked sections.** Spawns, despawns and leaves are included in every snapshot,
   from the first one that carried them, until an ack at or past that first one arrives. A spawn
   goes only to peers the entity is visible to (all of them, when it has no visibility to ask
   about), and a despawn only to peers that were sent its spawn.
4. **Entities.** An entity is a candidate when it is visible to the peer and has any slot that
   changed since the baseline. Each changed slot is written with its current value.
5. **Budget.** The whole snapshot aims for `SAFE_PACKET_BYTES` = 1200. Header and the three
   sections come first; entities get what is left, further capped by the peer's
   `bandwidth_bps` turned into a per-packet allowance. Candidates go in order of how long they
   have waited for this peer times their priority weight. Whatever doesn't fit is left out of
   this frame, so the baseline compare offers it again next time. The first candidate always
   goes in, so a crowded packet can't stall replication.
6. **Empty skip.** A snapshot with no spawn, despawn, leave or entity isn't sent, unless the peer
   has gone `CLOCK_KEEPALIVE_MS` = 250 ms without one — `server_time` is the client's clock feed
   and must not go quiet.

The server stores what each frame holds for the peer under its `seq`, so the next delta can be
built against whichever frame the peer acks.

## Receiving

For each snapshot:

1. **Version.** Not `SNAPSHOT_VERSION` → drop it (and warn once per session).
2. **Order.** `seq` not newer than the last committed frame → drop it.
3. **Clock.** Surface `server_time` — even if the next step drops the snapshot.
4. **Baseline.** If `base_seq` ≠ 0 and that frame isn't in the ring, re-ack the last committed
   frame and drop the snapshot. The server keeps diffing against frames the client holds,
   falling back to full state when it has to.
5. **Apply.** Spawns are applied in order, as read, skipping any net_id the client already created:
   resends of an unacked spawn arrive every snapshot. A spawn the client can't create yet isn't
   counted as created, so a later resend gets another try. The new frame starts as a copy of the
   baseline (empty for `base_seq` 0); each entity entry overwrites its changed slots and is applied
   to the entity. Despawns are applied after the entities, only for net_ids the client created
   (they repeat the same way), then entities that no longer exist locally are dropped from the
   frame, then leaves are dropped from it and reported.
6. **Truncated.** If the packet ran short, keep whatever it applied but neither commit nor ack.
7. **Warmup.** If an entity the client knows of couldn't take its values yet (not ready), re-ack
   the last committed frame instead of this one, for up to 32 frames in a row (and only once some
   frame has been committed; the very first is always accepted). Acking would tell
   the server the entity's state was delivered, and an idle entity would then never be resent.
8. **Commit.** Store the frame under `seq`, make it the last committed frame, and ack `seq`.

The server, on an ack newer than the peer's previous one, makes that frame the peer's baseline and
retires every reliable section entry first sent at or before it.

## Version history

- **v3** — the entity mask became a `uvarint` (was a fixed `u32`); a 1-byte version replaced the
  4-byte magic; empty snapshots stopped being sent.
- **v4** — added `ANGLE8` and `TIME_DELTA`.
