#pragma once
//
// Engine-agnostic wire helpers — the parts of the GoldSrc snapshot protocol that are
// pure arithmetic over a byte buffer, with no dependency on Variant, String, or any
// other engine-backed type.
//
// They live in their own header so they can be exercised by the standalone test suite
// in ../tests (see tests/CMakeLists.txt). godot-cpp's engine classes route through
// GDExtension function pointers that only exist inside a running Godot process, so
// anything touching Variant/StreamPeerBuffer directly cannot be unit tested out of
// process. These helpers can — and they're where the subtle bugs live: zig-zag varint
// widths, angle wraparound, and uint16 sequence rollover.
//
// Templating notes:
//   * Buffer access is through `buf->`, so production passes a Ref<StreamPeerBuffer>
//     and tests pass a pointer to a fake with the same put_*/get_* surface. The
//     production call sites are unchanged from when these were file-static functions.
//   * The ack-tracking helpers are templated on the map type so tests can supply a
//     minimal stand-in for godot-cpp's HashMap (which allocates through the engine).
//     The map must expose has()/erase()/operator[] and iterate as {.key, .value}.
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
// Shared by the spawn and despawn sections. `wait[net_id]` records the first seq of the
// current unacked run, so a record keeps being resent until an ack covers the seq it
// was *first* sent on — not merely the latest one, which would let a record fall out of
// the resend set while its only transmission was still in flight.

// Returns whether to (re)send this record to the peer, and stops tracking once acked.
template <typename M>
bool reliable_include(M &p_wait, uint32_t p_net_id, uint16_t p_seq, uint16_t p_last_acked,
		bool p_has_ack) {
	uint16_t fs;
	if (p_wait.has(p_net_id)) {
		fs = p_wait[p_net_id];
	} else {
		fs = p_seq;
		p_wait[p_net_id] = p_seq;
	}
	if (p_has_ack && seq_le(fs, p_last_acked)) {
		p_wait.erase(p_net_id);
		return false; // delivered — stop resending
	}
	return true;
}

// Picks this snapshot's PVS-leave markers out of the wait map, capped so one frame can't
// overrun the MTU, and retires the ones an ack confirms delivered. wait[id] == 0 means
// "owed, not sent yet" (seq 0 is reserved), so a leave held back by the cap can't be retired
// by an ack for a frame it was never in.
//
// Same first-seq rule as reliable_include(): the stamp is the seq of the FIRST send and stays
// put across resends, so an ack can catch up to it. Re-stamping on every resend moves the
// target the ack is chasing — nothing retires, the map grows without bound, and past the cap
// those undead entries starve every new leave.
template <typename M, typename V>
void emit_leaves(M &p_wait, uint16_t p_seq, uint16_t p_last_acked, bool p_has_ack,
		int p_cap, V &r_send) {
	V retired;
	for (auto &kv : p_wait) {
		if (kv.value != 0 && p_has_ack && seq_le(kv.value, p_last_acked)) {
			retired.push_back(kv.key); // the frame that carried it was acked → delivered
		} else if ((int)r_send.size() < p_cap) {
			r_send.push_back(kv.key);
			if (kv.value == 0) {
				kv.value = p_seq; // first send — the seq an ack has to cover to retire it
			}
		}
	}
	for (int i = 0; i < (int)retired.size(); i++) {
		p_wait.erase(retired[i]);
	}
}

// Drops the records an ack confirms delivered, reporting them through r_retired.
template <typename M, typename V>
void retire_acked(M &p_wait, uint16_t p_last_acked, V &r_retired) {
	for (const auto &kv : p_wait) {
		if (seq_le(kv.value, p_last_acked)) {
			r_retired.push_back(kv.key);
		}
	}
	for (int i = 0; i < (int)r_retired.size(); i++) {
		p_wait.erase(r_retired[i]);
	}
}

} // namespace goldnet
