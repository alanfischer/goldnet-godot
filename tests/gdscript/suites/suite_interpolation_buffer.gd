extends "res://suite.gd"
## InterpolationBuffer — the client-side entity smoothing buffer.
##
## Pure and time-independent (the caller supplies render_time), so unlike ServerClock this
## is exhaustively testable in-process with no wall clock involved. Every branch of
## sample() is reachable from here.
##
## The `holding` flag gets the most attention because it has already caused a shipped bug:
## a captured flag "never visually returned" out-of-PVS, because a frozen interp buffer
## re-asserted its stale position every frame and clobbered the reliable teleport that had
## repositioned the entity. `holding` is what tells the caller to stop writing. Its exact
## boundary — false inside the extrapolation window, true past it — is the contract that
## fix depends on.

const Buf := preload("res://addons/goldnet/interpolation_buffer.gd")


func run() -> void:
	_test_empty()
	_test_single_snapshot()
	_test_interpolates_midpoint()
	_test_clamps_at_bracket_ends()
	_test_render_time_before_buffer()
	_test_rejects_out_of_order()
	_test_teleport_clears_history()
	_test_max_size_eviction()
	_test_extrapolates_from_velocity()
	_test_extrapolation_is_capped()
	_test_holding_boundary()
	_test_rotation_lerps_the_short_way()
	_test_angular_extrapolation()
	_test_clear()


# --- degenerate buffers ---

func _test_empty() -> void:
	var b := Buf.new()
	check(not b.has_data(), "a fresh buffer has no data")
	check_eq(b.last_time(), -INF, "last_time() of an empty buffer is -INF")
	var s: Dictionary = b.sample(1.0)
	check(not s["valid"], "sampling an empty buffer is invalid")
	check(not s["holding"], "an empty buffer is not holding")


func _test_single_snapshot() -> void:
	# One snapshot can't be interpolated, so it's returned as-is at any render_time —
	# including times far past it, where two snapshots would extrapolate.
	var b := Buf.new()
	b.push(1.0, Vector3(5, 0, 0), Vector3.ZERO)
	check(b.has_data(), "has_data() after one push")
	check_eq(b.last_time(), 1.0, "last_time() tracks the newest snapshot")
	for rt in [0.0, 1.0, 99.0]:
		var s: Dictionary = b.sample(rt)
		check(s["valid"], "single snapshot is valid at render_time %.1f" % rt)
		check_near_v(s["position"], Vector3(5, 0, 0), 0.001,
			"single snapshot holds its position at render_time %.1f" % rt)
		check(not s["holding"], "a single snapshot is not 'holding' at render_time %.1f" % rt)


# --- interpolation ---

func _test_interpolates_midpoint() -> void:
	# 10 units apart — deliberately inside the default teleport threshold (12), so this
	# stays a test of interpolation. A larger step would trip the teleport path and clear
	# the history instead, which is the subject of _test_teleport_clears_history.
	var far := Vector3(6, 8, 0)
	var b := Buf.new()
	b.push(1.0, Vector3(0, 0, 0), Vector3.ZERO)
	b.push(2.0, far, Vector3.ZERO)

	var s: Dictionary = b.sample(1.5)
	check_near_v(s["position"], far * 0.5, 0.001, "midpoint is the average of the bracket")
	check(not s["holding"], "interpolating between two snapshots is not holding")

	# A few more points, to pin that it's linear rather than merely correct at the middle.
	for f in [0.0, 0.25, 0.75, 1.0]:
		var got: Dictionary = b.sample(1.0 + f)
		check_near_v(got["position"], far * f, 0.001,
			"interpolated at t=%.2f through the bracket" % f)


func _test_clamps_at_bracket_ends() -> void:
	# Three snapshots so render_time can sit inside a bracket that isn't the last one.
	#
	# SCOPE — established by mutation: removing the `clampf(..., 0.0, 1.0)` on the bracket
	# fraction leaves this (and every other) test passing, because the clamp is unreachable.
	# s0 is chosen as the latest snapshot at or before render_time, so whenever a bracket
	# exists render_time already lies within [s0, s1] and the fraction is in range by
	# construction. It's defensive, not load-bearing — worth knowing before anyone "fixes"
	# a test to cover it.
	var b := Buf.new()
	b.push(1.0, Vector3(0, 0, 0), Vector3.ZERO)
	b.push(2.0, Vector3(10, 0, 0), Vector3.ZERO)
	b.push(3.0, Vector3(20, 0, 0), Vector3.ZERO)
	check_near_v(b.sample(2.0)["position"], Vector3(10, 0, 0), 0.001,
		"exactly on a snapshot returns that snapshot")


