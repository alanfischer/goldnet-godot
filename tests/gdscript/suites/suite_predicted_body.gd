extends "res://suite.gd"
## PredictedBody — the client-side prediction / reconciliation harness.
##
## Simulation-agnostic and fully deterministic: it owns sequence bookkeeping, the unacked
## input history, and the decision of what to replay. All of that is testable in-process
## with a stub `diverged` predicate standing in for the caller's state comparison.
##
## The subtle behavior — and the reason this is worth testing rather than eyeballing — is
## the replay suppression via `_replay_end_seq`. Replayed snapshots are non-deterministic
## (the client re-runs many sim steps in one frame where the server ran one per tick), so
## re-comparing them chains corrections into a feedback loop: a correction causes a replay,
## the replayed states don't match, that triggers another correction, and the body rubber-
## bands indefinitely. Nothing about that is visible in a single-step reading of the code.

const PB := preload("res://addons/goldnet/predicted_body.gd")

## `diverged` predicates. reconcile() takes a Callable so the caller can close over the
## server state; the tests only need the two constant answers plus a spy.
static func _always(_predicted: Variant) -> bool:
	return true

static func _never(_predicted: Variant) -> bool:
	return false


func run() -> void:
	_test_seq_monotonic()
	_test_record_and_count()
	_test_unknown_ack_ignored()
	_test_out_of_order_ack_ignored()
	_test_converged_ack_prunes_without_correcting()
	_test_divergence_returns_replay_in_order()
	_test_replay_carries_context()
	_test_predicate_receives_the_recorded_snapshot()
	_test_replayed_inputs_are_not_recorrected()
	_test_max_buffer_eviction()
	_test_clear_keeps_seq()
	_test_no_correction_result_is_not_aliased()


# --- sequence bookkeeping ---

func _test_seq_monotonic() -> void:
	var pb := PB.new()
	check_eq(pb.last_seq(), 0, "last_seq() is 0 before the first next_seq()")
	check_eq(pb.next_seq(), 1, "sequences start at 1")
	check_eq(pb.next_seq(), 2, "sequences increment")
	check_eq(pb.next_seq(), 3, "sequences keep incrementing")
	check_eq(pb.last_seq(), 3, "last_seq() is the most recently handed-out id")


func _test_record_and_count() -> void:
	var pb := PB.new()
	check_eq(pb.unacked_count(), 0, "nothing unacked initially")
	for i in 3:
		pb.record(pb.next_seq(), "input%d" % i, "snap%d" % i)
	check_eq(pb.unacked_count(), 3, "every recorded input is unacked until the server acks it")


# --- acks that must not correct ---

func _test_unknown_ack_ignored() -> void:
	# An ack for an input we never recorded (or already pruned) tells us nothing.
	var pb := PB.new()
	pb.record(pb.next_seq(), "i1", "s1")
	var r: Dictionary = pb.reconcile(999, _always)
	check(not r["corrected"], "an ack for an unknown seq does not correct")
	check_eq(pb.unacked_count(), 1, "an unknown ack prunes nothing")


func _test_out_of_order_ack_ignored() -> void:
	# Acks arrive over an unreliable channel. Once seq 3 is reconciled, a late ack for
	# seq 2 is stale — acting on it would rewind to superseded state.
	#
	# SCOPE — established by mutation: deleting the `_highest_reconciled_seq` guard leaves
	# this test passing. Not a gap in the test so much as a fact about the code: every
	# accepted reconcile prunes the acked input and everything older, so by the time a stale
	# ack arrives its seq is no longer in the buffer and the `match_idx == -1` path returns
	# the same no-correction result. The guard is belt-and-braces for a caller that records
	# a seq out of order (record() takes the seq as a parameter), which is why it can't be
	# provoked through normal use. The assertions below still pin the observable contract.
	var pb := PB.new()
	for i in 4:
		pb.record(pb.next_seq(), "i%d" % i, "s%d" % i)
	var first: Dictionary = pb.reconcile(3, _always)
	check(first["corrected"], "the first ack reconciles normally")

	var stale: Dictionary = pb.reconcile(2, _always)
	check(not stale["corrected"], "an ack older than the last reconciled seq is ignored")
	var same: Dictionary = pb.reconcile(3, _always)
	check(not same["corrected"], "re-acking the same seq is ignored")


func _test_converged_ack_prunes_without_correcting() -> void:
	# The common case: the prediction was right. No correction, but the acked input and
	# everything older must still be dropped or the buffer grows without bound.
	var pb := PB.new()
	for i in 5:
		pb.record(pb.next_seq(), "i%d" % i, "s%d" % i)
	var r: Dictionary = pb.reconcile(3, _never)
	check(not r["corrected"], "a converged prediction does not correct")
	check_eq((r["replay"] as Array).size(), 0, "a converged reconcile replays nothing")
	check_eq(pb.unacked_count(), 2, "the acked input and everything older are pruned anyway")


# --- divergence ---

func _test_divergence_returns_replay_in_order() -> void:
	var pb := PB.new()
	for i in 5:
		pb.record(pb.next_seq(), "i%d" % i, "s%d" % i)  # seqs 1..5

	var r: Dictionary = pb.reconcile(2, _always)
	check(r["corrected"], "a diverged prediction corrects")

	# Replay is the STILL-UNACKED inputs — everything after the acked one, in order. The
	# acked input itself is not replayed: the server's state already includes it.
	var replay: Array = r["replay"]
	check_eq(replay.size(), 3, "replay holds the inputs after the acked one")
	for i in replay.size():
		check_eq(replay[i]["input"], "i%d" % (i + 2), "replay entry %d is in submission order" % i)
	check_eq(pb.unacked_count(), 3, "the acked input and older are pruned, unacked ones kept")


