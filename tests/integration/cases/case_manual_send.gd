extends "res://test_case.gd"
## manual_send: the game, not goldnet's timer, decides when snapshots go out.
##
## With manual_send on, goldnet's own clock sends nothing; each send_snapshots() sends one snapshot
## to every due peer. A game calls it once per server tick, after writing that tick's state, so the
## stream is locked to its simulation instead of drifting against it.
##
## Timeline (server drives state, client asserts):
##
##   t<ARRIVE_BY     manual on; send every frame until the entity has arrived
##   ..PACED_FROM    the entity moves every frame, but nothing calls send_snapshots
##                   → client sees no update at all: the timer really is off
##   ..PACED_TO      send_snapshots exactly every SEND_EVERY s while the entity moves every frame
##                   → client sees about one update per call, not the timer's 30 Hz
##   t≥PIN_AT        pin FINAL and send every frame → the client converges
##
## The server writes its own time into the entity's x, so the client tells which phase an update
## was sent in from its content — the two processes' clocks don't line up.

const ARRIVE_BY := 1.0
const PACED_FROM := 3.0
const PACED_TO := 6.0
const SEND_EVERY := 0.2
const PIN_AT := 6.5
const JUDGE_AT := 8.0
const FINAL := Vector3(7.0, 0.0, 0.0)

## 15 sends in the paced window. The band is loose for scheduling slop; the 30 Hz timer would
## deliver ~90 and can't land in it.
const PACED_MIN := 10
const PACED_MAX := 17

var _next_send := PACED_FROM
var _quiet := 0
var _paced := 0
var _connected := false
var _ent: Node3D = null


func setup(is_server: bool) -> void:
	timeout_s = 20.0
	if is_server:
		goldnet().manual_send = true


func server_step(t: float) -> void:
	if not spawn_once(t, 1):
		goldnet().send_snapshots()
		return
	var e: Node3D = main.entities().get("Ent0")
	if e == null:
		return
	e.position = FINAL if t >= PIN_AT else Vector3(t, 0.0, 0.0)
	if t < ARRIVE_BY or t >= PIN_AT:
		goldnet().send_snapshots()
	elif t >= PACED_FROM and t < PACED_TO and t >= _next_send:
		_next_send += SEND_EVERY
		goldnet().send_snapshots()


func client_step(t: float) -> void:
	var e: Node3D = main.entities().get("Ent0")
	if e == null:
		if t > ARRIVE_BY + 1.0:
			fail("Ent0 never arrived (t=%.1f)" % t)
			finish()
		return
	if not _connected:
		_ent = e
		e.get_node("Sync").synchronized.connect(_on_synchronized)
		_connected = true
	if t < JUDGE_AT:
		return
	check_eq(_quiet, 0, "no snapshot reached the client while nothing called send_snapshots")
	check(_paced >= PACED_MIN and _paced <= PACED_MAX,
			"one update per send_snapshots call while paced (%d in [%d, %d])" % [_paced, PACED_MIN, PACED_MAX])
	check_near(e.position, FINAL, 0.01, "converged once sends resumed")
	print("[client] quiet %d, paced %d, final %.2v" % [_quiet, _paced, e.position])
	finish()


func _on_synchronized() -> void:
	var sent_at := _ent.position.x  # the server's time when it wrote this
	if sent_at > ARRIVE_BY + 0.05 and sent_at < PACED_FROM:
		_quiet += 1
	elif sent_at >= PACED_FROM and sent_at < PACED_TO:
		_paced += 1
