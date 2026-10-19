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
#include <unordered_map>
#include <vector>

#include "test_support.h"
#include "goldnet/codec.h"
#include "goldnet/protocol.h"

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

// --- owe_include / retire_proven ---

typedef std::unordered_map<uint32_t, Owed> OwedMap;

// The first send and RELIABLE_BACKUP more, then nothing until overdue, then another burst.
static void test_owe_include_bursts_then_waits_for_overdue() {
	OwedMap owed;
	uint32_t now = 1000;
	uint16_t seq = 10;
	for (int i = 0; i <= RELIABLE_BACKUP; i++) {
		CHECK(owe_include(owed, 42, seq++, now, 200, RELIABLE_BACKUP));
		now += 16;
	}
	const uint32_t last = now - 16;
	while (now - last < 200) {
		CHECK(!owe_include(owed, 42, seq++, now, 200, RELIABLE_BACKUP));
		now += 16;
	}
	for (int i = 0; i <= RELIABLE_BACKUP; i++) {
		CHECK(owe_include(owed, 42, seq++, now, 200, RELIABLE_BACKUP));
		now += 16;
	}
	CHECK(!owe_include(owed, 42, seq++, now, 200, RELIABLE_BACKUP));
	printf("  owe_include bursts, then waits for overdue: ok\n");
}

// Only an ack of a snapshot that carried the record proves it arrived. An ack of a later one that
// didn't is the client's newest frame, not evidence of the earlier one.
static void test_retire_proven_needs_a_carrier() {
	OwedMap owed;
	owe_include(owed, 42, 10, 0, 1000, 0);
	std::vector<uint32_t> retired;
	retire_proven(owed, 11, retired);
	CHECK(retired.empty());
	CHECK(owed.count(42));
	retire_proven(owed, 10, retired);
	CHECK(retired == std::vector<uint32_t>{ 42 });
	CHECK(!owed.count(42));
	printf("  retire_proven needs an ack of a carrier: ok\n");
}

static void test_retire_proven_independent_records() {
	OwedMap owed;
	owe_include(owed, 1, 10, 0, 1000, 0);
	owe_include(owed, 2, 11, 0, 1000, 0);
	owe_include(owed, 3, 11, 0, 1000, 0);
	std::vector<uint32_t> retired;
	retire_proven(owed, 11, retired);
	CHECK(retired.size() == 2);
	CHECK(owed.count(1) && !owed.count(2) && !owed.count(3));
	printf("  retire_proven independent records: ok\n");
}

static void test_retire_proven_across_rollover() {
	OwedMap owed;
	owe_include(owed, 7, 65535, 0, 0, 0);
	owe_include(owed, 7, 1, 50, 0, 0); // seq skips 0 on the wrap
	std::vector<uint32_t> retired;
	retire_proven(owed, 1, retired);
	CHECK(retired == std::vector<uint32_t>{ 7 });
	printf("  retire_proven across rollover: ok\n");
}

static void test_overdue_is_a_round_trip_and_a_half() {
	CHECK(overdue_ms(0) == 66);
	CHECK(overdue_ms(200) == 316);
	printf("  overdue is a round trip and a half: ok\n");
}

// --- derive_leaves ---

static void test_derive_leaves_is_baseline_minus_visible() {
	FakeSet held;   // what the peer's acked frame says it holds
	FakeSet visible; // what it can see this tick
	for (uint32_t id = 1; id <= 4; id++) {
		held.insert(id);
	}
	visible.insert(2);
	visible.insert(4);
	visible.insert(9); // newly visible — an enter, which rides the entity data, not this diff

	std::vector<uint32_t> send, carry;
	derive_leaves(held, visible, 32, send, carry);
	CHECK(send.size() == 2);
	CHECK(send[0] == 1);
	CHECK(send[1] == 3);
	CHECK(carry.empty());
	printf("  derive_leaves is baseline minus visible: ok\n");
}

static void test_derive_leaves_recomputes_until_acked() {
	// The property the old queue needed bookkeeping for, and this gets for free: while the peer
	// hasn't acked, `held` doesn't move, so every tick derives the identical removal.
	FakeSet held, visible;
	held.insert(7);
	for (int tick = 0; tick < 5; tick++) {
		std::vector<uint32_t> send, carry;
		derive_leaves(held, visible, 32, send, carry);
		CHECK(send.size() == 1);
		CHECK(send[0] == 7);
	}
	// The ack replaces `held` with the frame that excluded it — and the removal stops.
	FakeSet acked; // the frame we sent, which no longer lists 7
	std::vector<uint32_t> send, carry;
	derive_leaves(acked, visible, 32, send, carry);
	CHECK(send.empty());
	printf("  derive_leaves recomputes until the baseline moves: ok\n");
}

static void test_derive_leaves_carries_what_the_cap_held_back() {
	// A removal the cap couldn't fit must be reported as carried, so the caller keeps it in the
	// baseline. Dropping it there instead loses the evidence the next diff needs, and the entity
	// stays drawn on the client forever — the failure the queued design shipped with.
	FakeSet held, visible;
	for (uint32_t id = 1; id <= 5; id++) {
		held.insert(id);
	}
	std::vector<uint32_t> send, carry;
	derive_leaves(held, visible, 2, send, carry);
	CHECK(send.size() == 2);
	CHECK(carry.size() == 3);
	CHECK(carry[0] == 3);
	CHECK(carry[2] == 5);
	printf("  derive_leaves carries what the cap held back: ok\n");
}

static void test_derive_leaves_empty_baseline_owes_nothing() {
	// A joining peer holds nothing, so it is owed no removals — no seeding, no join-time burst.
	FakeSet held, visible;
	visible.insert(1);
	visible.insert(2);
	std::vector<uint32_t> send, carry;
	derive_leaves(held, visible, 32, send, carry);
	CHECK(send.empty());
	CHECK(carry.empty());
	printf("  derive_leaves owes a fresh peer nothing: ok\n");
}

int main() {
	printf("test_seq\n");
	test_seq_basic();
	test_seq_rollover();
	test_seq_half_space_boundary();
	test_owe_include_bursts_then_waits_for_overdue();
	test_retire_proven_needs_a_carrier();
	test_retire_proven_independent_records();
	test_retire_proven_across_rollover();
	test_overdue_is_a_round_trip_and_a_half();
	test_derive_leaves_is_baseline_minus_visible();
	test_derive_leaves_recomputes_until_acked();
	test_derive_leaves_carries_what_the_cap_held_back();
	test_derive_leaves_empty_baseline_owes_nothing();
	printf("test_seq: %d checks passed\n", checks);
	return 0;
}
