extends "res://test_case.gd"
## Per-peer PVS: relevance loss, re-entry, and lazy spawning.
##
## goldnet gates each entity per peer on the synchronizer's visibility (`_server_tick`
## reads `is_visibility_public() || get_visibility_for(peer)`). Two behaviors hang off
## that gate, and both have bitten this codebase before:
##
##   * An entity that leaves a peer's PVS must raise `entity_relevance_lost` on that
##     client, which is how the game knows to hide it. Without the signal the client
##     keeps rendering a frozen copy — the "running in place" corpse.
##   * An entity that has NEVER been visible to a peer must not be spawned on it at all
##     (lazy spawn). Spawning eagerly is what made goldnet's bandwidth blow up.
##
## Timeline:
##
##   t=0.1  spawn Ent0 (visible to the peer) and Ent1 (never visible)
##   t<3    client converges to Ent0's HOME, and Ent1 never appears   (assertions 1, 2)
##   t=3    server hides Ent0 → client raises entity_relevance_lost   (assertion 3)
##   t=3.5  server moves Ent0 while it is out of PVS
##   t=5    server re-shows Ent0 → client converges to the new spot   (assertion 4)
##
## Assertion 4 doubles as a re-entry correctness check: the move happened entirely while
## the entity was invisible, so the client can only be right if re-entry sent full state.

const HOME := Vector3(1.0, 0.0, 0.0)
const AWAY := Vector3(42.0, 0.0, 0.0)
const TOL := 0.01

const HIDE_AT := 3.0
const MOVE_AT := 3.5
const SHOW_AT := 5.0
const DEADLINE := 9.0

var _saw_home := false
var _lost_fired := false
var _lost_checked := false


func setup(is_server: bool) -> void:
	timeout_s = 15.0
	# Ent0 is per-peer gated; Ent1 is gated the same way but never granted to anyone, so
	# it exercises the lazy-spawn path.
	public_visibility = false
	if is_server:
		# Relevance events are OPT-IN and default off: with them disabled the server emits
		# no leave section at all (leave_ct 0, 2 bytes), so games that don't want the
		# feature pay nothing. Forgetting this reads exactly like a broken PVS.
		goldnet().relevance_events = true
	else:
		goldnet().connect("entity_relevance_lost", _on_relevance_lost)


func _on_relevance_lost(_sync: Object) -> void:
	_lost_fired = true


func server_step(t: float) -> void:
	if not spawn_once(t, 2):
		return

	var peers := main.multiplayer.get_peers()
	if peers.is_empty():
		return
	var peer: int = peers[0]

	var e0: Node3D = main.entities().get("Ent0")
	if e0 == null:
		return
	var s0: MultiplayerSynchronizer = e0.get_node("Sync")

	# Ent1 is deliberately never made visible to anyone.
	if not fired("hide"):
		s0.set_visibility_for(peer, true)
	if not fired("move"):
		e0.position = HOME

	if at(t, HIDE_AT, "hide"):
		s0.set_visibility_for(peer, false)
		print("[server] Ent0 hidden from peer %d" % peer)
	if at(t, MOVE_AT, "move"):
		e0.position = AWAY
		print("[server] moved Ent0 while out of PVS")
	if at(t, SHOW_AT, "show"):
		s0.set_visibility_for(peer, true)
		print("[server] Ent0 shown again")


func client_step(t: float) -> void:
	var ents: Dictionary = main.entities()

	# 1) The visible entity arrives and converges.
	if not _saw_home:
		var e0: Node3D = ents.get("Ent0")
		if e0 != null and e0.position.distance_to(HOME) <= TOL:
			_saw_home = true
			print("[client] Ent0 at HOME at t=%.1f" % t)
		elif t > HIDE_AT:
			fail("Ent0 never converged to HOME while visible (t=%.1f)" % t)
			finish()
		return

	# 2) The never-visible entity was never spawned here. Checked after Ent0 has arrived,
	#    so we know the stream is flowing and this isn't just "nothing arrived yet".
	if not _lost_checked and t < HIDE_AT + 0.5:
		check(not ents.has("Ent1"), "Ent1 was never visible to this peer, so it was never spawned")
		_lost_checked = true
		return

	# 3) Losing relevance raises the signal the game hides entities on.
	if not _lost_fired:
		if t > SHOW_AT:
			fail("Ent0 left this peer's PVS at t=%.1f but entity_relevance_lost never fired" % HIDE_AT)
			finish()
		return

	# 4) Re-entry delivers the state changed while we were blind to it.
	var e0b: Node3D = ents.get("Ent0")
	if e0b != null and e0b.position.distance_to(AWAY) <= TOL:
		check(true, "entity_relevance_lost fired when Ent0 left the PVS")
		check(true, "Ent0 re-entered the PVS with the state it changed to while invisible")
		finish()
	elif t > DEADLINE:
		fail("Ent0 re-entered the PVS but never reached AWAY (last position %.3v)"
			% (e0b.position if e0b != null else Vector3.ZERO))
		finish()
