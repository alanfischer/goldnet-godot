extends "res://test_case.gd"
## The server-time feed survives an idle world.
##
## Every snapshot header carries the server's send-time, and the client re-emits it as
## `server_time_received` — the feed ServerClock estimates the render timeline from. goldnet
## advertises that as always-on precisely because it does NOT depend on entity traffic: a
## client with nothing moving in view still needs a clock.
##
## `_server_tick` skips snapshots with nothing to report, which is worth real bytes on an
## idle map but points straight at that guarantee — the world being static is exactly when
## the skip engages and exactly when no other packet will carry the time. So the skip is
## bounded by GN_CLOCK_KEEPALIVE_MS: past it, an otherwise-empty snapshot goes out anyway.
##
## Without the bound the failure is quiet and delayed. The clock does not stop — ServerClock
## keeps its last offset and free-runs on local time — it just stops being *corrected*, and
## drifts by however far the two machines' clocks disagree. Nothing logs, nothing throws;
## interpolation gradually samples the wrong instant.
##
## Timeline (server drives state, client asserts):
##
##   t=0.1        spawn one entity, park it, never touch it again
##   t<WINDOW_FROM  it converges; from here the world is fully static
##   t=4..8       client counts clock samples AND position changes
##   t≥JUDGE_AT   position changes == 0        → assertion 1 (the world really was idle)
##                clock samples >= MIN_SAMPLES → assertion 2 (the feed ran anyway)
##
## Assertion 1 is what makes assertion 2 mean anything: if the entity were still being
## updated, ordinary snapshots would carry the clock and the case would pass with the
## keepalive deleted. Verified by mutation — dropping the keepalive bound takes assertion 2
## to zero samples while assertion 1 still holds.

const HOME := Vector3(3.0, 0.0, 0.0)

const WINDOW_FROM := 4.0
const WINDOW_TO := 8.0
const JUDGE_AT := 8.0

## GN_CLOCK_KEEPALIVE_MS is 250, so a 4 s window should see ~15. Asserting half that leaves
## room for scheduling slop without leaving room for "the feed stopped".
const MIN_SAMPLES := 8

var _samples := 0
var _moves := 0
var _last_pos = null
var _connected_signal := false


func setup(is_server: bool) -> void:
	timeout_s = 20.0
	if not is_server:
		goldnet().server_time_received.connect(func(_ms: int): _samples += 1)
		_connected_signal = true


func server_step(t: float) -> void:
	if not spawn_once(t, 1):
		return
	# Written once. After the client acks it there is nothing left to report, which is the
	# state this case is about — every later snapshot is a keepalive or nothing at all.
	if at(t, SPAWN_GRACE_S, "park"):
		var e: Node3D = main.entities().get("Ent0")
		if e != null:
			e.position = HOME


func client_step(t: float) -> void:
	var e: Node3D = main.entities().get("Ent0")

	if t >= WINDOW_FROM and t < WINDOW_TO:
		if _samples_window_start < 0:
			_samples_window_start = _samples
		if e != null:
			if _last_pos != null and _last_pos != e.position:
				_moves += 1
			_last_pos = e.position

	if t < JUDGE_AT:
		return

	if not check(e != null, "the entity arrived"):
		finish()
		return
	check(_connected_signal, "client hooked server_time_received")

	var in_window := _samples - _samples_window_start
	print("[client] clock samples in window=%d  position changes=%d" % [in_window, _moves])

	# 1. The world was genuinely idle across the window...
	check_eq(_moves, 0,
		"no entity traffic during the window (%d position changes — if this is nonzero the "
		% _moves + "case is measuring ordinary snapshots, not the keepalive)")
	# ...2. so any clock sample at all had to come from a keepalive.
	check(in_window >= MIN_SAMPLES,
		"clock feed kept running while idle (%d samples >= %d — zero means the skip silenced "
		% [in_window, MIN_SAMPLES] + "server_time_received and ServerClock is free-running)")
	finish()


## Samples already counted when the window opened; the connect above starts counting at
## t=0, and the convergence traffic before the window is not what this case is measuring.
var _samples_window_start := -1
