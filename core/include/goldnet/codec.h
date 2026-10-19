#pragma once
//
// Wire helpers — the parts of the GoldSrc snapshot protocol that are pure arithmetic: varints,
// angle quantization, uint16 sequence comparison, and the ack bookkeeping. They're where the
// subtle bugs live (zig-zag widths, angle wraparound, sequence rollover), so the standalone test
// suite in ../../../tests pins them directly.
//
// Templating notes:
//   * Buffer access is through `buf->`, so the protocol passes a Writer* / Reader* and tests may
//     pass a pointer to any fake with the same put_*/get_* surface.
//   * The bookkeeping helpers are templated on their container types, which need the std
//     interface: count()/erase()/operator[], with maps iterating as {.first, .second}. Tests use
//     insertion-ordered fakes so emission order is assertable.
//
#include <cmath>
#include <cstdint>

namespace goldnet {

static const float TAU = 6.28318530717958647692f;

// --- zig-zag varint ---
//
// Zig-zag maps small-magnitude signed values (including negatives) onto small
// unsigned ones, so the common case costs a single byte. Values are then emitted
// 7 bits at a time, low group first, with the high bit marking "another byte
// follows". A full-width negative costs 10 bytes; that's the intended trade.

template <typename B>
void put_varint(const B &buf, int64_t p_v) {
	uint64_t u = ((uint64_t)p_v << 1) ^ (uint64_t)(p_v >> 63); // zig-zag: small magnitudes → few bytes
	while (u >= 0x80) {
		buf->put_u8((uint8_t)u | 0x80);
		u >>= 7;
	}
	buf->put_u8((uint8_t)u);
}

// The shift guard is not hygiene: a snapshot is unreliable UDP whose bytes are attacker- or
// corruption-reachable, and a run of continuation bytes (0x80..0xFF) longer than the encoding can
// produce would shift past the width of `u` — undefined behaviour, which the test suite's
// -fno-sanitize-recover UBSan turns into an abort. Stop consuming payload bits once the type is
// full; the loop still drains the continuation run so the stream stays framed for whatever follows.
static const int VARINT_MAX_SHIFT_64 = 63;
static const int VARINT_MAX_SHIFT_32 = 28;

template <typename B>
int64_t get_varint(const B &buf) {
	uint64_t u = 0;
	int shift = 0;
	uint8_t b;
	do {
		b = buf->get_u8();
		if (shift <= VARINT_MAX_SHIFT_64) {
			u |= (uint64_t)(b & 0x7F) << shift;
			shift += 7;
		}
	} while (b & 0x80);
	return (int64_t)(u >> 1) ^ -(int64_t)(u & 1); // un-zig-zag
}

// --- unsigned varint (no zig-zag) ---
//
// For values that are never negative — bitmasks, counts — zig-zag's doubling only
// costs range for nothing in return, so this is the same 7-bits-at-a-time encoding
// without it.

template <typename B>
void put_uvarint(const B &buf, uint32_t p_v) {
	uint32_t u = p_v;
	while (u >= 0x80) {
		buf->put_u8((uint8_t)u | 0x80);
		u >>= 7;
	}
	buf->put_u8((uint8_t)u);
}

// Same shift guard as get_varint, and it matters more here: at 32 bits a malformed stream runs out
// of width after five bytes rather than ten, so it is that much easier to reach.
template <typename B>
uint32_t get_uvarint(const B &buf) {
	uint32_t u = 0;
	int shift = 0;
	uint8_t b;
	do {
		b = buf->get_u8();
		if (shift <= VARINT_MAX_SHIFT_32) {
			u |= (uint32_t)(b & 0x7F) << shift;
			shift += 7;
		}
	} while (b & 0x80);
	return u;
}

// --- angle quantization ---
//
// A full turn folded onto a u16: ~0.0055° steps, 2 bytes instead of 4. The fold is
// what makes this safe for yaw/pitch that accumulate without bound — TAU and 0 both
// land on 0, and negatives wrap up into range rather than truncating toward zero.

template <typename B>
void put_angle16(const B &buf, float radians) {
	float t = fmodf(radians, TAU);
	// NaN/inf guard: fmodf(NaN or ±inf, TAU) == NaN, and casting NaN — or the boundary value that
	// rounds to exactly 65536.0 — straight to uint16 is undefined behaviour (a debug build traps it,
	// crashing the server). Sanitize to 0 and go through uint32+mask so the wrap is well-defined.
	if (std::isnan(t)) {
		t = 0.0f;
	} else if (t < 0.0f) {
		t += TAU;
	}
	// [0,TAU) → [0,65536); cast through uint32 (always in range) then mask to 16 bits so the boundary
	// 65536 wraps to 0 without the out-of-range float→uint16 cast the comment used to rely on.
	buf->put_u16((uint16_t)((uint32_t)((t / TAU) * 65536.0f) & 0xFFFFu));
}

template <typename B>
float get_angle16(const B &buf) {
	return ((float)buf->get_u16() / 65536.0f) * TAU;
}

// A full turn folded onto a u8: ~1.4° steps (256 positions) — matching classic GoldSrc/Quake angle
// precision, half the cost of angle16. Same fold/NaN/wrap handling, at 8 bits instead of 16: safe
// for a remote avatar's rendered orientation, where sub-degree precision rarely matters.

template <typename B>
void put_angle8(const B &buf, float radians) {
	float t = fmodf(radians, TAU);
	if (std::isnan(t)) {
		t = 0.0f;
	} else if (t < 0.0f) {
		t += TAU;
	}
	// [0,TAU) → [0,256); cast through uint32 then mask to 8 bits so the 256 boundary wraps to 0,
	// same reasoning as angle16's 65536 boundary above.
	buf->put_u8((uint8_t)((uint32_t)((t / TAU) * 256.0f) & 0xFFu));
}

template <typename B>
float get_angle8(const B &buf) {
	return ((float)buf->get_u8() / 256.0f) * TAU;
}

// --- sequence comparison ---
//
// Frame sequences are uint16 and roll over roughly every 36 minutes at 30 Hz, so they
// must never be compared with a plain `>`. The subtraction-and-cast trick compares
// them on the circle: `a` is newer than `b` when the signed distance from b to a is
// positive, which stays correct across the rollover boundary as long as the two are
// within half the sequence space (32767 frames ≈ 18 min) of each other. Seq 0 is
// reserved by the protocol for "no baseline / full state".

inline bool seq_newer(uint16_t a, uint16_t b) {
	return (int16_t)(a - b) > 0;
}

inline bool seq_le(uint16_t a, uint16_t b) {
	return !seq_newer(a, b);
}

// --- deterministic PRNG for network-condition simulation ---
//
// The sim defaults to the engine RNG, which is fine for "does it self-heal?" poking but
// useless for a regression test: the same run drops different packets and draws different
// latencies every time, so a failure can't be reproduced or bisected. One seed feeds every
// random draw in the sim — the loss roll and the latency draw — so a seeded session
// replays whole rather than half-deterministically.
//
// xorshift32 — not cryptographic and not meant to be. What matters is that it's
// identical on every platform and compiler, which rules out the standard library's
// generators (std::mt19937's distributions are implementation-defined).

// Advances `state` and returns the next value. `state` must never be 0 — xorshift
// latches there permanently — so seed through sim_rng_seed().
inline uint32_t xorshift32(uint32_t &state) {
	uint32_t x = state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	state = x;
	return x;
}

// Maps any user-supplied seed onto a legal (non-zero) xorshift state.
inline uint32_t sim_rng_seed(uint32_t p_seed) {
	return p_seed == 0 ? 0x9E3779B9u : p_seed; // golden-ratio constant, an arbitrary non-zero
}

// --- reliable-until-acked bookkeeping ---
//
// Shared by the spawn and despawn sections. A record owed to a peer goes in the first snapshot
// and the RELIABLE_BACKUP after it, so a lost snapshot or two is covered before anyone could know,
// and after that only once overdue (GoldSrc's reliable channel resends on loss, not every packet).
// Delivery is proven only by an ack naming a snapshot that carried it: an ack names the newest
// frame the client committed, so an ack of a later frame says nothing about one lost on the way.

// Sent longer ago than this and still unacked: presumably lost. A round trip and a half, since an
// ack waits for the other end's next packet, plus a 60 Hz tick.
inline uint32_t overdue_ms(uint32_t p_srtt_ms) {
	return (p_srtt_ms * 3 / 2 > 50 ? p_srtt_ms * 3 / 2 : 50) + 16;
}

struct Owed {
	static const int SEQS = 16; // the most recent carriers kept; older acks are long overdue anyway
	uint16_t seqs[SEQS] = {};
	uint32_t carries = 0;
	int backups = 0; // copies still due in the snapshots right after this one
	uint32_t last_ms = 0;

