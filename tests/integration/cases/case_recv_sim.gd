extends "res://test_case.gd"
## The receive sim: a client alone slows, delays and drops what it receives, with nothing set on
## the server, and changes it live mid-session.
##
## The server moves one entity (unreliable snapshots) and answers each ask with an 8000-byte
## reliable report, so the client can time a reliable round trip and watch the snapshot stream.
##
##   t=1.0  no sim: a round trip is quick
##   t=2.0  150 ms of receive latency: a round trip takes at least that
##   t=3.5  100% receive loss: snapshots stop (the entity freezes), the reliable report still arrives
##   t=5.0  sim off: the entity moves again
##   t=6.0  a 4000 B/s receive link: the 8000-byte report takes about 2 s

const REPORT_BYTES := 8000

var _asked_at := -1.0
var _rtt := -1.0
var _frozen_x := 0.0


func setup(_is_server: bool) -> void:
	timeout_s = 14.0


func server_step(t: float) -> void:
	if not spawn_once(t, 1):
		return
	var e: Node3D = main.entities().get("Ent0")
	if e != null:
		e.position = Vector3(t, 0.0, 0.0)


func report() -> Variant:
	var b := PackedByteArray()
	b.resize(REPORT_BYTES)
	return b


func on_report(data: Variant) -> void:
	check_eq((data as PackedByteArray).size(), REPORT_BYTES, "the reliable report arrives whole")
	_rtt = main._t - _asked_at


func _ask(t: float) -> void:
	_asked_at = t
	_rtt = -1.0
	main._case_ask.rpc_id(1)


func _x() -> float:
	var e: Node3D = main.entities().get("Ent0")
	return e.position.x if e != null else -1.0


func client_step(t: float) -> void:
	if at(t, 0.5, "peer"):
		check(goldnet().multiplayer_peer is ENetMultiplayerPeer, "multiplayer_peer is still the game's own peer")
	if at(t, 1.0, "ask_plain"):
		_ask(t)
	if at(t, 1.8, "judge_plain"):
		check(_rtt >= 0.0 and _rtt < 0.1, "with no sim a round trip is quick (%.3f s)" % _rtt)

	if at(t, 2.0, "latency"):
		goldnet().recv_sim_config = {"latency_min_ms": 150, "latency_max_ms": 150}
		_ask(t)
	if at(t, 3.0, "judge_latency"):
		check(_rtt >= 0.15 and _rtt < 0.35, "150 ms of receive latency delays the round trip (%.3f s)" % _rtt)

	if at(t, 3.5, "loss"):
		goldnet().recv_sim_config = {"loss_percent": 100}
	if at(t, 3.8, "freeze"):
		_frozen_x = _x()
		_ask(t)
	if at(t, 4.8, "judge_loss"):
		check(_frozen_x > 0.0 and is_equal_approx(_x(), _frozen_x),
			"100%% receive loss stops snapshots (x %.2f then %.2f)" % [_frozen_x, _x()])
		check(_rtt >= 0.0, "a reliable report still arrives under 100% receive loss")

	if at(t, 5.0, "off"):
		goldnet().recv_sim_config = {}
	if at(t, 5.8, "judge_off"):
		check(_x() > _frozen_x + 0.5, "with the sim off snapshots resume (x %.2f)" % _x())

	if at(t, 6.0, "link"):
		goldnet().recv_sim_config = {"link_bps": 4000}
		_ask(t)
	if _rtt >= 0.0 and fired("link") and not fired("judge_link"):
		at(t, t, "judge_link")
		check(_rtt >= 1.8 and _rtt < 3.0, "a 4000 B/s link takes about 2 s for 8000 bytes (%.3f s)" % _rtt)
		finish()
	if t > 11.0:
		fail("the report never arrived over the slow link")
		finish()
