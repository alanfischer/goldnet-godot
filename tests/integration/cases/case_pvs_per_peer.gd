extends "res://test_case.gd"
## @clients 2
##
## The per-peer gate on entity DELTAS, as opposed to on spawns.
##
## `multipeer` covers the lazy-spawn gate: an entity never visible to a peer is never
## spawned there. But that gate masks the second one — once an entity HAS been spawned on
## a peer, `_server_tick` still consults visibility per peer before including it in that
## peer's delta. If that read were wrong (global instead of per-peer, or reading the wrong
## peer's bit), a hidden entity would keep streaming to a client that shouldn't see it.
## In a game that's a wallhack: positions of entities you're not supposed to know about.
##
## So this case gives BOTH peers the entity first, then hides it from one of them and
## moves it. The two clients must disagree — which is only possible if the gate is
## genuinely per peer.
##
##   t=0.1  spawn Ent0, visible to both peers
##   t<4    both clients converge to HOME               (both)
##   t=4    hide Ent0 from client 0 only, move to AWAY
##   t≥7    client 0 still at HOME                      (assertion: gate held)
##          client 1 at AWAY                            (assertion: gate didn't over-block)
##
## Both halves are needed. Client 0 alone would pass if the server had simply stopped
## sending to everyone; client 1 alone would pass if the gate did nothing.

const HOME := Vector3(1.0, 0.0, 0.0)
const AWAY := Vector3(77.0, 0.0, 0.0)
const TOL := 0.01

const HIDE_AT := 4.0
const JUDGE_AT := 7.0
const DEADLINE := 11.0

var _hidden := false
var _saw_home := false


func setup(_is_server: bool) -> void:
	timeout_s = 16.0
	required_clients = 2
	public_visibility = false # the server grants the per-peer bit explicitly


func server_step(t: float) -> void:
	if not spawn_once(t, 1):
		return

	var e: Node3D = main.entities().get("Ent0")
	if e == null:
		return
	var sync: MultiplayerSynchronizer = e.get_node("Sync")

	# Both peers get it up front.
	if not _hidden:
		for peer in main.multiplayer.get_peers():
			sync.set_visibility_for(peer, true)
		e.position = HOME

	# Wait for both clients to identify themselves before acting asymmetrically —
	# peer ids alone don't say which process is which.
	if t >= HIDE_AT and not _hidden and main.registered_count() >= 2:
		var target: int = main.peer_for_index(0)
		if target != 0:
			_hidden = true
			sync.set_visibility_for(target, false)
			e.position = AWAY
			print("[server] hid Ent0 from client 0 (peer %d) and moved it to AWAY" % target)


func client_step(t: float) -> void:
	var e: Node3D = main.entities().get("Ent0")

	# Both clients must have it before the split, or neither assertion means anything.
	if not _saw_home:
		if e != null and e.position.distance_to(HOME) <= TOL:
			_saw_home = true
			print("[client %d] converged to HOME at t=%.1f" % [client_index, t])
		elif t > HIDE_AT:
			fail("never converged to HOME before the split (t=%.1f)" % t)
			finish()
		return

	if t < JUDGE_AT:
		return

	if not check(e != null, "Ent0 still present (hiding must not despawn it)"):
		finish()
		return

	if client_index == 0:
		# Hidden from us: the move must not have arrived.
		check_near(e.position, HOME, TOL,
			"client 0 was hidden from Ent0 and did NOT receive its move (a leak here is a wallhack)")
	else:
		# Still visible to us: the move must have arrived.
		check_near(e.position, AWAY, TOL,
			"client 1 still sees Ent0 and received its move (the gate must not over-block)")
	print("[client %d] final position %.2v" % [client_index, e.position])
	finish()
