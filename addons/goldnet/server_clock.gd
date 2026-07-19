class_name ServerClock
extends RefCounted

## Reusable client-side server-clock estimator — the third goldnet client helper, alongside
## InterpolationBuffer and PredictedBody, and transport-agnostic like them. Feed it server-time
## observations (goldnet emits one per snapshot via the `server_time_received` signal, but any
## beacon RPC or packet stamp works) and it maintains a smoothly-corrected estimate of the current
## server clock. now() is the render timeline that drives interpolation:
##
##   var clock := ServerClock.new()
##   # each snapshot / beacon:  clock.sample(server_time_ms)
##   # each frame:              clock.advance(delta)
##   # to render N ms in the past (a jitter buffer):
##   var render_time := clock.now() - interp_delay
##   var s := interp_buffer.sample(render_time)     # see InterpolationBuffer
##
## The estimate is a running offset such that  estimated_server_time = local_time + offset.
## Each observation yields a candidate offset of (server_time_ms - local_recv_ms). That candidate is
## a LOWER bound on the true offset: the server was already at server_time_ms when it sent, and has
## advanced by the one-way network delay since — so a candidate is (true_offset − one_way_delay).
## The BEST (largest) candidate is the one with the smallest one-way delay, i.e. the most accurate
## clock sample. This is the classic min-delay filter used by NTP/chrony/PTP and Source-style netcode.
##
## Two-stage design (target vs. applied), Source's CClockDriftMgr in spirit:
##  1. TARGET = the min-delay estimate: the largest candidate still inside a sliding window. Because
##     it tracks only the low-delay floor, it barely moves in steady state — per-packet delay jitter
##     does NOT enter it. The window length sets how fast a *real* downward clock shift is tracked
##     (stale low-delay samples age out, letting the target fall).
##  2. APPLIED offset = what now() reports. Each frame it moves a bounded fraction of the way toward
##     the target (proportional correction, speed-capped), so it low-passes even the target's small
##     step-downs into sub-ms ripple and never steps the render timeline. A hard SNAP is reserved for
##     genuine discontinuities (first sync after join, an alt-tab hitch, a real latency step) — the
##     only time the gap can exceed snap_threshold_ms now that the target is jitter-free.
##
## Why not hard-reset the offset per packet: that stutters interpolated positions by ±(one-way-delay
## jitter) every tick. Why not a flat slow slew toward a per-packet target (the earlier design): the
## target itself carried the link jitter and drifted between packets, so a slow applied slew fell
## progressively behind until it tripped the snap threshold in normal operation — a periodic snap.
##
## Units: sample() takes milliseconds (matching goldnet's u32 header clock and typical
## Time.get_ticks_msec() beacons). now() returns seconds (InterpolationBuffer's recommended unit);
## now_ms() returns milliseconds.

## Sliding window holding recent offset samples. The target is the min-delay (largest) candidate
## still inside it; the window length is how fast a real downward clock shift is tracked as the
## stale low-delay samples that were holding the target up age out.
var sample_window_ms := 1000.0
## Applied offset closes this fraction of the remaining error toward target per second (proportional
## correction). 6.0 ≈ a ~170 ms time constant: converges fast, stays smooth. Since the steady-state
## target is nearly static, this trivially keeps up and |applied − target| stays in single-digit ms.
var correction_rate := 6.0
## Hard cap on how fast the applied offset chases the target, in ms of clock per real second. Bounds
## the correction of a large (but sub-snap) gap to a slew instead of a jump — every ms/s of slew is a
## ms/s of render_time dilation (visible drift of moving entities against the wall-time camera), so
## 20 ms/s = 2% dilation, imperceptible and only ever engaged briefly after a discontinuity.
var max_slew_ms_per_sec := 20.0
## When |applied − target| exceeds this, snap immediately instead of correcting. Slewing away a
## multi-hundred-ms error (first sync after a join — the first snapshot is whatever sat in the socket
## queue during the synchronous map load) would take far too long, so a big gap justifies one step.
## Steady-state jitter never reaches this because the target is a stable min-delay envelope.
var snap_threshold_ms := 150.0

var _offset_ms := 0.0         # applied offset — what now() sees
var _initialized := false
var _last_server_ms := -1     # newest server-time seen; older observations are stale/reordered
var _samples: Array = []      # sliding window of [local_recv_ms, candidate_ms], oldest first


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
## is fake). Records the candidate offset into the sliding window; advance() moves the applied offset
## toward the resulting target. The first observation initializes the applied offset so now() is
## usable immediately, then refines.
func sample(server_time_ms: int) -> void:
	if server_time_ms <= _last_server_ms:
		return
	_last_server_ms = server_time_ms
	var local := Time.get_ticks_msec()
	_samples.append([float(local), float(server_time_ms - local)])
	# Drop samples that have aged out of the window (keep at least one so the target is always defined).
	var cutoff := float(local) - sample_window_ms
	while _samples.size() > 1 and _samples[0][0] < cutoff:
		_samples.pop_front()
	if not _initialized:
		_offset_ms = _samples[_samples.size() - 1][1]
		_initialized = true


## Min-delay estimate of the true offset: the largest candidate still inside the window.
func _target_ms() -> float:
	var best := -INF
	for s in _samples:
		if s[1] > best:
			best = s[1]
	return best


## Per-frame correction of the applied offset toward the target. `delta` is the frame time in seconds.
## Proportional approach (fraction of remaining error), capped to a bounded slew rate so a large gap
## corrects smoothly rather than dilating render_time; a gap too large to close in reasonable time snaps.
func advance(delta: float) -> void:
	if not _initialized or _samples.is_empty():
		return
	var target := _target_ms()
	var diff := target - _offset_ms
	if absf(diff) > snap_threshold_ms:
		_offset_ms = target
		return
	var step := diff * clampf(correction_rate * delta, 0.0, 1.0)
	var cap := max_slew_ms_per_sec * delta
	_offset_ms += clampf(step, -cap, cap)


## Forget all sync state (session end / reconnect — old offsets are meaningless).
func reset() -> void:
	_offset_ms = 0.0
	_initialized = false
	_last_server_ms = -1
	_samples.clear()
