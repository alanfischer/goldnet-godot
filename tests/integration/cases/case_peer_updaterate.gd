extends "res://test_case.gd"
## @clients 2
##
## Per-peer snapshot cadence (GoldSrc cl_updaterate): one client asks to be served slower
## than the server ticks, and that must throttle ONLY that client.
##
## The server ticks at the fastest replication_interval among its synchronizers (30Hz here).
## set_peer_snapshot_interval_ms subsamples which peers a given tick serves, so the throttled
## client should receive roughly interval/tick as many snapshots as the other one. The client
## counts applied snapshots via the synchronizer's `synchronized` signal, which goldnet
## re-emits once per delivered frame that changed something — a frame-rate-independent counter
## (sampling positions per frame would alias against the send rate instead).
##
## The second half matters as much as the first: a throttled peer must still CONVERGE. Every
## frame it does receive deltas against its own older acked baseline, which is the same path a
## peer takes after a lost snapshot. If throttling broke that, the client would drift or freeze.
##
##   t=0.1   spawn Ent0; server moves it every tick so every snapshot carries a change
##   t=3     throttle client 0 to 200ms (client 1 left alone)
##   t=4..8  both clients count applied snapshots
##   t=9     server pins Ent0 to FINAL and stops moving
##   t=11    client 0: count is throttled but non-zero, and it converged to FINAL
##           client 1: count is full-rate, and it converged to FINAL

const THROTTLE_MS := 200
const FINAL := Vector3(42.0, 0.0, 0.0)
const TOL := 0.01

const THROTTLE_AT := 3.0
const MEASURE_FROM := 4.0
const MEASURE_TO := 8.0
const PIN_AT := 9.0
const JUDGE_AT := 11.0

# Over the 4s window at a 30Hz server tick: ~120 snapshots unthrottled, ~20 at 200ms.
# The bands are wide enough to absorb scheduling noise and still leave the two cases
# nowhere near each other — an unthrottled client cannot land under THROTTLED_MAX.
const THROTTLED_MIN := 5
const THROTTLED_MAX := 45
const FULL_MIN := 60

var _count := 0
var _counting := false
var _connected := false


func setup(_is_server: bool) -> void:
	timeout_s = 20.0
	required_clients = 2


func server_step(t: float) -> void:
	if not spawn_once(t, 1):
		return
	var e: Node3D = main.entities().get("Ent0")
	if e == null:
		return

	# Wait for both clients to identify themselves — peer ids alone don't say which
	# process is which, and this case is deliberately asymmetric.
	if t >= THROTTLE_AT and not fired("throttled") and main.registered_count() >= 2:
		var target: int = main.peer_for_index(0)
		if target != 0 and at(t, THROTTLE_AT, "throttled"):
			goldnet().set_peer_snapshot_interval_ms(target, THROTTLE_MS)
			print("[server] throttled client 0 (peer %d) to %dms" % [target, THROTTLE_MS])

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

	# Both must land on FINAL: throttling delays frames, it must not desync the baseline.
	check_near(e.position, FINAL, TOL,
		"client %d converged after the throttle window" % client_index)
	print("[client %d] applied %d snapshots in the window, final %.2v" % [client_index, _count, e.position])
	finish()


func _on_synchronized() -> void:
	if _counting:
		_count += 1
