// Unit tests for uint16 sequence comparison and the reliable-until-acked bookkeeping.
//
// Background: frame sequences are uint16 and roll over about every 36 minutes at 30 Hz.
// Every comparison against them therefore has to be modular, and a bug in that logic
// has a nasty signature — the session runs perfectly for half an hour, then snapshots
// start getting rejected or spawn records start resending forever. That's expensive to
// catch by playing and trivial to catch here, which is the entire argument for this file.
//
// The rollover cases below are the ones a naive `a > b` implementation fails.

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <vector>

#include "test_support.h"
#include "../src/goldnet_codec.h"

using namespace goldnet;


// --- sequence comparison ---

static void test_seq_basic() {
	CHECK(seq_newer(5, 3));
	CHECK(!seq_newer(3, 5));
	CHECK(!seq_newer(4, 4)); // equal is not newer
	CHECK(seq_le(4, 4));
	CHECK(seq_le(3, 5));
	CHECK(!seq_le(5, 3));
	printf("  seq basic ordering: ok\n");
}

static void test_seq_rollover() {
	// The cases that matter: sequences that have wrapped past 65535 are still newer
	// than the ones just before the wrap.
	CHECK(seq_newer(0, 65535));
	CHECK(seq_newer(1, 65535));
	CHECK(seq_newer(1, 65534));
	CHECK(seq_newer(3, 65530));
	CHECK(!seq_newer(65535, 0));
	CHECK(!seq_newer(65530, 3));

	// seq_le must agree across the wrap — this is the comparison the spawn/despawn
	// ack path uses, so getting it backwards resends records forever.
	CHECK(seq_le(65530, 3));
	CHECK(!seq_le(3, 65530));
	printf("  seq rollover: ok\n");
}

static void test_seq_half_space_boundary() {
	// Modular comparison is only meaningful while the two sequences are within half the
	// space of each other (32767 frames ≈ 18 min at 30 Hz). At exactly half, the
	// direction is genuinely ambiguous and resolves to "not newer". Pinned so a change
	// to the comparison has to acknowledge this edge rather than silently move it.
	CHECK(seq_newer(32767, 0));
	CHECK(!seq_newer(32768, 0)); // ambiguous midpoint → not newer
	CHECK(!seq_newer(0, 32767));
	CHECK(seq_newer(0, 32769)); // past the midpoint, reads as wrapped-around
	printf("  seq half-space boundary: ok\n");
}

// --- reliable_include ---

static void test_reliable_include_first_send() {
	FakeMap wait;
	// No ack yet: the record is sent and starts being tracked.
	CHECK(reliable_include(wait, 42, 10, 0, false));
	CHECK(wait.has(42));
	CHECK(wait[42] == 10);
	printf("  reliable_include first send: ok\n");
}

static void test_reliable_include_retains_first_seq() {
	// The key invariant: the tracked seq is the FIRST send of the unacked run, not the
	// latest. If it were updated to the current seq on every resend, an ack could never
	// catch up to it and the record would resend forever.
	FakeMap wait;
	reliable_include(wait, 42, 10, 0, false);
	for (uint16_t seq = 11; seq <= 15; seq++) {
		CHECK(reliable_include(wait, 42, seq, 0, false)); // still unacked → keep resending
		CHECK(wait[42] == 10); // first seq preserved
	}
	// An ack for 12 covers the original send at 10, even though we're now at seq 15.
	CHECK(!reliable_include(wait, 42, 15, 12, true));
	CHECK(!wait.has(42)); // stopped tracking
	printf("  reliable_include retains first seq: ok\n");
}

static void test_reliable_include_ack_too_old() {
	// An ack that predates the first send doesn't confirm anything.
	FakeMap wait;
	reliable_include(wait, 42, 10, 0, false);
	CHECK(reliable_include(wait, 42, 11, 9, true)); // ack 9 < first send 10 → resend
	CHECK(wait.has(42));
	printf("  reliable_include ack too old: ok\n");
}

static void test_reliable_include_no_ack_flag() {
	// has_ack=false means the peer has never acked anything; last_acked is meaningless
	// and must not be trusted even though its value looks newer.
	FakeMap wait;
	reliable_include(wait, 42, 10, 0, false);
	CHECK(reliable_include(wait, 42, 11, 9999, false));
	CHECK(wait.has(42));
	printf("  reliable_include ignores last_acked without has_ack: ok\n");
}

static void test_reliable_include_rollover() {
	// First send just before the wrap, ack just after it.
	FakeMap wait;
	CHECK(reliable_include(wait, 7, 65530, 0, false));
	CHECK(wait[7] == 65530);
	CHECK(!reliable_include(wait, 7, 3, 3, true)); // ack 3 is newer than 65530 → delivered
	CHECK(!wait.has(7));
	printf("  reliable_include across rollover: ok\n");
}

static void test_reliable_include_independent_records() {
	// Records must not interfere — acking one leaves the others tracked.
	FakeMap wait;
	reliable_include(wait, 1, 10, 0, false);
	reliable_include(wait, 2, 20, 0, false);
	reliable_include(wait, 3, 30, 0, false);
	CHECK(wait.size() == 3);
	CHECK(!reliable_include(wait, 2, 31, 25, true)); // only #2 is covered by ack 25
	CHECK(wait.has(1));
	CHECK(!wait.has(2));
	CHECK(wait.has(3));
	printf("  reliable_include independent records: ok\n");
}

// --- retire_acked ---

static void test_retire_acked() {
	FakeMap wait;
	wait[1] = 10;
	wait[2] = 20;
	wait[3] = 30;

	std::vector<uint32_t> retired;
	retire_acked(wait, 20, retired);

	// Entries first sent at or before seq 20 are confirmed delivered and dropped.
	CHECK(retired.size() == 2);
	CHECK(!wait.has(1));
	CHECK(!wait.has(2));
	CHECK(wait.has(3)); // 30 is still in flight
	CHECK(wait.size() == 1);
	printf("  retire_acked partial: ok\n");
}

static void test_retire_acked_none() {
	FakeMap wait;
	wait[1] = 50;
	wait[2] = 60;

	std::vector<uint32_t> retired;
	retire_acked(wait, 40, retired);

	CHECK(retired.empty());
	CHECK(wait.size() == 2);
	printf("  retire_acked none eligible: ok\n");
}

static void test_retire_acked_rollover() {
	FakeMap wait;
	wait[1] = 65530; // sent just before the wrap
	wait[2] = 10;    // sent just after

	std::vector<uint32_t> retired;
	retire_acked(wait, 5, retired); // ack 5: covers 65530, not 10

	CHECK(retired.size() == 1);
	CHECK(retired[0] == 1);
	CHECK(!wait.has(1));
	CHECK(wait.has(2));
	printf("  retire_acked across rollover: ok\n");
}

int main() {
	printf("test_seq\n");
	test_seq_basic();
	test_seq_rollover();
	test_seq_half_space_boundary();
	test_reliable_include_first_send();
	test_reliable_include_retains_first_seq();
	test_reliable_include_ack_too_old();
	test_reliable_include_no_ack_flag();
	test_reliable_include_rollover();
	test_reliable_include_independent_records();
	test_retire_acked();
	test_retire_acked_none();
	test_retire_acked_rollover();
	printf("test_seq: %d checks passed\n", checks);
	return 0;
}
