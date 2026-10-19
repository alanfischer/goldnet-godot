extends "res://test_case.gd"
## @clients 2
##
## Per-peer snapshot cadence (GoldSrc cl_updaterate): one client asks to be served slower
## than the server ticks, and that must throttle ONLY that client.
##
## The server ticks at the fastest replication_interval among its synchronizers (30Hz here).
## A client's update_rate rides its uplink, and the server serves it every nth tick, so the
## throttled client should receive about a third as many snapshots as the other one, and hear
## what it was granted through update_rate_granted. The client
## counts applied snapshots via the synchronizer's `synchronized` signal, which goldnet
## re-emits once per delivered frame that changed something — a frame-rate-independent counter
## (sampling positions per frame would alias against the send rate instead).
##
## The second half matters as much as the first: a throttled peer must still CONVERGE. Every
## frame it does receive deltas against its own older acked baseline, which is the same path a
## peer takes after a lost snapshot. If throttling broke that, the client would drift or freeze.
##
##   t=0.1   spawn Ent0; server moves it every tick so every snapshot carries a change
##   t=3     client 0 asks for 10 Hz (client 1 asks for nothing)
##   t=4..8  both clients count applied snapshots
##   t=9     server pins Ent0 to FINAL and stops moving
##   t=11    client 0: count is throttled but non-zero, it was granted 10, and it converged to FINAL
##           client 1: count is full-rate, it was granted 30, and it converged to FINAL

const ASK_HZ := 10
const SERVER_HZ := 30
const FINAL := Vector3(42.0, 0.0, 0.0)
const TOL := 0.01

const THROTTLE_AT := 3.0
const MEASURE_FROM := 4.0
const MEASURE_TO := 8.0
const PIN_AT := 9.0
const JUDGE_AT := 11.0

# Over the 4s window at a 30Hz server tick: ~120 snapshots unthrottled, ~40 at 10 Hz.
# The bands are wide enough to absorb scheduling noise and still leave the two cases
# nowhere near each other — an unthrottled client cannot land under THROTTLED_MAX.
const THROTTLED_MIN := 20
const THROTTLED_MAX := 70
const FULL_MIN := 90

var _count := 0
var _counting := false
var _connected := false
var _granted := -1


func setup(is_server: bool) -> void:
	timeout_s = 20.0
	required_clients = 2
	if not is_server:
		goldnet().update_rate_granted.connect(func(hz: int) -> void: _granted = hz)


func server_step(t: float) -> void:
	if not spawn_once(t, 1):
		return
	var e: Node3D = main.entities().get("Ent0")
	if e == null:
		return

	# Move every tick before the pin so each snapshot genuinely changes state, then hold
	# still so both clients have something fixed to converge to.
	e.position = FINAL if t >= PIN_AT else Vector3(t, 0.0, 0.0)


func client_step(t: float) -> void:
	var e: Node3D = main.entities().get("Ent0")
	if e == null:
		if t > THROTTLE_AT:
			fail("Ent0 never arrived (t=%.1f)" % t)
			finish()
		return

	if not _connected:
		var sync: MultiplayerSynchronizer = e.get_node("Sync")
		sync.synchronized.connect(_on_synchronized)
		_connected = true

	if client_index == 0 and at(t, THROTTLE_AT, "throttled"):
		goldnet().update_rate = ASK_HZ

	_counting = t >= MEASURE_FROM and t < MEASURE_TO

	if t < JUDGE_AT:
		return

	if client_index == 0:
		check(_count >= THROTTLED_MIN,
			"throttled client kept receiving snapshots (%d >= %d)" % [_count, THROTTLED_MIN])
		check(_count <= THROTTLED_MAX,
			"throttled client received FEWER than the server ticked (%d <= %d)" % [_count, THROTTLED_MAX])
	else:
		check(_count >= FULL_MIN,
			"unthrottled client still ran at full rate (%d >= %d) — the gate must be per peer"
				% [_count, FULL_MIN])
	var want := ASK_HZ if client_index == 0 else SERVER_HZ
	check(_granted == want, "client %d heard its grant (%d == %d)" % [client_index, _granted, want])
	check(goldnet().get_peer_stats(1)["update_rate"] == want, "and its stats agree")

	# Both must land on FINAL: throttling delays frames, it must not desync the baseline.
	check_near(e.position, FINAL, TOL,
		"client %d converged after the throttle window" % client_index)
	print("[client %d] applied %d snapshots in the window, final %.2v" % [client_index, _count, e.position])
	finish()


func _on_synchronized() -> void:
	if _counting:
		_count += 1
