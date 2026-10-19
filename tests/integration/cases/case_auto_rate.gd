extends "res://test_case.gd"
## Automatic rate and command rate on a slow link both ways: the client alone sets a 3000 B/s link
## on its uplink (sim_config) and its downlink (recv_sim_config), and turns both on. The server
## streams far more than that, and the client sends a command at 60 Hz, which is more packets than
## its uplink carries.
##
## Without them, both queues fill to the sim's 1000 ms buffer and stay there. With them:
##
##   - the rate settles under the link, and the server hears it
##   - the update rate the server grants falls with it, under the server's 30 Hz
##   - the command rate settles under 60
##   - neither link holds a standing queue once they have
##
## The queues are timed from the wall clock, which both processes share: Ent0 carries the server's,
## and each command the client's. goldnet's own lag meters can't be the judge, since a queue full
## from the first packet reads to them as no lag at all.
##
##   t=0.2      server: spawn ENTITIES, moving every frame; client: commands at 60 Hz from now on
##   t<JUDGE_FROM  warmup, the first cuts and the queues draining
##   JUDGE_FROM..JUDGE_TO  both ends sample the delay of what they receive
##   t=ASK_AT   client asks the server for its side: the rate it holds, how often it changed, and
##              the commands' mean delay

const LINK := {"link_bps": 3000}
const ENTITIES := 20
const CMD_RATE := 60.0
const JUDGE_FROM := 18.0
const JUDGE_TO := 26.0
const ASK_AT := 27.0
## Well under the 1000 ms a full sim buffer holds.
const MAX_MEAN_DELAY_MS := 300.0
const WRAP := 100000  # wall ms, folded so a float32 position holds it exactly

var _t := 0.0
var _cmds_sent := 0
var _rate_changes := 0  # server
var _rate_seen := 0     # server
var _delay_sum := 0.0
var _delay_n := 0
var _report: Variant = null
var _clock_seen := -1.0  # client: Ent0's last value, so each arrival is timed once


static func _wall() -> int:
	return int(Time.get_unix_time_from_system() * 1000.0) % WRAP


static func _since(stamp: int) -> int:
	return posmod(_wall() - stamp, WRAP)


func setup(is_server: bool) -> void:
	timeout_s = 45.0
	if is_server:
		return
	goldnet().sim_config = LINK
	goldnet().recv_sim_config = LINK
	goldnet().auto_rate = true
	goldnet().auto_command_rate = true


func server_step(t: float) -> void:
	_t = t
	var peers: PackedInt32Array = main.multiplayer.get_peers()
	if not peers.is_empty():
		var rate: int = goldnet().get_peer_stats(peers[0])["rate"]
		if rate != _rate_seen:
			_rate_seen = rate
			_rate_changes += 1
	if not spawn_once(t, ENTITIES):
		return
	var ents: Dictionary = main.entities()
	for i in ENTITIES:
		var e: Node3D = ents.get("Ent%d" % i)
		if e != null:
			e.position = Vector3(float(_wall()) if i == 0 else float(i), 0.0, t * 10.0)


func on_command(_sender: int, args: Array) -> void:
	if _t >= JUDGE_FROM and _t < JUDGE_TO:
		_delay_sum += _since(int(args[1]))
		_delay_n += 1


func report() -> Variant:
	var peer: int = main.multiplayer.get_peers()[0]
	return {"rate": goldnet().get_peer_stats(peer)["rate"], "changes": _rate_changes,
		"up": _delay_sum / maxi(_delay_n, 1)}


func on_report(data: Variant) -> void:
	_report = data


func client_step(t: float) -> void:
	while t >= 0.2 and _cmds_sent < int((t - 0.2) * CMD_RATE):
		main._case_command.rpc_id(1, _cmds_sent, str(_wall()))
		_cmds_sent += 1
	var clock: Node3D = main.entities().get("Ent0")
	if clock != null and clock.position.x != _clock_seen:
		_clock_seen = clock.position.x
		if t >= JUDGE_FROM and t < JUDGE_TO:
			_delay_sum += _since(int(clock.position.x))
			_delay_n += 1
	if at(t, ASK_AT, "ask"):
		main._case_ask.rpc_id(1)
	if _report == null:
		return
	var stats: Dictionary = goldnet().get_peer_stats(1)
	var rate: int = stats["rate"]
	var cmd_rate: float = stats["command_rate"]
	var down := _delay_sum / maxi(_delay_n, 1)
	print("[client] rate=%d cmd_rate=%.1f mean delay down=%.0f up=%.0f server rate=%d changes=%d"
		% [rate, cmd_rate, down, _report["up"], _report["rate"], _report["changes"]])
	check(rate > 0 and rate <= LINK["link_bps"], "the rate settled under the link (%d)" % rate)
	check(_report["changes"] > 0 and _report["rate"] > 0, "the server applied it (%s)" % _report)
	check(cmd_rate > 0.0 and cmd_rate < CMD_RATE, "the command rate settled under %d (%.1f)" % [CMD_RATE, cmd_rate])
	check(stats["update_rate"] >= 10 and stats["update_rate"] < 30, "the update rate fell with it (%d)" % stats["update_rate"])
	check(down < MAX_MEAN_DELAY_MS, "no standing queue on the downlink (mean %.0f ms)" % down)
	check(_report["up"] < MAX_MEAN_DELAY_MS, "no standing queue on the uplink (mean %.0f ms)" % _report["up"])
	finish()
