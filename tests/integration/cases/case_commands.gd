extends "res://test_case.gd"
## Command streams: a client's calls to a marked @rpc reach the server once each, in order,
## under loss both ways and a blackout, the way a reliable RPC would.
##
## The client sends at 60 Hz, alternating the two marked RPCs so ordering is checked across
## streams and both payload forms (ordinary arguments, a lone PackedByteArray) are covered.
## The server records what arrived and who get_remote_sender_id() said sent it, and reports
## back once the client has stopped.
##
## The commands are declared reliable, so if goldnet failed to carry them they'd still
## arrive through the inner and the order check alone would pass. The command stats are
## the evidence goldnet carried them: commands were unacked for a while, then all acked.
##
##   t=0.2-4.2  client sends 240 commands at 60 Hz, 30% loss each way, 30 ms each way
##   t=1.5-2.0  client blackout (100% loss), 30 commands: under COMMAND_RING
##   t=5.0      client asks for the report; the server sends what it received

const RATE := 60.0
const START := 0.2
const COUNT := 240
const BLACKOUT_START := 1.5
const BLACKOUT_END := 2.0
const ASK_AT := 5.0
const LOSSY := {"loss_percent": 30, "latency_min_ms": 30, "latency_max_ms": 30, "seed": 11}

var _sent := 0
var _max_pending := 0
var _received: Array = []  # server: [sender, n, sender_matches_peer]
var _report: Variant = null


func setup(_is_server: bool) -> void:
	timeout_s = 12.0
	goldnet().sim_config = LOSSY


func on_command(sender: int, args: Array) -> void:
	var n: int
	if args.size() == 2:
		n = args[0]
		check_eq(args[1], "c%d" % n, "argument form carries its arguments")
	else:
		var b: PackedByteArray = args[0]
		n = b.decode_u16(0)
		check_eq(b.size(), 6, "byte form carries its bytes")
	_received.append(n)
	check(sender > 1, "get_remote_sender_id() names the client (got %d)" % sender)


func server_step(_t: float) -> void:
	pass




func client_step(t: float) -> void:
	if at(t, BLACKOUT_START, "blackout_on"):
		goldnet().sim_config = {"loss_percent": 100}
	if at(t, BLACKOUT_END, "blackout_off"):
		goldnet().sim_config = LOSSY
	while _sent < COUNT and t >= START + _sent / RATE:
		if _sent % 2 == 0:
			main._case_command.rpc_id(1, _sent, "c%d" % _sent)
		else:
			var b := PackedByteArray([0, 0, 7, 7, 7, 7])
			b.encode_u16(0, _sent)
			main._case_raw.rpc_id(1, b)
		_sent += 1
	_max_pending = maxi(_max_pending, goldnet().get_command_stats()["pending"])
	if at(t, ASK_AT, "ask"):
		main._case_ask.rpc_id(1)
	if _report != null:
		var got: Array = _report
		check_eq(got.size(), COUNT, "every command arrived once")
		var in_order := true
		for i in got.size():
			if got[i] != i:
				in_order = false
				break
		check(in_order, "commands arrived in the order they were made")
		var stats: Dictionary = goldnet().get_command_stats()
		check(_max_pending > 10, "goldnet carried them: up to %d unacked at once" % _max_pending)
		check_eq(stats["pending"], 0, "all acked by the end")
		check_eq(stats["dropped"], 0, "none given up on")
		check(stats["rtt_ms"] >= 50, "the command round trip includes both legs (%d ms)" % stats["rtt_ms"])
		finish()


func on_report(data: Variant) -> void:
	_report = data


func report() -> Array:
	return _received
