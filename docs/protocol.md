# goldnet wire protocol (version 8)

The byte-level contract between goldnet's two ends: what a snapshot and an uplink packet contain,
how each value is encoded, and what each end does with them. The README's **How it
works** explains why the protocol is shaped this way; this page is what a second implementation
would need to interoperate. The source of truth is `core/include/goldnet/` — `protocol.h` for the
constants, `codec.h` and `buffer.h` for the encodings, `server.h` and `client.h` for the snapshot
rules, `uplink.h` for the uplink's, `netchan.h` for the channel both carry.

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
`CommandSender::write` on the client. The protocol assumes an **unreliable datagram** link, where packets
may be lost, duplicated or reordered, and it does its own recovery. A reliable link works, but the
recovery is then wasted and a loss stalls everything queued behind it.

The Godot binding carries both as RPCs on a `GoldNetLink` node at `/root/__GoldNetLink`, through
the inner `SceneMultiplayer` and therefore whatever `MultiplayerPeer` is installed, on channel 0:

| RPC        | Direction       | Transfer mode         | Argument |
|------------|-----------------|-----------------------|----------|
| `_gn_recv` | server → client | `UNRELIABLE`          | the snapshot, as a `PackedByteArray` |
| `_gn_up`   | client → server | `UNRELIABLE`          | the uplink packet, as a `PackedByteArray` |

Every other RPC between the server and a client is a message on the channel (see **Channel**), as
GoldSrc's user messages rode its netchan: a reliable RPC in the reliable block, an unreliable one in
the datagram. A message is `uvarint target` and then:

