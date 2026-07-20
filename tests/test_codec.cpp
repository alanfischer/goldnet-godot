// Unit tests for the goldnet wire codec — zig-zag varint and angle16 quantization.
//
// Background: these two encodings carry most of the snapshot payload, so a regression
// here is a bandwidth regression (values silently getting wider) or a corruption bug
// (values decoding to something plausible-but-wrong). Neither announces itself — the
// game keeps running and the packets keep parsing. That's what these tests are for.
//
// The size assertions are as important as the round-trip ones: they're what would catch
// an "optimization" that quietly costs bytes on the common small-value path.

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdint>

#include "test_support.h"
#include "../src/goldnet_codec.h"

using namespace goldnet;


static bool approx(float a, float b, float tol) {
	return std::fabs(a - b) < tol;
}

// One quantization step of angle16. The encoder truncates rather than rounds, so
// round-trip error is in [0, STEP) — never more than one step, never negative.
static const float STEP = TAU / 65536.0f;

// --- varint ---

static void test_varint_roundtrip() {
	const int64_t values[] = {
		0, 1, -1, 2, -2, 63, 64, -64, -65, 127, 128, -128,
		8191, 8192, -8192, 16383, 16384,
		2147483647LL, -2147483648LL,
		INT64_MAX, INT64_MIN,
	};
	for (int64_t v : values) {
		FakeBuf buf;
		put_varint(&buf, v);
		buf.rewind();
		int64_t got = get_varint(&buf);
		CHECK(got == v);
		// The decoder must consume exactly what the encoder wrote — a mismatch here
		// desyncs every subsequent field in the snapshot, not just this one.
		CHECK(buf.read_pos == buf.size());
	}
	printf("  varint roundtrip: ok\n");
}

static void test_varint_widths() {
	struct Case {
		int64_t value;
		size_t bytes;
	};
	// Zig-zag maps small negatives as cheaply as small positives; that's the whole
	// point of it, and these pin the boundaries where each width tips over.
	const Case cases[] = {
		{ 0, 1 }, { 1, 1 }, { -1, 1 },
		{ 63, 1 }, { -64, 1 },   // largest values still costing one byte
		{ 64, 2 }, { -65, 2 },   // first values needing two
		{ 8191, 2 }, { 8192, 3 },
		{ INT64_MAX, 10 }, { INT64_MIN, 10 },
	};
	for (const Case &c : cases) {
		FakeBuf buf;
		put_varint(&buf, c.value);
		CHECK(buf.size() == c.bytes);
	}
	printf("  varint widths: ok\n");
}

static void test_varint_stream() {
	// Values are written back-to-back with no framing, so decoding depends entirely on
	// each varint self-terminating at the right byte.
	const int64_t seq[] = { 0, -1, 300, 64, INT64_MIN, 7, -8192 };
	FakeBuf buf;
	for (int64_t v : seq) {
		put_varint(&buf, v);
	}
	buf.rewind();
	for (int64_t v : seq) {
		CHECK(get_varint(&buf) == v);
	}
	CHECK(buf.read_pos == buf.size());
	printf("  varint stream framing: ok\n");
}

// --- angle16 ---

static void test_angle16_roundtrip() {
	const float angles[] = {
		0.0f, 0.1f, 1.0f, 1.5707963f /*PI/2*/, 3.1415926f /*PI*/,
		4.712389f /*3PI/2*/, 6.2831f /*just under TAU*/,
	};
	for (float a : angles) {
		FakeBuf buf;
		put_angle16(&buf, a);
		buf.rewind();
		CHECK(approx(get_angle16(&buf), a, STEP * 2.0f));
		CHECK(buf.size() == 2); // the entire point: 2 bytes, not 4
	}
	printf("  angle16 roundtrip: ok\n");
}

static void test_angle16_wrap() {
	// TAU and 0 are the same heading and must encode identically, or a player spinning
	// through north would pop.
	FakeBuf zero, tau;
	put_angle16(&zero, 0.0f);
	put_angle16(&tau, TAU);
	CHECK(zero.bytes == tau.bytes);

	// Negative angles fold up into range rather than truncating toward zero.
	struct Case {
		float in;
		float expect;
	};
	const Case cases[] = {
		{ -1.5707963f, TAU - 1.5707963f },  // -PI/2 → 3PI/2
		{ -3.1415926f, TAU - 3.1415926f },  // -PI   → PI
		{ -0.1f, TAU - 0.1f },
	};
	for (const Case &c : cases) {
		FakeBuf buf;
		put_angle16(&buf, c.in);
		buf.rewind();
		CHECK(approx(get_angle16(&buf), c.expect, STEP * 2.0f));
	}
	printf("  angle16 wrap: ok\n");
}

static void test_angle16_unbounded_input() {
	// Yaw/pitch accumulate without bound in the game, so the codec has to fold arbitrary
	// multiples of a turn. Tolerance is looser here because float can't hold 10*TAU+x
	// precisely — the limit is the input's representation, not the encoding.
	const float base = 0.7853981f; // PI/4
	const float multiples[] = { 1.0f, 2.0f, 10.0f, -1.0f, -10.0f };
	for (float m : multiples) {
		FakeBuf buf;
		put_angle16(&buf, base + m * TAU);
		buf.rewind();
		CHECK(approx(get_angle16(&buf), base, STEP * 8.0f));
	}
	printf("  angle16 unbounded input: ok\n");
}

