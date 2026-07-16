class_name ServerClock
extends RefCounted

## Reusable client-side server-clock estimator — the third goldnet client helper, alongside
## InterpolationBuffer and PredictedBody, and transport-agnostic like them. Feed it server-time
## observations (goldnet emits one per snapshot via the `server_time_received` signal, but any
## beacon RPC or packet stamp works) and it maintains a smoothly-slewed estimate of the current
## server clock. now() is the render timeline that drives interpolation:
##
##   var clock := ServerClock.new()
##   # each snapshot / beacon:  clock.sample(server_time_ms)
##   # each frame:              clock.advance(delta)
##   # to render N ms in the past (a jitter buffer):
##   var render_time := clock.now() - interp_delay
##   var s := interp_buffer.sample(render_time)     # see InterpolationBuffer
##
## Why a slewed offset and not just `server_time - local_time` per packet:
## the estimate is a running offset such that  estimated_server_time = local_time + offset.
## Each observation yields a candidate offset of (server_time_ms - local_recv_ms), which is a
## LOWER bound on the true offset (the server was already at server_time_ms when it sent, and
## has advanced by the one-way delay since). We ratchet the *target* UP immediately toward any
## larger candidate (that packet had the smallest one-way delay we've seen — the best estimate)
## and drift it DOWN slowly toward smaller candidates (tracking real clock drift). The *applied*
## offset then slews toward the target at a bounded rate in advance(), so packet arrival never
## steps the render timeline. Hard-resetting the offset on every packet makes interpolated
## positions stutter by ±(one-way-delay jitter) every tick; this removes that.
##
## Units: sample() takes milliseconds (matching goldnet's u32 header clock and typical
## Time.get_ticks_msec() beacons). now() returns seconds (InterpolationBuffer's recommended unit);
## now_ms() returns milliseconds.

## Max rate the applied offset chases the target, in ms of clock per real second. Every ms of slew
## is a ms/s of render_time dilation, which maps 1:1 to visible drift of moving entities against the
## local (wall-time) camera. Real hardware clock drift is <0.01% (~0.1 ms/s), so 0.5 ms/s (0.05%)
## tracks it with 5x margin while staying well below the perception threshold. Higher values cause
## slow-period jitter as the applied offset chases a target that ratchets up on low-delay packets
## and drifts down between them.
var slew_ms_per_sec := 0.5
## How fast the *target* relaxes toward larger-one-way-delay candidates (real drift tracking).
## Per-observation fraction; 0.005 at 30 Hz ≈ 15 ms/s of target motion.
var target_drift_rate := 0.005
## When |applied - target| exceeds this, snap immediately instead of slewing. Slewing at 0.5 ms/s
## would take tens of minutes to recover a multi-hundred-ms initial error — catastrophic for
## first-sync after a join (the first snapshot is whatever sat in the socket queue while the joiner
## synchronously loaded the map). Slew is still preferred for small drift to keep render_time
## locally near-linear; only big gaps justify a step.
var snap_threshold_ms := 50.0

var _offset_ms := 0.0         # applied offset — what now() sees
var _target_offset_ms := 0.0  # best estimate from observations
var _initialized := false
var _last_server_ms := -1     # newest server-time seen; older observations are stale/reordered


## True once at least one observation has been fed.
func is_synced() -> bool:
	return _initialized


## Estimated current server time in milliseconds. Before the first sample, falls back to local time.
func now_ms() -> float:
	if not _initialized:
		return float(Time.get_ticks_msec())  # pre-sync fallback
	return float(Time.get_ticks_msec()) + _offset_ms


## Estimated current server time in seconds (InterpolationBuffer's recommended unit).
func now() -> float:
	return now_ms() / 1000.0


## Feed one server-time observation (ms). Ignores stale/reordered ones (their apparent one-way delay
## is fake). Updates only the *target*; advance() moves the applied offset toward it. The first
## observation initializes both — now() is usable immediately, then refines.
func sample(server_time_ms: int) -> void:
	if server_time_ms <= _last_server_ms:
		return
	_last_server_ms = server_time_ms
	var candidate := float(server_time_ms - Time.get_ticks_msec())
	if not _initialized:
		_offset_ms = candidate
		_target_offset_ms = candidate
		_initialized = true
		return
	if candidate > _target_offset_ms:
		_target_offset_ms = candidate  # smaller one-way delay → better estimate → ratchet up now
	else:
		_target_offset_ms = lerpf(_target_offset_ms, candidate, target_drift_rate)


## Per-frame slew of the applied offset toward the target. `delta` is the frame time in seconds.
## Bounded rate keeps render_time locally near-linear (at worst the render clock runs at
## (1 ± slew_ms_per_sec/1000) of real time); a gap too large to slew away in reasonable time snaps.
func advance(delta: float) -> void:
	if not _initialized:
		return
	var diff := _target_offset_ms - _offset_ms
	if absf(diff) > snap_threshold_ms:
		_offset_ms = _target_offset_ms
		return
	var max_step := slew_ms_per_sec * delta
	if absf(diff) <= max_step:
		_offset_ms = _target_offset_ms
	else:
		_offset_ms += signf(diff) * max_step


## Forget all sync state (session end / reconnect — old offsets are meaningless).
func reset() -> void:
	_offset_ms = 0.0
	_target_offset_ms = 0.0
	_initialized = false
	_last_server_ms = -1
