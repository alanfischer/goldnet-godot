extends "res://test_case.gd"
## Ordinary RPCs on the netchan: goldnet carries every call between the server and a client in its
## own packets, as GoldSrc's netchan carried user messages. Under loss both ways:
##
##   - reliable calls arrive once each, in order, both directions
##   - unreliable calls arrive only in increasing order: a late packet's never play
##   - a call bigger than a reliable block arrives whole, cut into fragments on the way
##   - call_local still runs on the server, and get_remote_sender_id() names the caller
##
## The message stats are the evidence goldnet carried them: the inner would deliver the same calls.
##
##   t=0.2-2.2  server: 120 reliable and 120 unreliable calls at 60 Hz; client: 60 of each up, 30 Hz
##   t=0.3      server: one call_local call
##   t=0.5      server: one 6 KB call (8 fragments)
##   t=7.0      client asks for the report

const LOSSY := {"loss_percent": 30, "latency_min_ms": 20, "latency_max_ms": 20, "seed": 5}
const START := 0.2
const DOWN_COUNT := 120
const UP_COUNT := 60
const BIG := 6000
const ASK_AT := 7.0

var _down_sent := 0
var _up_sent := 0
var _rel: Array = []
var _unrel: Array = []
var _big: PackedByteArray = PackedByteArray()
var _local: Array = []
var _up: Array = []        # server
var _up_unrel: Array = []  # server
var _report: Variant = null


func setup(_is_server: bool) -> void:
	timeout_s = 14.0
	goldnet().sim_config = LOSSY


static func _big_bytes() -> PackedByteArray:
	var b := PackedByteArray()
	b.resize(BIG)
	for i in BIG:
		b[i] = (i * 13) % 251
	return b


func on_message(sender: int, kind: String, args: Array) -> void:
	match kind:
		"rel":
			check_eq(args[1], "m%d" % args[0], "a reliable call carries its arguments")
			_rel.append(args[0])
		"unrel":
			_unrel.append(args[0])
		"big":
			_big = args[0]
		"local":
			_local.append([sender, args[0]])
		"up":
			check_eq(args[1], args[0] * 0.1, "a float argument arrives exactly")
			check(sender > 1, "an uplink call names its client (got %d)" % sender)
			_up.append(args[0])
		"up_unrel":
			_up_unrel.append(args[0])
	if kind in ["rel", "unrel", "big"]:
		check_eq(sender, 1, "a server call names the server")


func server_step(t: float) -> void:
	while _down_sent < DOWN_COUNT and t >= START + _down_sent / 60.0:
		main._case_msg_rel.rpc(_down_sent, "m%d" % _down_sent)
		main._case_msg_unrel.rpc(_down_sent)
		_down_sent += 1
	if at(t, 0.3, "local"):
		main._case_msg_local.rpc(7)
	if at(t, 0.5, "big"):
		main._case_msg_big.rpc(_big_bytes())


func client_step(t: float) -> void:
	while _up_sent < UP_COUNT and t >= START + _up_sent / 30.0:
		main._case_msg_up.rpc_id(1, _up_sent, _up_sent * 0.1)
		main._case_msg_up_unrel.rpc_id(1, _up_sent)
		_up_sent += 1
	if at(t, ASK_AT, "ask"):
		main._case_ask.rpc_id(1)
	if _report == null:
		return
	var r: Dictionary = _report
	check_eq(_rel, range(DOWN_COUNT), "every reliable call arrived once, in order")
	check(_unrel.size() > DOWN_COUNT / 3, "most unreliable calls arrived (%d)" % _unrel.size())
	check(_unrel.size() < DOWN_COUNT, "and some were lost, as they may be (%d)" % _unrel.size())
	check(_increasing(_unrel), "unreliable calls only ever arrive newer than the last")
	check(_big == _big_bytes(), "the big call arrived whole (%d bytes)" % _big.size())
	check_eq(_local, [[1, 7]], "the client got the call_local call once")
	check_eq(r["local"], [[1, 7]], "the server ran its own copy once")
	check_eq(r["up"], range(UP_COUNT), "every reliable uplink call arrived once, in order")
	check(_increasing(r["up_unrel"]), "unreliable uplink calls only ever arrive newer than the last")
	var down: Dictionary = r["stats"]
	var up: Dictionary = goldnet().get_message_stats(1)
	check(down["delivered"] >= DOWN_COUNT + 2, "goldnet carried the server's calls (%s)" % down)
	check_eq(down["delivered"], down["queued"], "and all of them are acked")
	check(up["delivered"] >= UP_COUNT, "goldnet carried the client's calls (%s)" % up)
	finish()


static func _increasing(a: Array) -> bool:
	for i in range(1, a.size()):
		if a[i] <= a[i - 1]:
			return false
	return true


func on_report(data: Variant) -> void:
	_report = data


func report() -> Variant:
	var peer: int = main.multiplayer.get_peers()[0]
	return {"up": _up, "up_unrel": _up_unrel, "local": _local, "stats": goldnet().get_message_stats(peer)}
