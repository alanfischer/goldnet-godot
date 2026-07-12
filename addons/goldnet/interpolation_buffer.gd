class_name InterpolationBuffer
extends RefCounted

## Reusable client-side interpolation buffer. Ships with the goldnet addon as one of
## its "+ helpers", but it is transport-agnostic and standalone — it has no dependency
## on goldnet's wire protocol and works with ANY source of authoritative snapshots
## (goldnet, a stock MultiplayerSynchronizer, a bespoke RPC stream, replay files, …).
##
## GoldSrc/Quake-style entity smoothing: snapshots arrive at the network tick rate
## (jittery, discrete), but you render every frame. Feed each authoritative snapshot
## in with push(); each frame, sample(render_time) returns the transform interpolated
## between the two snapshots that bracket render_time — where render_time is your
## estimated server clock minus an interpolation delay (a jitter buffer). Rendering a
## fixed delay in the past is what makes tick-rate state look continuous.
##
## Pure and self-contained: it holds one entity's history and does the bracket/lerp/
## extrapolate/teleport math. The server-clock estimate and render_time are supplied by
## the caller, so the same buffer works driven per-node or from a central loop.
##
## Units: times are floats in a single consistent unit (seconds recommended). push()
## and sample() must agree. Positions are Vector3; rotations are euler Vector3 (each
## component lerped as an angle, so wrap is handled).

## Snapshot layout: [time, position: Vector3, rotation: Vector3, velocity: Vector3].
const _T := 0
const _POS := 1
const _ROT := 2
const _VEL := 3

## Max snapshots retained. Oldest are dropped once exceeded (~1 s at 60 Hz by default).
var max_size := 64
## A position jump farther than this between consecutive snapshots is treated as a
## teleport (round-reset, respawn, portal): history is cleared so we snap instead of
## lerping across the map. Squared distance, in world units².
var teleport_dist_sq := 144.0  # (12 units)²
## Cap on how far past the last snapshot we extrapolate before holding, in time units.
var extrap_max := 0.25

var _buf: Array = []
var _last_time := -INF  # out-of-order guard: reject snapshots not newer than the last


## True once at least one snapshot has been received.
func has_data() -> bool:
	return not _buf.is_empty()


## Server time of the most recent snapshot (-INF if empty).
func last_time() -> float:
	return _buf[_buf.size() - 1][_T] if not _buf.is_empty() else -INF


func clear() -> void:
	_buf.clear()
	_last_time = -INF


## Feed one authoritative snapshot. Silently drops out-of-order/duplicate times (unreliable
## transport reorders), and clears history on a teleport-sized jump so we don't lerp across it.
## velocity is optional and only used to extrapolate when the buffer runs dry.
func push(time: float, pos: Vector3, rot: Vector3, vel := Vector3.ZERO) -> void:
	if time <= _last_time:
		return
	_last_time = time
	if not _buf.is_empty() and pos.distance_squared_to(_buf[_buf.size() - 1][_POS]) > teleport_dist_sq:
		_buf.clear()
	_buf.append([time, pos, rot, vel])
	if _buf.size() > max_size:
		_buf.pop_front()


## Sample the interpolated transform at render_time (your server-clock estimate minus the
## interpolation delay). Returns { valid: bool, position: Vector3, rotation: Vector3 }.
## valid is false only when no snapshot has ever arrived.
func sample(render_time: float) -> Dictionary:
	if _buf.is_empty():
		return { "valid": false, "position": Vector3.ZERO, "rotation": Vector3.ZERO }

	if _buf.size() == 1:
		return { "valid": true, "position": _buf[0][_POS], "rotation": _buf[0][_ROT] }

	# Find the latest snapshot at or before render_time.
	var s0_idx := -1
	for i in range(_buf.size() - 1, -1, -1):
		if _buf[i][_T] <= render_time:
			s0_idx = i
			break

	# render_time precedes the buffer (fresh entity, clock still warming up) → hold oldest.
	if s0_idx == -1:
		return { "valid": true, "position": _buf[0][_POS], "rotation": _buf[0][_ROT] }

	# Buffer ran dry → extrapolate from the last snapshot's velocity, capped, then hold.
	if s0_idx >= _buf.size() - 1:
		var last: Array = _buf[_buf.size() - 1]
		var vel: Vector3 = last[_VEL] if last.size() > _VEL else Vector3.ZERO
		var overshoot := clampf(render_time - last[_T], 0.0, extrap_max)
		return {
			"valid": true,
			"position": (last[_POS] as Vector3) + vel * overshoot,
			"rotation": last[_ROT],
		}

	# Interpolate between the two bracketing snapshots.
	var s0: Array = _buf[s0_idx]
	var s1: Array = _buf[s0_idx + 1]
	var dt: float = s1[_T] - s0[_T]
	var t := 0.0
	if dt > 0.0:
		t = clampf((render_time - s0[_T]) / dt, 0.0, 1.0)
	var r0: Vector3 = s0[_ROT]
	var r1: Vector3 = s1[_ROT]
	return {
		"valid": true,
		"position": (s0[_POS] as Vector3).lerp(s1[_POS] as Vector3, t),
		"rotation": Vector3(
			lerp_angle(r0.x, r1.x, t),
			lerp_angle(r0.y, r1.y, t),
			lerp_angle(r0.z, r1.z, t)),
	}
