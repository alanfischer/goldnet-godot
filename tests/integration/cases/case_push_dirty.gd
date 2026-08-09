extends "res://test_case.gd"
## push_dirty: with polling off, a marked entity replicates and an UNMARKED one goes stale.
##
## Timeline: the server enables push_dirty at t=0, spawns two entities, and from t=1s moves both
## every tick — but calls mark_dirty on Ent0 only. The client asserts from t=6s that Ent0 tracks
## the server while Ent1 is stuck at the last position it saw before the moves began.
##
## The negative half is the point. push_dirty trades a poll for the game's promise that it marks
## every write, and the failure mode of a missed mark is silence — the entity simply stops
## updating. Asserting only that marking works would pass just as happily if push_dirty were
## ignored entirely and everything were still polled, so this pins BOTH directions: marking
## replicates, and not marking genuinely doesn't. (goldnet's `dirty_audit` is the tool that
## catches an unmarked write in a real game; this case is what proves there is something to
## catch.)
##
## Scope: covers the read gate only. It says nothing about WHICH slots a marked entity sends —
## that is the delta mask, covered by convergence and ring_expiry.

const TOL := 0.01
const START_MOVE_T := 1.0
## When the client samples where the unmarked entity came to rest. Deliberately well after the
## server stops marking it rather than at the boundary: the two processes keep independent clocks
## and a snapshot already in flight would otherwise land after the sample and read as a live
## update. Latching late means anything still arriving has long since been applied.
const LATCH_T := 3.0
const MARKED := 0
const UNMARKED := 1

var _stale_pos := Vector3.ZERO   # client: where Ent1 sat before the server started moving it
var _stale_latched := false


func setup(_is_server: bool) -> void:
	timeout_s = 20.0
	if _is_server and main.multiplayer.has_method(&"set_push_dirty"):
		main.multiplayer.set_push_dirty(true)


func server_step(t: float) -> void:
	if not spawn_once(t, 2):
		return
	var ents: Dictionary = main.entities()
	var e0: Node3D = ents.get("Ent0")
	var e1: Node3D = ents.get("Ent1")
	if e0 == null or e1 == null:
		return
	if t < START_MOVE_T:
		# Both parked on their home spot, both marked, so the client has a baseline for each.
		e0.position = home_of(MARKED)
		e1.position = home_of(UNMARKED)
		_mark(e0)
		_mark(e1)
		return
	# Same motion for both; only Ent0 is announced.
	var offset := Vector3(0.0, sin(t) * 2.0, 0.0)
	e0.position = home_of(MARKED) + offset
	e1.position = home_of(UNMARKED) + offset
	_mark(e0)


func _mark(node: Node) -> void:
	if main.multiplayer.has_method(&"mark_dirty"):
		main.multiplayer.mark_dirty(node)


func client_step(t: float) -> void:
	var ents: Dictionary = main.entities()
	var e0: Node3D = ents.get("Ent0")
	var e1: Node3D = ents.get("Ent1")
	if e0 == null or e1 == null:
		if t > 10.0:
			fail("entities did not arrive within 10s (got %d)" % ents.size())
			finish()
		return
	# Latch where the unmarked entity sat while it was still being marked — that is the position
	# it must remain frozen at once the server stops announcing it.
	if not _stale_latched and t > LATCH_T:
		_stale_latched = true
		_stale_pos = e1.position
	if t < LATCH_T + 3.0:
		return

	check(e0.position.distance_to(home_of(MARKED)) > 0.5,
			"marked entity moved away from its parked position")
	check_near(e1.position, _stale_pos, TOL,
			"unmarked entity stayed at the position it last replicated")
	finish()