func _test_render_time_before_buffer() -> void:
	# Fresh entity / clock still warming up: render_time precedes everything we hold. The
	# oldest snapshot is held, and this is NOT 'holding' — the stream is alive, we're just
	# early, so the caller should keep writing.
	var b := Buf.new()
	b.push(10.0, Vector3(1, 2, 3), Vector3.ZERO)
	b.push(11.0, Vector3(4, 5, 6), Vector3.ZERO)
	var s: Dictionary = b.sample(5.0)
	check(s["valid"], "render_time before the buffer is still valid")
	check_near_v(s["position"], Vector3(1, 2, 3), 0.001, "holds the oldest snapshot")
	check(not s["holding"], "being early is not 'holding'")


# --- push guards ---

func _test_rejects_out_of_order() -> void:
	# Unreliable transport reorders and duplicates. An older snapshot must not rewrite
	# history, or the buffer would interpolate backwards.
	var b := Buf.new()
	b.push(1.0, Vector3(0, 0, 0), Vector3.ZERO)
	b.push(2.0, Vector3(10, 0, 0), Vector3.ZERO)
	b.push(1.5, Vector3(999, 0, 0), Vector3.ZERO)  # late arrival
	b.push(2.0, Vector3(888, 0, 0), Vector3.ZERO)  # duplicate time
	check_eq(b.last_time(), 2.0, "out-of-order and duplicate pushes leave last_time() alone")
	check_near_v(b.sample(1.5)["position"], Vector3(5, 0, 0), 0.001,
		"the rejected snapshots did not enter the bracket")


func _test_teleport_clears_history() -> void:
	# A jump farther than teleport_dist_sq is a respawn/portal/round-reset, not motion.
	# Lerping across it would drag the entity through the map.
	var b := Buf.new()
	b.teleport_dist_sq = 144.0  # (12 units)²
	b.push(1.0, Vector3(0, 0, 0), Vector3.ZERO)
	b.push(2.0, Vector3(5, 0, 0), Vector3.ZERO)   # 5 units: ordinary motion
	check_near_v(b.sample(1.5)["position"], Vector3(2.5, 0, 0), 0.001,
		"a sub-threshold move still interpolates")

	b.push(3.0, Vector3(500, 0, 0), Vector3.ZERO)  # way past the threshold
	# History was cleared, so only the teleport destination remains — sampling anywhere
	# returns it rather than a point between here and there.
	check_near_v(b.sample(2.5)["position"], Vector3(500, 0, 0), 0.001,
		"a teleport snaps instead of lerping across the map")
	check_near_v(b.sample(3.0)["position"], Vector3(500, 0, 0), 0.001,
		"the teleport destination is the only history left")


func _test_max_size_eviction() -> void:
	var b := Buf.new()
	b.max_size = 4
	for i in 10:
		b.push(float(i), Vector3(float(i), 0, 0), Vector3.ZERO)
	# Only the newest 4 survive (times 6..9), so times 0..5 are no longer bracketable and a
	# render_time back there falls off the front of the buffer.
	check_near_v(b.sample(0.0)["position"], Vector3(6, 0, 0), 0.001,
		"evicted snapshots are gone — the oldest retained one is held instead")
	check_near_v(b.sample(7.5)["position"], Vector3(7.5, 0, 0), 0.001,
		"retained snapshots still interpolate normally")
	check_eq(b.last_time(), 9.0, "eviction drops from the front, not the back")


# --- extrapolation and holding ---

func _test_extrapolates_from_velocity() -> void:
	var b := Buf.new()
	b.extrap_max = 0.25
	b.push(1.0, Vector3(0, 0, 0), Vector3.ZERO, Vector3(10, 0, 0))
	b.push(2.0, Vector3(10, 0, 0), Vector3.ZERO, Vector3(10, 0, 0))
	# 0.1 s past the last snapshot at 10 units/s → 1 unit further along.
	check_near_v(b.sample(2.1)["position"], Vector3(11, 0, 0), 0.001,
		"extrapolates past the last snapshot using its velocity")


