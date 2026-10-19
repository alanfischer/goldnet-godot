#pragma once
//
// The snapshot protocol's shared constants: what both ends of the wire must agree on.
//
#include <cstdint>

namespace goldnet {

// Snapshot wire-protocol version, written as the first byte of every snapshot. Bump on any
// wire-format change so a client talking to a mismatched build fails loudly (one clear warning +
// dropped snapshot) instead of misparsing every packet into a flood of decode errors. A single
// byte is plenty — this only has to distinguish "same build" from "not", not identify goldnet's
// framing (the packet already arrives on goldnet's own channel, which is identification enough).
// v3: changed-field mask is a uvarint (was a fixed u32); magic shrank from a 4-byte "GNS"+version
// tag to this 1-byte version; empty snapshots (nothing changed/spawned/despawned/left) are no
// longer sent at all.
// v4: two new self-describing value tags, TAG_ANGLE8 and TAG_TIME_DELTA — an old decoder seeing
// either would fall through to the opaque-value default and misparse the stream, so this must bump
// even though nothing about the existing tags changed.
// v5: the header carries the client's command ack (see uplink.h), and the client's snapshot ack
// rides its uplink packet instead of travelling alone.
// v6: GoldSrc's netchan (netchan.h): both packets carry a reliable block and a datagram after the
// header, and the uplink packet gained a seq of its own and the newest snapshot received.
// v7: the uplink packet carries its send time and the client's rate; the snapshot header carries the
// server's measure of the client's uplink (auto_rate.h).
// v8: the uplink packet carries the client's update rate; the snapshot header carries the rate the
// server grants it.
static const uint8_t SNAPSHOT_VERSION = 8;

// Frames of history kept per peer (server) and received (client). A power of two: slots are
// indexed by seq & (RING - 1). ~1s at 30 Hz — covers ack RTT + loss.
static const int RING = 32;

// Max replicated slots per entity — the width of the u32 changed-field bitmask.
static const int MAX_SLOTS = 32;

// Max PVS-leave markers per snapshot (4 bytes each on the wire). A fresh peer's relevance seed can
// queue hundreds of leaves (every out-of-PVS entity on a big map); emitting them all in one frame
// overruns the MTU, so it's dropped and never acked — and the same oversized frame resends forever.
// Capping keeps each frame under MTU; leaves are reliable-until-acked, so they drain over a few ticks.
static const uint16_t MAX_LEAVES_PER_SNAPSHOT = 32;

// Conservative safe-UDP-payload ceiling for a WHOLE snapshot, the same MTU hazard as the leave cap
// above but across the packet rather than one section of it: a peer's first full baseline (every
// entity at once) or a burst of simultaneous changes (an explosion, a big fight) can overrun
// the MTU, fragment, and — since a dropped fragment costs the whole packet, and an unacked packet
// is resent — thrash. The entity-delta body gets whatever is left of this after the header and the
// spawn/despawn/leave sections, all of which are built BEFORE it and measured rather than
// estimated. An entity that doesn't fit isn't lost: it's simply left out of the peer's baseline so the
// acked-baseline compare still sees it as changed, and its overflow priority only goes up — it
// outranks everything that fit this tick on the next one, the same self-heal the leave cap relies on.
static const int SAFE_PACKET_BYTES = 1200;

// Fixed part of every snapshot: version u8, seq u16, base_seq u16, server_time u32, command_ack u16,
// the uplink report's two uvarints and the granted update rate's at their widest in practice, the channel part's ack u16, flags u8 and datagram length u16, and the four section counts
// (spawn/despawn/leave/changed) at u16 each. Subtracted from SAFE_PACKET_BYTES when sizing the
// entity-delta budget, so the budget describes the packet rather than one section.
static const int SNAPSHOT_HEADER_BYTES = 1 + 2 + 2 + 4 + 2 + 3 + 3 + 2 + 2 + 1 + 2 + 2 + 2 + 2 + 2;

// The same fixed part at its narrowest: a snapshot with nothing in it.
static const int IDLE_SNAPSHOT_BYTES = 1 + 2 + 2 + 4 + 2 + 1 + 1 + 1 + 2 + 1 + 2 + 2 + 2 + 2 + 2;

// The least update rate the server grants (GoldSrc sv_minupdaterate), snapshots/sec.
static const uint32_t MIN_UPDATE_RATE = 10;

// The most of a peer's rate that idle snapshots alone may take. A grant over it would leave a slow
// link choking on headers, served unevenly instead of at a lower, steady rate.
static const double UPDATE_RATE_RATE_SHARE = 0.3;

// Largest reliable block (netchan.h), the GoldSrc fragment size scaled to SAFE_PACKET_BYTES: a block
// leaves a third of the packet for the datagram and the entities, and a bigger message is cut into
// fragments of this size.
static const int RELIABLE_BLOCK_BYTES = 800;

// Fragments of one message in flight at once (netchan.h). GoldSrc sent one per round trip, which a
// message the size of a resource list turns into a long join; a window lets the link's rate set the
// pace instead, while each fragment is still resent only on proof of its loss.
static const int RELIABLE_FRAGMENT_WINDOW = 32;

// How long after its last fragment arrived an end keeps telling the other which fragment it needs
// next. Past that it says nothing, and the two bytes stop going in every packet.
static const uint32_t FRAGMENT_ACK_MS = 1000;

// Pending reliable data past this, not counting messages big enough to fragment, is an overflow:
// the other end isn't taking it. GoldSrc dropped the client ("reliable channel overflowed").
static const int RELIABLE_OVERFLOW_BYTES = 64 * 1024;

// Longest a peer may go with no snapshot at all. Every snapshot header carries the server's
// send-time, which the client surfaces as its server clock feed — an always-on feed independent
// of entity traffic. Skipping empty snapshots would silence that feed exactly when the world is
// idle, leaving the client's clock free-running on local time, so an otherwise-empty snapshot
// still goes out this often to keep it fed. Cheap: a header-only packet at 4 Hz is ~68 B/s,
// against the ~450 B/s the skip saves at a 30 Hz tick.
static const uint32_t CLOCK_KEEPALIVE_MS = 250;

// Unacked commands a client holds for resending (see uplink.h). One that would make it COMMAND_RING
// + 1 drops the oldest, which the server then skips. ~1s at 60 Hz, which covers a WiFi dropout.
static const int COMMAND_RING = 64;

// Older commands riding with each new one (GoldSrc cl_cmdbackup): a lost packet or two is covered
// before anyone could know it was lost.
static const int COMMAND_BACKUP_DEFAULT = 2;

// Snapshots right after a spawn's or despawn's first that carry it again, the same cover for the
// downlink's reliable sections. Past those it goes again only when overdue (see codec.h).
static const int RELIABLE_BACKUP = 2;

// Compact, self-delimiting wire encoding for per-slot values: a 1-byte tag, then the payload.
// The DECODER needs no hint (it reads the tag and knows the width, so a value for an unknown
// entity can still be consumed); only the SENDER chooses a lossy tag, from the slot's hint.
enum ValueTag : uint8_t {
	TAG_OPAQUE = 0,      // u32 length + bytes in the host engine's own encoding
	TAG_FLOAT = 1,       // f32
	TAG_INT = 2,         // zig-zag varint
	TAG_VEC3 = 3,        // 3x f32
	TAG_BOOL = 4,        // u8
	TAG_ANGLE16 = 5,     // float radians -> u16 over [0, TAU)
	TAG_HALF = 6,        // float -> IEEE binary16
	TAG_VEC3_HALF = 7,   // vec3 -> 3x binary16
	TAG_ANGLE8 = 8,      // float radians -> u8 over [0, TAU) — GoldSrc-classic angle precision
	TAG_TIME_DELTA = 9,  // int ms timestamp -> zig-zag varint of (value - this packet's header time)
};

// Per-slot quantization hint meaning "none — pick the tag from the value's runtime type".
static const uint8_t QUANT_AUTO = 255;

} // namespace goldnet
