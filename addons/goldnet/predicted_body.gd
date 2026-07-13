class_name PredictedBody
extends RefCounted

## Reusable client-side prediction + reconciliation harness. Ships with the goldnet addon as one
## of its "+ helpers", but it is simulation-agnostic and standalone: it knows nothing about
## movement, physics, or any particular state. It owns the generic control flow of client-side
## prediction — a monotonic input sequence, a history of still-unacked inputs, and, when the
## server acks an input, out-of-order rejection, matching the acked input, deciding whether the
## prediction diverged, and (on divergence) reporting the still-unacked inputs to replay on top of
## the authoritative state. The domain-specific pieces stay with the caller:
##   - what a "snapshot" of predicted state is (captured at record() time; opaque here),
##   - how far off counts as "diverged" (a predicate passed to reconcile()),
##   - how to apply the server state and how to re-simulate one input (done with the replay list
##     reconcile() returns).
## so the same harness drives a GoldSrc movement capsule, a vehicle, or anything with an
## input -> state simulation. WizardWars' netmove ClientMovement is one such consumer.
##
## Typical use, per predicted body:
##   var pb := PredictedBody.new()
##   # each physics frame:
##   var seq := pb.next_seq()
##   # ... build an input carrying this seq, simulate it locally against your body ...
##   pb.record(seq, input, my_state_snapshot(), context)   # context replays with the input
##   # when the server's ack for `acked_seq` (with its authoritative state) arrives:
##   var r := pb.reconcile(acked_seq, func(predicted, unacked): return is_diverged(predicted))
##   if r.corrected:
##       apply_server_state()                                 # rewind the body to authoritative
##       for e in r.replay: resimulate(e.input, e.context)    # replay still-unacked inputs
##
## Determinism note: replay is data-driven from the `context` captured at record() time, never
## from live state — so state that has since changed (an expired buff, a lane since left) cannot
## make the replay diverge from what was originally predicted.

## Max unacked inputs retained (~2.1 s at 60 Hz). Oldest drop if the server never acks.
var max_buffer := 128

var _buffer: Array = []            # [{ seq, input, snapshot, context }], oldest first
var _next_seq := 1
var _highest_reconciled_seq := -1  # reject out-of-order / already-superseded server acks
var _replay_end_seq := -1          # inputs <= this were produced by a replay; don't re-correct them


## Monotonic id for the next input. Never resets across clear() — the server drops a repeated seq
## as a duplicate, so it must keep climbing (e.g. across respawns).
func next_seq() -> int:
	var seq := _next_seq
	_next_seq += 1
	return seq


## The most recently handed-out sequence id (i.e. next_seq() - 1), or 0 before the first next_seq().
func last_seq() -> int:
	return _next_seq - 1


## Record a locally-simulated input as unacked. `snapshot` is your opaque capture of the predicted
## state right after simulating this input (reconcile hands it back to the `diverged` predicate);
## `context` is opaque data replayed with the input on correction (e.g. modifiers captured now).
func record(seq: int, input: Variant, snapshot: Variant, context: Variant = null) -> void:
	_buffer.append({ "seq": seq, "input": input, "snapshot": snapshot, "context": context })
	if _buffer.size() > max_buffer:
		_buffer.pop_front()


## Process the server's ack for `acked_seq` (whose authoritative state your `diverged` predicate
## closes over). Returns a Dictionary:
##   { corrected: bool, replay: Array, predicted, unacked: int }
## When corrected is true, `replay` is the still-unacked inputs (each { input, context }, in order)
## to re-simulate after you snap the body to the server state, `predicted` is the opaque snapshot
## recorded for the acked input, and `unacked` is how many inputs will be replayed — the last two
## are there for the caller's own diagnostics. When false, `replay` is empty and the other fields
## are absent. The acked input and everything older are pruned in every accepted path, so the
## buffer then holds only still-unacked inputs.
##
## `diverged.call(predicted_snapshot) -> bool` decides whether the prediction for the acked input
## is far enough from the server's state to warrant a correction. It is called at most once per
## reconcile, and only when the acked input hasn't already been superseded or replayed.
func reconcile(acked_seq: int, diverged: Callable) -> Dictionary:
	var none := { "corrected": false, "replay": [] }
	# Out-of-order / superseded: we already reconciled this seq or a newer one.
	if acked_seq <= _highest_reconciled_seq:
		return none
	var match_idx := -1
	for i in _buffer.size():
		if _buffer[i]["seq"] == acked_seq:
			match_idx = i
			break
	if match_idx == -1:
		return none
	_highest_reconciled_seq = acked_seq
	# Suppress corrections for inputs a prior correction already replayed: replayed snapshots are
	# non-deterministic (the caller re-runs many sim steps in one frame vs one/frame on the server),
	# so re-comparing them chains corrections. Trust the server state and let fresh predictions settle.
	if acked_seq <= _replay_end_seq:
		_prune_up_to(match_idx)
		return none
	var predicted: Variant = _buffer[match_idx]["snapshot"]
	var unacked := _buffer.size() - match_idx - 1
	if not diverged.call(predicted):
		_prune_up_to(match_idx)
		return none
	var replay: Array = []
	for i in range(match_idx + 1, _buffer.size()):
		replay.append({ "input": _buffer[i]["input"], "context": _buffer[i]["context"] })
	# The last unacked input is now the newest replayed seq — inputs up to it are non-deterministic.
	if _buffer.size() > 0:
		_replay_end_seq = _buffer[_buffer.size() - 1]["seq"]
	_prune_up_to(match_idx)
	return { "corrected": true, "replay": replay, "predicted": predicted, "unacked": unacked }


## Number of still-unacked recorded inputs.
func unacked_count() -> int:
	return _buffer.size()


## Drop unacked history + reconciliation guards. Does NOT reset next_seq() (see next_seq).
func clear() -> void:
	_buffer.clear()
	_highest_reconciled_seq = -1
	_replay_end_seq = -1


## Discard the matched entry and everything older, keeping only still-unacked inputs. In-place
## shift to avoid reallocating a new array.
func _prune_up_to(idx: int) -> void:
	var start := idx + 1
	if start < _buffer.size():
		var remaining := _buffer.size() - start
		for i in remaining:
			_buffer[i] = _buffer[i + start]
		_buffer.resize(remaining)
	else:
		_buffer.clear()