func _test_extrapolation_is_capped() -> void:
	# Without a cap, a peer that goes quiet sends its entity flying off the map.
	var b := Buf.new()
	b.extrap_max = 0.25
	b.push(1.0, Vector3(0, 0, 0), Vector3.ZERO, Vector3(10, 0, 0))
	b.push(2.0, Vector3(10, 0, 0), Vector3.ZERO, Vector3(10, 0, 0))
	var capped := Vector3(10 + 10 * 0.25, 0, 0)  # 12.5
	check_near_v(b.sample(2.25)["position"], capped, 0.001, "extrapolation reaches the cap")
	check_near_v(b.sample(5.0)["position"], capped, 0.001,
		"extrapolation holds at the cap instead of running away")
	check_near_v(b.sample(60.0)["position"], capped, 0.001,
		"still capped a minute later")


func _test_holding_boundary() -> void:
	# The regression guard. `holding` is the caller's signal to STOP re-asserting the
	# transform, so that an out-of-band teleport (a reliable state RPC arriving while the
	# entity's PVS-gated snapshot stream is quiet) isn't clobbered every frame.
	var b := Buf.new()
	b.extrap_max = 0.25
	b.push(1.0, Vector3(0, 0, 0), Vector3.ZERO, Vector3(10, 0, 0))
	b.push(2.0, Vector3(10, 0, 0), Vector3.ZERO, Vector3(10, 0, 0))

	check(not b.sample(2.0)["holding"], "not holding exactly at the last snapshot")
	check(not b.sample(2.1)["holding"], "not holding inside the extrapolation window")
	check(not b.sample(2.25)["holding"], "not holding exactly at extrap_max")
	check(b.sample(2.26)["holding"], "holding just past extrap_max")
	check(b.sample(9.0)["holding"], "still holding long past extrap_max")

	# Interpolating within the buffer must never report holding, or the caller would stop
	# writing while the stream is perfectly healthy.
	check(not b.sample(1.5)["holding"], "never holding while a bracket exists")


# --- rotation ---

func _test_rotation_lerps_the_short_way() -> void:
	# Euler components are lerped as ANGLES, so 350° → 10° must cross 0° (20° of travel),
	# not run 340° backwards through the whole circle.
	var b := Buf.new()
	var from := deg_to_rad(350.0)
	var to := deg_to_rad(10.0)
	b.push(1.0, Vector3.ZERO, Vector3(0, from, 0))
	b.push(2.0, Vector3.ZERO, Vector3(0, to, 0))

	var mid: float = (b.sample(1.5)["rotation"] as Vector3).y
	# The short way puts the midpoint at 0° (i.e. 0 or TAU); the long way would put it
	# at 180°. Compare via wrapping so either representation of "0" passes.
	var off_zero := absf(wrapf(mid, -PI, PI))
	check(off_zero < deg_to_rad(1.0),
		"yaw 350°→10° passes through 0°, not 180° (midpoint at %.1f°)" % rad_to_deg(mid))


func _test_angular_extrapolation() -> void:
	var b := Buf.new()
	b.extrap_max = 0.25
	var spin := Vector3(0, 2.0, 0)  # rad/s
	b.push(1.0, Vector3.ZERO, Vector3(0, 0, 0), Vector3.ZERO, spin)
	b.push(2.0, Vector3.ZERO, Vector3(0, 1.0, 0), Vector3.ZERO, spin)
	check_near((b.sample(2.1)["rotation"] as Vector3).y, 1.0 + 2.0 * 0.1, 0.001,
		"rotation extrapolates from angular velocity")
	check_near((b.sample(5.0)["rotation"] as Vector3).y, 1.0 + 2.0 * 0.25, 0.001,
		"angular extrapolation obeys the same cap as linear")


func _test_clear() -> void:
	var b := Buf.new()
	b.push(1.0, Vector3(1, 1, 1), Vector3.ZERO)
	b.clear()
	check(not b.has_data(), "clear() empties the buffer")
	check(not b.sample(1.0)["valid"], "sampling after clear() is invalid")
	# Time must be re-pushable from scratch after a clear (session restart / respawn),
	# so the out-of-order guard has to have been reset along with the data.
	b.push(0.5, Vector3(2, 2, 2), Vector3.ZERO)
	check(b.has_data(), "an older timestamp is accepted after clear()")
