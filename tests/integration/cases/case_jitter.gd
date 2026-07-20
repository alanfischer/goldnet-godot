extends "res://test_case.gd"
## State converges and never regresses under jittered delivery.
##
## With a wide latency range the send-side sim draws a different delay per packet. The
## server drives the entity monotonically along +X, then holds; the client asserts it
## never sees a decrease and lands on the held value.
##
## WHAT THIS DOES NOT COVER — read before treating it as reorder coverage.
##
## `apply_snapshot`'s `_seq_newer` guard (which rejects a snapshot older than the one
## already applied) is NOT exercised here. Verified by mutation: deleting that guard
## leaves this case passing. The reason is structural — `_sim_queue_send` clamps every
## packet's fire time to `_last_fire_at`, so the send-side sim's queue is monotonic by
## construction and cannot emit out of order no matter how wide the latency spread.
##
## The guard exists for *real* network reordering (UDP, WiFi power-save/DTIM bursts),
## which the sim deliberately does not reproduce — reorder simulation was dropped when
## the netsim moved into goldnet. Covering it would need a reorder knob in the sim. The
## `_seq_newer` arithmetic itself is unit-tested in ../test_seq.cpp; it's the wiring at
## the call site that has no coverage.
##
## So: this case pins jitter tolerance and the no-regression contract as delivered by the
## ordering cursor. It is not evidence that the stale-snapshot guard works.

const RATE := 10.0        # units/sec along +X
const RAMP_START := 0.5
const RAMP_END := 5.0
const FINAL_X := (RAMP_END - RAMP_START) * RATE
const TOL := 0.01

var _max_x := -INF
var _regressions := 0
var _worst := 0.0
var _samples := 0


func setup(is_server: bool) -> void:
	timeout_s = 16.0
	if is_server:
		# Seed the sim first: this case's verdict depends on random latency draws, so
		# without a seed a failure here couldn't be reproduced or bisected — which is the
		# entire argument for the seed existing. Any non-zero constant will do.
		goldnet().sim_seed = 20260720
		# Wide spread so consecutive packets draw meaningfully different delays.
		goldnet().latency_min_ms = 20
		goldnet().latency_max_ms = 120


func server_step(t: float) -> void:
	if not spawn_once(t, 1):
		return
	var e: Node3D = main.entities().get("Ent0")
	if e == null:
		return
	var x := clampf((t - RAMP_START) * RATE, 0.0, FINAL_X)
	e.position = Vector3(x, 0.0, 0.0)


func client_step(t: float) -> void:
	var e: Node3D = main.entities().get("Ent0")
	if e == null:
		if t > 8.0:
			fail("entity never arrived under jittered delivery")
			finish()
		return

	var x: float = e.position.x
	_samples += 1
	if x < _max_x - TOL:
		_regressions += 1
		_worst = maxf(_worst, _max_x - x)
	_max_x = maxf(_max_x, x)

	# Judge only once the server has finished ramping and the queue has drained.
	if t < RAMP_END + 2.0:
		return

	check_eq(_regressions, 0,
		"position never moved backwards under 20-120ms jitter (worst regression %.3f over %d samples)"
			% [_worst, _samples])
	check(absf(x - FINAL_X) <= TOL,
		"converged to the final value %.2f under jitter (got %.3f)" % [FINAL_X, x])
	print("[client] %d samples, %d regressions, final x=%.3f" % [_samples, _regressions, x])
	finish()
