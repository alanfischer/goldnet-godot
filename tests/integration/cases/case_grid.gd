extends "res://test_case.gd"
## Grid slots (`gn_grid` meta): a Vector3 that travels as whole multiples of its grid, each axis
## coded as its step from the value the client holds at its baseline.
##
## Steps only land right if both ends agree on the base: the server steps from the value the peer
## held at its acked baseline, the client applies the step to that same frame's value. So the
## server moves both entities every tick for a few seconds over a lossy downlink, where baselines
## lag and snapshots go missing, then stops at FINAL. A step taken from the wrong base would leave
## the gridded entity off by whole grid steps, not by rounding.
##
##   t=0-4    both move every tick, 20% of snapshots lost, 40 ms each way
##   t=4+     both rest at FINAL
##   t=7      the client checks:
##              Ent0 (gn_grid {position: 0.25}) at FINAL rounded to the grid, exactly
##              Ent1 (no hint)                   at FINAL, exactly

const GRID := 0.25
const FINAL := Vector3(12.34, -5.67, 8.9)
const SNAPPED := Vector3(12.25, -5.75, 9.0)
const MOVE_UNTIL := 4.0
const CHECK_AT := 7.0
const LOSSY := {"loss_percent": 20, "latency_min_ms": 40, "latency_max_ms": 40, "seed": 5}


func make_entity(data: Variant) -> Node:
	var n: Node = super.make_entity(data)
	if int(data) == 0:
		n.get_node("Sync").set_meta("gn_grid", {"position": GRID})
	return n


func setup(is_server: bool) -> void:
	timeout_s = 12.0
	if is_server:
		goldnet().sim_config = LOSSY


func server_step(t: float) -> void:
	if not spawn_once(t, 2):
		return
	var ents: Dictionary = main.entities()
	var p := FINAL
	if t < MOVE_UNTIL:
		p = Vector3(t * 3.7, sin(t * 2.0) * 6.0, -t * 2.3)
	for i in 2:
		var e: Node3D = ents.get("Ent%d" % i)
		if e != null:
			e.position = p


func client_step(t: float) -> void:
	if t < CHECK_AT:
		return
	var ents: Dictionary = main.entities()
	var gridded: Node3D = ents.get("Ent0")
	var plain: Node3D = ents.get("Ent1")
	if not check(gridded != null and plain != null, "both entities present"):
		finish()
		return
	check(plain.position.is_equal_approx(FINAL), "the unhinted position lands exactly (got %v)" % plain.position)
	check(gridded.position.is_equal_approx(SNAPPED),
		"the gridded position lands on the grid at FINAL's nearest point (got %v, want %v)" % [gridded.position, SNAPPED])
	finish()