	bool carried_in(uint16_t p_seq) const {
		const uint32_t n = carries < (uint32_t)SEQS ? carries : (uint32_t)SEQS;
		for (uint32_t i = 0; i < n; i++) {
			if (seqs[i] == p_seq) {
				return true;
			}
		}
		return false;
	}
};

// Whether this snapshot (seq p_seq, which goes out) carries the record, starting to track it on
// its first call. Proof of delivery is retire_proven's job, never this one's.
template <typename M>
bool owe_include(M &p_owed, uint32_t p_net_id, uint16_t p_seq, uint32_t p_now, uint32_t p_overdue_ms,
		int p_backup) {
	auto it = p_owed.find(p_net_id);
	if (it == p_owed.end()) {
		it = p_owed.emplace(p_net_id, Owed()).first;
		it->second.backups = p_backup;
	} else if (it->second.backups > 0) {
		it->second.backups--;
	} else if (p_now - it->second.last_ms >= p_overdue_ms) {
		it->second.backups = p_backup;
	} else {
		return false;
	}
	Owed &o = it->second;
	o.seqs[o.carries % Owed::SEQS] = p_seq;
	o.carries++;
	o.last_ms = p_now;
	return true;
}

// Drops the records an ack of p_seq proves delivered, reporting them through r_retired. Any ack
// proves its frame was committed, so this holds for a late ack older than the newest too.
template <typename M, typename V>
void retire_proven(M &p_owed, uint16_t p_seq, V &r_retired) {
	for (const auto &kv : p_owed) {
		if (kv.second.carried_in(p_seq)) {
			r_retired.push_back(kv.first);
		}
	}
	for (int i = 0; i < (int)r_retired.size(); i++) {
		p_owed.erase(r_retired[i]);
	}
}

// --- PVS removal derivation (GoldSrc) ---
//
// The removals a snapshot owes this peer: everything its acked baseline holds that it can no
// longer see. Derived, never queued — if the snapshot carrying them is lost the peer doesn't
// ack it, the baseline doesn't move, and the identical set is recomputed next tick. That is
// what makes leaves self-correcting without any reliable-until-acked bookkeeping.
//
// Bounded by p_cap so one frame can't overrun the MTU. Whatever doesn't fit is reported through
// r_carry: the caller must keep those ids in THIS frame's baseline, because the peer still holds
// them — we simply haven't told it otherwise yet. Dropping them from the baseline instead would
// erase the very evidence the next diff needs, and the entity would stay drawn forever.
template <typename S, typename V>
void derive_leaves(const S &p_held, const S &p_visible, int p_cap, V &r_send, V &r_carry) {
	for (const uint32_t &id : p_held) {
		if (p_visible.count(id)) {
			continue; // still in view
		}
		if ((int)r_send.size() < p_cap) {
			r_send.push_back(id);
		} else {
			r_carry.push_back(id);
		}
	}
}

} // namespace goldnet