static void test_angle16_nonfinite() {
	// A NaN or ±inf angle reaching the quantizer used to CRASH the server: fmodf(NaN, TAU)
	// is NaN, and casting NaN straight to uint16 is undefined behaviour that a debug build
	// traps on. Game code produces these more easily than it looks — a zero-length look
	// vector normalized, a physics blowup — and it only has to happen once.
	//
	// The contract is "sanitize to 0", not "reject": the slot still has to be written, or
	// the snapshot's framing shifts and every field after it decodes as garbage.
	const float bad[] = {
		NAN, -NAN, INFINITY, -INFINITY,
	};
	for (float a : bad) {
		FakeBuf buf;
		put_angle16(&buf, a);
		CHECK(buf.size() == 2); // still exactly 2 bytes — framing is preserved
		buf.rewind();
		float got = get_angle16(&buf);
		CHECK(got == 0.0f);
	}
	printf("  angle16 non-finite input sanitizes to 0: ok\n");
}

static void test_angle16_upper_boundary() {
	// The other half of the same crash: an angle just under TAU can scale to exactly
	// 65536.0, which does not fit in a uint16 — also UB on the direct cast. The encoder
	// goes through uint32 and masks, so the boundary wraps to 0 (the correct answer: TAU
	// and 0 are the same heading). Walk right up to TAU from below.
	float a = TAU;
	for (int i = 0; i < 64; i++) {
		a = nextafterf(a, 0.0f);
		FakeBuf buf;
		put_angle16(&buf, a);
		CHECK(buf.size() == 2);
		buf.rewind();
		float got = get_angle16(&buf);
		// Whatever it lands on must be a legal heading — never negative, never >= TAU.
		CHECK(got >= 0.0f && got < TAU);
	}
	// Exactly TAU, and a large multiple of it, are the same boundary reached another way.
	const float turns[] = { TAU, 2.0f * TAU, 1000.0f * TAU };
	for (float t : turns) {
		FakeBuf buf;
		put_angle16(&buf, t);
		buf.rewind();
		float got = get_angle16(&buf);
		CHECK(got >= 0.0f && got < TAU);
	}
	printf("  angle16 upper boundary wraps instead of overflowing: ok\n");
}

static void test_angle16_output_range() {
	// Decoded angles must always land in [0, TAU) so downstream math never sees a
	// negative heading. Sweep the whole u16 space, not a sample of it.
	for (int i = 0; i < 65536; i++) {
		FakeBuf buf;
		buf.put_u16((uint16_t)i);
		buf.rewind();
		float a = get_angle16(&buf);
		CHECK(a >= 0.0f && a < TAU);
	}
	printf("  angle16 output range (all 65536): ok\n");
}

// --- network-condition sim PRNG ---

static void test_rng_reproducible() {
	// The whole reason this generator exists: a seeded session must replay exactly,
	// or a failure it surfaces can't be reproduced or bisected.
	uint32_t a = sim_rng_seed(12345);
	uint32_t b = sim_rng_seed(12345);
	for (int i = 0; i < 1000; i++) {
		CHECK(xorshift32(a) == xorshift32(b));
	}
	printf("  rng reproducible from same seed: ok\n");
}

static void test_rng_seeds_differ() {
	uint32_t a = sim_rng_seed(1);
	uint32_t b = sim_rng_seed(2);
	int same = 0;
	for (int i = 0; i < 1000; i++) {
		if (xorshift32(a) == xorshift32(b)) {
			same++;
		}
	}
	CHECK(same == 0); // distinct seeds must not walk the same path
	printf("  rng distinct seeds diverge: ok\n");
}

static void test_rng_never_latches() {
	// xorshift32 is absorbing at 0 — a zero state would make the sim silently
	// stop varying. sim_rng_seed must map 0 onto something legal, and the state must
	// never reach 0 on its own.
	CHECK(sim_rng_seed(0) != 0);
	uint32_t s = sim_rng_seed(0);
	for (int i = 0; i < 100000; i++) {
		xorshift32(s);
		CHECK(s != 0);
	}
	printf("  rng never latches at zero: ok\n");
}

static void test_rng_distribution() {
	// A rough uniformity check on the %100 mapping used by the loss roll. Not a
	// statistical test — just enough to catch a generator that's stuck in a narrow band
	// or skewed enough to make "20% loss" mean something else entirely.
	uint32_t s = sim_rng_seed(999);
	int drops = 0;
	const int trials = 100000;
	for (int i = 0; i < trials; i++) {
		if ((int)(xorshift32(s) % 100) < 20) {
			drops++;
		}
	}
	CHECK(drops > trials * 18 / 100 && drops < trials * 22 / 100);
	printf("  rng distribution at 20%%: %d/%d drops: ok\n", drops, trials);
}

int main() {
	printf("test_codec\n");
	test_varint_roundtrip();
	test_varint_widths();
	test_varint_stream();
	test_angle16_roundtrip();
	test_angle16_wrap();
	test_angle16_unbounded_input();
	test_angle16_nonfinite();
	test_angle16_upper_boundary();
	test_angle16_output_range();
	test_rng_reproducible();
	test_rng_seeds_differ();
	test_rng_never_latches();
	test_rng_distribution();
	printf("test_codec: %d checks passed\n", checks);
	return 0;
}
