extends "res://test_case.gd"
## @clients 2
##
## Per-peer isolation with two clients connected at once.
##
## Everything else in this suite is single-client, which means a whole class of bug is
## invisible to it: state that is supposed to be *per peer* leaking between peers.
## goldnet keeps a `PeerRing` per peer (its own seq counter, baseline ring, spawn_acked
## and spawn_wait sets) and evaluates PVS per peer. If any of that were accidentally
## shared — a single ring, a global spawn_acked, visibility read for the wrong peer —
## every single-client case would still pass.
##
## Setup: two entities, each visible to exactly one client.
##
##   Ent0  visible only to the first peer to connect
##   Ent1  visible only to the second
##
## Each client then asserts it sees exactly ONE entity, and that it's the right one at the
## right position. Seeing two means spawn records aren't tracked per peer; seeing zero
## means the gate is stuck closed.
##
## SCOPE — what this actually covers, established by mutation:
##
## This exercises the per-peer LAZY-SPAWN gate (`_server_tick`'s "defer the spawn until
## the entity is visible to this peer" branch). Disabling that gate fails this case.
##
## It does NOT cover the per-peer gate on the entity *delta*. Disabling that one leaves
## this case passing, because an entity whose spawn never reached a peer can't leak
## through the delta path either — the spawn gate masks it. `pvs_per_peer` covers the
## delta gate, using an entity both peers already have.
##
## Deliberately no client-index coordination: the server assigns entities by connection
## order and each client just checks "I have exactly one, and it's correct". The harness
## does offer `main.peer_for_index()` (pvs_per_peer needs it), but this case doesn't have
## to care which client is which, so it doesn't ask.

const TOL := 0.01
const SETTLE_AT := 4.0
const DEADLINE := 9.0

var _assigned := {}  # peer id -> entity index


func setup(_is_server: bool) -> void:
	timeout_s = 15.0
	required_clients = 2
	public_visibility = false # or the per-peer visibility bit is never consulted


func server_step(t: float) -> void:
	if not spawn_once(t, 2):
		return

	var ents: Dictionary = main.entities()

	# Assign each peer the next entity, in connection order. Stable once assigned.
	for peer in main.multiplayer.get_peers():
		if not _assigned.has(peer):
			if _assigned.size() >= 2:
				continue # more clients than entities — not this case's business
			_assigned[peer] = _assigned.size()
			print("[server] peer %d assigned Ent%d" % [peer, _assigned[peer]])

	for peer in _assigned:
		var mine: int = _assigned[peer]
		for i in 2:
			var e: Node = ents.get("Ent%d" % i)
			if e != null:
				e.get_node("Sync").set_visibility_for(peer, i == mine)

	for i in 2:
		var e: Node3D = ents.get("Ent%d" % i)
		if e != null:
			e.position = home_of(i)


func client_step(t: float) -> void:
	var ents: Dictionary = main.entities()

	if ents.is_empty():
		if t > DEADLINE:
			fail("no entity ever became visible to this peer — the PVS gate never opened")
			finish()
		return

	# Wait past the point where a leaked second entity would have arrived. Asserting the
	# moment the first one lands would pass trivially, since the second may be in flight.
	if t < SETTLE_AT:
		return

	if not check_eq(ents.size(), 1,
			"this peer sees exactly one entity (saw %s — per-peer visibility leaked)" % [ents.keys()]):
		finish()
		return

	var name: String = ents.keys()[0]
	var idx := int(name.substr(3))
	var e: Node3D = ents[name]
	check(idx == 0 or idx == 1, "visible entity is one of the two spawned (%s)" % name)
	check_near(e.position, home_of(idx), TOL, "%s replicated correctly to its own peer" % name)
	print("[client %d] sees %s at %.2v" % [client_index, name, e.position])
	finish()