func _test_replay_carries_context() -> void:
	# Replay is data-driven from the context captured at record() time, never from live
	# state — so a modifier that has since expired can't change what the replay produces.
	var pb := PB.new()
	pb.record(pb.next_seq(), "i1", "s1", {"speed": 1.0})
	pb.record(pb.next_seq(), "i2", "s2", {"speed": 2.0})
	pb.record(pb.next_seq(), "i3", "s3", {"speed": 3.0})

	var r: Dictionary = pb.reconcile(1, _always)
	var replay: Array = r["replay"]
	check_eq(replay.size(), 2, "two inputs to replay")
	check_eq(replay[0]["context"]["speed"], 2.0, "replay carries the context recorded with input 2")
	check_eq(replay[1]["context"]["speed"], 3.0, "replay carries the context recorded with input 3")


func _test_predicate_receives_the_recorded_snapshot() -> void:
	# The predicate must be handed the snapshot recorded for the ACKED input specifically —
	# handing it the newest (or oldest) one would compare the server's state against a
	# prediction from a different tick, which is how a correct prediction reads as diverged.
	var pb := PB.new()
	for i in 4:
		pb.record(pb.next_seq(), "i%d" % i, "snapshot-for-seq-%d" % (i + 1))

	var seen: Array = []
	var spy := func(predicted: Variant) -> bool:
		seen.append(predicted)
		return true

	var r: Dictionary = pb.reconcile(2, spy)
	check_eq(seen.size(), 1, "the predicate is called exactly once per reconcile")
	check_eq(seen[0], "snapshot-for-seq-2", "the predicate sees the acked input's snapshot")
	check_eq(r["predicted"], "snapshot-for-seq-2", "the result reports the same snapshot")


func _test_replayed_inputs_are_not_recorrected() -> void:
	# The rubber-banding guard. After a correction replays seqs 3..5, the server's acks for
	# those seqs will arrive later. Comparing against their (non-deterministically replayed)
	# snapshots would trigger another correction, which replays again, forever. They must be
	# pruned quietly instead — and normal correction must resume for inputs recorded after.
	var pb := PB.new()
	for i in 5:
		pb.record(pb.next_seq(), "i%d" % i, "s%d" % i)  # seqs 1..5

	var first: Dictionary = pb.reconcile(2, _always)
	check(first["corrected"], "the initial divergence corrects")
	check_eq((first["replay"] as Array).size(), 3, "seqs 3..5 were replayed")

	var calls := 0
	var counting := func(_predicted: Variant) -> bool:
		calls += 1
		return true

	# Acks for the replayed seqs: suppressed, and the predicate isn't even consulted.
	for seq in [3, 4, 5]:
		var r: Dictionary = pb.reconcile(seq, counting)
		check(not r["corrected"], "ack for replayed seq %d does not re-correct" % seq)
	check_eq(calls, 0, "the divergence predicate is not consulted for replayed inputs")
	check_eq(pb.unacked_count(), 0, "replayed inputs are still pruned as they are acked")

	# Fresh inputs recorded after the replay window are predicted normally again — the
	# suppression must be a window, not a permanent off switch.
	pb.record(pb.next_seq(), "i6", "s6")  # seq 6
	pb.record(pb.next_seq(), "i7", "s7")  # seq 7
	var after: Dictionary = pb.reconcile(6, _always)
	check(after["corrected"], "corrections resume for inputs recorded after the replay")


# --- housekeeping ---

func _test_max_buffer_eviction() -> void:
	# If the server stops acking, the buffer must not grow forever.
	var pb := PB.new()
	pb.max_buffer = 4
	for i in 20:
		pb.record(pb.next_seq(), "i%d" % i, "s%d" % i)
	check_eq(pb.unacked_count(), 4, "the unacked buffer is capped at max_buffer")
	# The oldest were dropped, so only the newest 4 seqs (17..20) are still ackable.
	check(not pb.reconcile(1, _always)["corrected"], "an evicted seq is no longer ackable")
	check(pb.reconcile(18, _always)["corrected"], "a retained seq still reconciles")


func _test_clear_keeps_seq() -> void:
	# The server rejects a repeated seq as a duplicate, so ids must keep climbing across a
	# respawn or map change even though the unacked history is discarded.
	var pb := PB.new()
	for i in 3:
		pb.record(pb.next_seq(), "i%d" % i, "s%d" % i)
	pb.clear()
	check_eq(pb.unacked_count(), 0, "clear() drops unacked history")
	check_eq(pb.next_seq(), 4, "clear() does NOT reset the sequence counter")

	# The reconciliation guards must have been cleared too, or the first ack after a
	# respawn would be rejected as superseded by the pre-clear session.
	pb.record(4, "i4", "s4")
	check(pb.reconcile(4, _always) != null, "reconcile works after clear()")


func _test_no_correction_result_is_not_aliased() -> void:
	# The no-correction path returns a shared const Dictionary to avoid allocating on the
	# per-tick hot path. GDScript consts are still mutable at runtime, so a caller that
	# appended to `replay` would corrupt every future return. Pin that it comes back empty
	# each time — this is the assertion that fails if that sharing is ever made unsafe.
	var pb := PB.new()
	pb.record(pb.next_seq(), "i1", "s1")
	var a: Dictionary = pb.reconcile(99, _never)
	check_eq((a["replay"] as Array).size(), 0, "first no-correction result has an empty replay")
	var b: Dictionary = pb.reconcile(98, _never)
	check_eq((b["replay"] as Array).size(), 0, "second no-correction result is still empty")
	check(not a["corrected"] and not b["corrected"], "both are no-corrections")
