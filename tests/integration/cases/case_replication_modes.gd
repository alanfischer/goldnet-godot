extends "res://test_case.gd"
## ON_CHANGE (watch) properties replicate, in a mixed config and on their own.
##
## goldnet used to read only `property_get_sync()`, which Godot sets for ALWAYS alone, so an
## ON_CHANGE property never reached the wire — in EITHER config shape, for two reasons:
##
##   MIXED           intercepted (an ALWAYS prop is present), then the slot walk dropped
##                   the ON_CHANGE ones -> goldnet skipped them and the inner never saw
##                   the synchronizer
##   all ON_CHANGE   not intercepted, so it fell through to the inner SceneMultiplayer —
##                   which cannot replicate it either, because goldnet owns the spawner
##                   (Phase 3) and the inner has no path cache for a node it never spawned
##                   ("Node not found: Main/Ent1/Sync")
##
## Both were silent: no error on the sending side, and it works on stock, so it reads as a
## game bug. Mixed is the shape a game reaches by accident — add one editor-default
## ON_CHANGE property to a synchronizer that already streams a transform.
##
## Ent0 is mixed, Ent1 is all-ON_CHANGE; both must replicate through goldnet now.
##
## Timeline:
##
##   t=0.1  spawn Ent0/Ent1, server writes the initial values
##   t=3.0  client asserts every replicated slot arrived                     (phase A)
##   t=4.0  server rewrites only the ON_CHANGE slots
##   t=7.0  client asserts the new values arrived, and that the ALWAYS slot
##          still holds its phase-A value from the baseline                 (phase B)
##
## Phase B matters twice: it exercises the delta path for a watch slot, and a watch slot
## that only ever arrived in the spawn payload would pass phase A and fail here.

const Fixture := preload("res://cases/modes_entity.gd")

const ALWAYS_A := 11
const CHANGE_A := 22
const SPAWN_A := 33
const CHANGE_B := 99

const PHASE_A_AT := 3.0
const WRITE_B_AT := 4.0
const PHASE_B_AT := 7.0

var _phase_a_done := false


## Ent0 mixes ALWAYS + ON_CHANGE + NEVER; Ent1 carries only the ON_CHANGE slot.
func make_entity(data: Variant) -> Node:
	var idx := int(data)
	var n := Node3D.new()
	n.set_script(Fixture)
	n.name = "Ent%d" % idx

	var cfg := SceneReplicationConfig.new()
	var modes := {
		"change_val": SceneReplicationConfig.REPLICATION_MODE_ON_CHANGE,
	}
	if idx == 0:
		modes["always_val"] = SceneReplicationConfig.REPLICATION_MODE_ALWAYS
		modes["spawn_val"] = SceneReplicationConfig.REPLICATION_MODE_NEVER
	for slot in modes:
		var p := NodePath(".:%s" % slot)
		cfg.add_property(p)
		cfg.property_set_replication_mode(p, modes[slot])

	var sync := MultiplayerSynchronizer.new()
	sync.name = "Sync"
	sync.replication_config = cfg
	sync.replication_interval = 1.0 / 30.0
	sync.set_visibility_public(public_visibility)
	n.add_child(sync)
	return n


func setup(_is_server: bool) -> void:
	timeout_s = 16.0


func server_step(t: float) -> void:
	if not spawn_once(t, 2):
		return
	var e0: Node = main.entities().get("Ent0")
	var e1: Node = main.entities().get("Ent1")
	if e0 == null or e1 == null:
		return

	if not fired("write_b"):
		e0.always_val = ALWAYS_A
		e0.change_val = CHANGE_A
		e0.spawn_val = SPAWN_A
		e1.change_val = CHANGE_A
	if at(t, WRITE_B_AT, "write_b"):
		# Only the watch slots. always_val must survive on the client from its baseline.
		e0.change_val = CHANGE_B
		e1.change_val = CHANGE_B
		print("[server] rewrote the ON_CHANGE slots")


func client_step(t: float) -> void:
	var e0: Node = main.entities().get("Ent0")
	var e1: Node = main.entities().get("Ent1")
	if e0 == null or e1 == null:
		if t > PHASE_A_AT:
			fail("entities never arrived")
			finish()
		return

	if not _phase_a_done:
		if t < PHASE_A_AT:
			return
		_phase_a_done = true
		check_eq(e0.always_val, ALWAYS_A, "phase A: ALWAYS slot arrived (mixed config)")
		check_eq(e0.change_val, CHANGE_A, "phase A: ON_CHANGE slot arrived (mixed config)")
		check_eq(e1.change_val, CHANGE_A, "phase A: ON_CHANGE slot arrived (watch-only config)")
		# NEVER is spawn-only. The server wrote it after the spawn, so a value arriving here
		# would mean the slot predicate had widened past the two per-tick modes.
		check_eq(e0.spawn_val, 0, "phase A: NEVER slot does not stream")
		return

	if t < PHASE_B_AT:
		return
	check_eq(e0.change_val, CHANGE_B, "phase B: ON_CHANGE slot re-replicated on change (mixed)")
	check_eq(e1.change_val, CHANGE_B, "phase B: ON_CHANGE slot re-replicated on change (watch-only)")
	check_eq(e0.always_val, ALWAYS_A, "phase B: untouched ALWAYS slot held from the baseline")
	finish()
