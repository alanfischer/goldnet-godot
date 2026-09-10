extends "res://test_case.gd"
## push_dirty: it gates the slots a sync DECLARED in gn_push, and nothing else.
##
## Each entity replicates two slots: `position`, declared push-managed, and `scale`, declared
## nothing. From t=1s the server moves both on both entities every tick, but calls mark_dirty on
## Ent0 only. The client asserts three things from t=6s:
##
##   Ent0.position tracks the server      — marking a declared slot replicates it
##   Ent1.position is frozen              — NOT marking a declared slot genuinely withholds it
##   Ent1.scale tracks the server         — an undeclared slot is polled, mark or no mark
##
## The middle assertion is what proves push_dirty is doing anything at all: without it the case
## would pass just as happily if the mode were ignored and everything still polled. The third is
## what makes the mode safe to ship — a game can only lose updates for properties it explicitly
## promised to announce, so forgetting to mark a property it never declared costs a read rather
## than silently desyncing it for every peer. That is why there is no audit mode to run.
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

var _stale_pos := Vector3.ZERO     # client: where Ent1 sat before the server started moving it
var _stale_latched := false


## Two slots with opposite declarations on the same sync, so one entity covers both directions.
func make_entity(data: Variant) -> Node:
	var n := Node3D.new()
	n.name = "Ent%d" % int(data)
	var cfg := SceneReplicationConfig.new()
	for p in [^".:position", ^".:scale"]:
		cfg.add_property(p)
		cfg.property_set_sync(p, true)
	var sync := MultiplayerSynchronizer.new()
	sync.name = "Sync"
	sync.replication_config = cfg
	sync.replication_interval = 1.0 / 30.0
	sync.set_visibility_public(public_visibility)
	# position is the game's promise to announce; scale is deliberately undeclared and so polled.
	sync.set_meta("gn_push", ["position"])
	n.add_child(sync)
	return n


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
	# Same motion for both, on both slots; only Ent0 is announced.
	var offset := Vector3(0.0, sin(t) * 2.0, 0.0)
	# Monotone, unlike the position wobble: the check below is "did it leave 1.0 at all", and a
	# sinusoid would be back at its start on the sample frame roughly half the time.
	var s := 1.0 + t * 0.1
	e0.position = home_of(MARKED) + offset
	e1.position = home_of(UNMARKED) + offset
	e0.scale = Vector3.ONE * s
	e1.scale = Vector3.ONE * s
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
			"unmarked entity's DECLARED slot stayed at the position it last replicated")
	# The undeclared slot was never marked either, and must have arrived anyway. Scale starts at
	# ONE and the server only ever ramps it upward, so any departure from ONE can only be a poll.
	check(absf(e1.scale.x - 1.0) > 0.05,
			"unmarked entity's UNDECLARED slot kept updating (polled, not gated by the mark)")
	finish()