- target 0, a registration: `uvarint id`, then the node path and the method name, each a
  `uvarint` length and UTF-8. Each end registers an id with a peer, reliably, the first time it
  sends that peer a call to the target (GoldSrc's `svc_newusermsg`, done lazily). Until the peer
  has it, an unreliable call to the target is a registration followed by the call's `argc` and
  arguments, so it goes in the datagram, in order with the rest, and needs nothing before it.
- any other target, a call: `uvarint argc`, then each argument as a value (see **Values**), or one
  of the binding's own tags: 64 nil, 65 string and 66 StringName (`uvarint` length, UTF-8),
  67 PackedByteArray (`uvarint` length, bytes), 68 a float an f32 would change (8 bytes, binary64).

## Snapshot

One per peer per send, server to client.

```
u8   version        SNAPSHOT_VERSION = 8
u16  seq            this frame's number for this peer
u16  base_seq       the frame this one is a delta against; 0 = full state
u32  server_time    the server's clock at send, ms
u16  command_ack    the newest command from this peer delivered with none missing before it; 0 = none
uvarint uplink_lag  how far behind its best this peer's uplink arrives, ms (see **Uplink report**)
uvarint uplink_pps  this peer's uplink packets arriving per second; 0 = not known yet
uvarint update_rate the snapshots/sec this peer is granted, rounded (see **Sending**); 0 = every tick, at a rate the server doesn't know
...  channel part   (see **Channel**) its ack is the newest uplink packet received
u16  spawn_ct       then spawn_ct  × spawn entry
u16  despawn_ct     then despawn_ct × u32 net_id
u16  leave_ct       then leave_ct   × u32 net_id
u16  changed_ct     then changed_ct × entity entry
```

Only the first 11 bytes sit at fixed offsets; each count is followed directly by its entries. The
fixed part (header with the uplink report at 3 bytes a field, an empty channel part and the four
counts) is `SNAPSHOT_HEADER_BYTES` = 30.

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
cumulative records of everything received — only the newest one matters, so every uplink packet
repeats it and a lost one is covered by the next.

## Channel

GoldSrc's netchan: both packets, the snapshot and the uplink packet, carry a **reliable block** and a
**datagram** of the host's messages, opaque byte strings (in Godot, one RPC each).

```
u16  ack            the newest packet received from the other end (its seq), committed or not
u8   flags          bit 0: a reliable block follows
                    bit 1: that block's alternating bit
                    bit 2: a fragment follows
                    bit 3: next_fragment follows
                    bit 4: the bit of the last block this end took from the other
u16  next_fragment  only with bit 3: the fragment this end needs next
u16  block_len      only with bit 0, then block_len bytes
u16  fragment       only with bit 2: its number, then u16 remaining (how many more of its message
                    follow it), u16 len and len bytes
u16  datagram_len   then datagram_len bytes
```

A block, a message's fragments joined, and a datagram are each a run of messages: `uvarint len`,
then `len` bytes.

**Reliable.** One block is in flight at a time. A new one starts only once the other end's ack
bit equals the in-flight block's bit, and flips the bit; messages queued meanwhile wait. Each block
holds as many whole messages as fit in `RELIABLE_BLOCK_BYTES` = 800. A block goes again when an ack
names a packet newer than the one that last carried it and the bit hasn't flipped: the other end
received something later and not the block. There is no timer: a block that arrived is never sent
twice.

**Fragments.** A message too big for a block is cut into fragments of that size, numbered on from
the last message's (a `u16` that wraps), and takes the block's turn: nothing else reliable goes
until the other end has all of it, and it waits for the block before it. GoldSrc sent one fragment
per round trip; here up to `RELIABLE_FRAGMENT_WINDOW` = 32 are in flight, one per packet, so the
link's rate sets the pace. The receiver joins them in order, holding any past a missing one, and
says which it needs next in every packet it sends for `FRAGMENT_ACK_MS` = 1000 after the last one
arrived. The sender drops the fragments before that number, and sends the one at it again when an
ack names a packet newer than the one that carried it: the block's rule, applied to the first
fragment missing.
Pending data past `RELIABLE_OVERFLOW_BYTES` = 64 KB, not counting messages big enough to fragment,
is an overflow, and the sender drops the other end, as GoldSrc did ("reliable channel overflowed").

**Datagram.** The messages queued since the last packet, all of them or none: one that doesn't fit
the packet is dropped. It sits after the reliable block and the snapshot's spawn, despawn and leave
sections in the packet budget, ahead of the entities.

**Receiving.** A packet's block and datagram count only if the packet is newer than every one
received before it; a block whose bit equals the last one taken is a repeat. The snapshot delivers
the block's messages before its spawns and the datagram's after its entity deltas and before its
despawns, so an event can name an entity the same snapshot creates or removes. It delivers them
even when the frame itself can't be baselined. The uplink delivers the block's before its commands
and the datagram's after them, as GoldSrc ran a string command before a move.

## Uplink

Client to server: the snapshot ack, the channel, and the client's **commands** — opaque byte strings, each
tagged with a host-chosen `u32` stream id (in Godot, the marked RPC it stands for). The contract
is a reliable channel's: each command reaches the server's host once, in the order it was made,
across all of a client's streams. It is kept over the unreliable link the Quake way, by repetition.

```
u8   version        SNAPSHOT_VERSION
u16  seq            this packet's, counting up from 1 and skipping 0 on the wrap
u16  snapshot_ack   the newest committed snapshot; 0 = none
u16  send_time      the client's clock at send, ms, low 16 bits
uvarint rate        the rate the client asks for, in units of 100 bytes/sec, rounded up; 0 = no limit
uvarint update_rate the snapshots/sec it asks for (GoldSrc cl_updaterate); 0 = as many as the server sends
...  channel part   (see **Channel**) its ack is the newest snapshot received, committed or not
u16  floor          the oldest command the client still holds; its next seq if it holds none
u8   stream_ct      then stream_ct × u32 stream_id
u8   cmd_ct         then, if cmd_ct > 0, u16 first_seq and cmd_ct × command, seqs ascending
```

**Command** — seq, stream and bytes:

```
uvarint  seq − previous seq − 1      absent for the first, whose seq is first_seq
u8       stream index                only when stream_ct > 1
uvarint  len
ceil(len / 8) bytes                  bit i set: byte i differs from the base
the differing bytes, in order
```

The base is the previous command of the same stream in this packet, or zeros for the first
(Quake's `nullcmd`); bytes past the base's end compare against zero.

Command seqs are per client and count up from 1, skipping 0 on the wrap like snapshot seqs, and
compare the same way.

**Sending.** The client keeps every unacked command, up to `COMMAND_RING` = 64. A packet carries
every command not sent yet, the `COMMAND_BACKUP_DEFAULT` = 2 before them, and any unacked command
last sent more than `max(1.5 × rtt, 50) + 16` ms ago or proven lost: the snapshot header's channel
ack names an uplink packet newer than the one that last carried it (the netchan's rule for a block;
one held behind a lost command arrives twice, which the server ignores). The round trip is timed from an
uplink packet's send to the first snapshot whose channel ack names it. A packet goes only once, so
every ack times cleanly, however often its commands went again, and a round trip longer than the
ring is still measured. A
packet goes when one of those is due and the command interval (GoldSrc `cl_cmdrate`) is up. A
command that would overflow the ring drops the oldest, which the client then gives up on. A
packet is kept under `SAFE_PACKET_BYTES`, but its first command always goes.

**Receiving.** The server delivers commands strictly in seq order, holding any that arrive past a
gap until the gap fills. A truncated packet is dropped whole, ack included. The first packet from
a peer starts it at `floor`; a later `floor` newer than the next command due means the client gave
up on everything before it, and the server skips ahead. Its `command_ack` is the seq before the
next command due, sent in the peer's next snapshot.

## Uplink report

The server meters each peer's uplink from its packets' `send_time`s: it unwraps the 16 bits against
the newest seen, and arrival time minus send time is the two clocks' offset plus the trip. Its floor
over the last 4 to 8 s is the uncongested trip, and `uplink_lag` is how far the newest packet sits
above it: time spent queued. A gap of over 500 ms in the stream starts the floor over. `uplink_pps`
is the packets that arrived over the last 500 ms window. A queueing link delivers at its capacity,
so the client's command rate can cut to just under it.

The server applies each peer's `rate`, at least 1000 B/s, and `update_rate`, from its newest packet.

## Sequence numbers

`seq` is per peer and counts up from 1. 0 is reserved for "no baseline", so the counter skips it
when it wraps. A seq is used only by a snapshot that is sent (a choked or empty one takes none), so
a gap in the seqs a client receives is a snapshot lost on the way. Two seqs compare on the circle — `a` is newer than `b` when `(int16)(a − b) > 0` —
which holds while they are within 32767 frames of each other.

Both ends keep the last `RING` = 32 frames, in slot `seq & 31`, and check the stored seq before
trusting a slot.

## Sending

Per tick, for each peer:

1. **Cadence.** Skip the peer if its granted update rate's interval hasn't elapsed since its last
   send, or if its `rate` hasn't cleared what was already sent to it (a *choke*). A peer's first
   snapshot is never skipped. The grant is the `update_rate` it asks for (all of them, if 0), held
   under the server's snapshot rate and under `rate × 0.3 / (idle snapshot + per-packet overhead)`, so
   idle headers take at most 30% of the peer's rate, and at least `MIN_UPDATE_RATE` = 10. With the
   snapshot rate known, it is then rounded down to every nth tick, and a peer is due half a tick
   early so a jittery clock can't make it a tick late.
2. **Baseline.** `base_seq` is the peer's newest ack if that frame is still in the ring, else 0
   and the snapshot carries full state.
3. **Reliable-until-acked sections.** A spawn or despawn goes in the first snapshot that owes it
   and the `RELIABLE_BACKUP` = 2 after it, then again, with the same backups, whenever its last
   copy is older than `max(1.5 × rtt, 50) + 16` ms, where rtt is the peer's smoothed time from a
   snapshot's send to its first ack. It stops once the peer acks a snapshot that carried it: an
   ack names only the newest frame committed, so an ack of a later frame proves nothing about an
   earlier one. A spawn goes only to peers the entity is visible to (all of them, when it has no
   visibility to ask about), and a despawn only to peers that were sent its spawn. Leaves are
   derived from the baseline instead, so they repeat every snapshot until acked (see **Ack**).
4. **Entities.** An entity is a candidate when it is visible to the peer and has any slot that
   changed since the baseline. Each changed slot is written with its current value. An entity
   named by a spawn not yet proven delivered to the peer is written whole and left out of the
   frame's baseline: a client that missed the spawn discards its values and acks anyway.
5. **Budget.** The whole snapshot aims for `SAFE_PACKET_BYTES` = 1200. Header and the three
   sections come first; entities get what is left. Candidates go in order of how long they
   have waited for this peer times their priority weight. Whatever doesn't fit is left out of
   this frame, so the baseline compare offers it again next time. The first candidate always
   goes in, so a crowded packet can't stall replication.
6. **Empty skip.** A snapshot with no spawn, despawn, leave or entity, no channel content, no news
   from the uplink (commands held behind a lost one, or a packet of nothing but repeats, which
   means the client lacks the ack), and no `command_ack` the
   peer hasn't been sent, isn't sent, unless the peer has gone `CLOCK_KEEPALIVE_MS` = 250 ms
   without one — `server_time` is the client's clock feed and must not go quiet.

The server stores what each frame holds for the peer under its `seq`, so the next delta can be
built against whichever frame the peer acks.

## Receiving

For each snapshot:

1. **Version.** Not `SNAPSHOT_VERSION` → drop it (and warn once per session).
2. **Order.** `seq` not newer than the last committed frame → drop it.
3. **Clock.** Surface `server_time` and `command_ack` — even if the next step drops the snapshot.
4. **Baseline.** If `base_seq` ≠ 0 and that frame isn't in the ring, re-ack the last committed
   frame and drop the snapshot. The server keeps diffing against frames the client holds,
   falling back to full state when it has to.
5. **Apply.** Spawns are applied in order, as read, skipping any net_id the client already created:
   an unacked spawn arrives again as backups and on every overdue resend. A spawn the client can't create yet isn't
   counted as created, so a later resend gets another try. The new frame starts as a copy of the
   baseline (empty for `base_seq` 0); each entity entry overwrites its changed slots and is applied
   to the entity. Despawns are applied after the entities, only for net_ids the client created
   (they repeat the same way), then entities that no longer exist locally are dropped from the
   frame, then leaves are dropped from it and reported.
6. **Truncated.** If the packet ran short, keep whatever it applied but neither commit nor ack.
7. **Warmup.** If an entity the client knows of couldn't take its values yet (not ready), re-ack
   the last committed frame instead of this one, for up to 32 frames in a row (and only once some
   frame has been committed; the very first is always accepted). Acking would tell the server the
   entity's state was delivered, and an idle entity would then never be resent. A spawn the client
   couldn't create holds the frame the same way, and also on the very first frame, which it leaves
   unacked: acking a frame carrying a spawn retires it, so it would come only when overdue.
8. **Commit.** Store the frame under `seq`, make it the last committed frame, and ack `seq`.

The server, on an ack newer than the peer's previous one, makes that frame the peer's baseline and
times its round trip. On any ack, it retires every spawn and despawn that frame carried.
