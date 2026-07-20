extends "res://test_case.gd"
## Baseline replication: spawns reach the client and their state converges.
##
## This is the foundation case. It doesn't probe an edge — it establishes that the
## harness itself works, so that a failure in a sharper case (ring_expiry) means the
## protocol is broken rather than the test rig. If this one fails, don't trust the others.

const COUNT := 3
const TOL := 0.01

# Fixed positions (home_of, on the base) rather than motion: convergence is then a stable
# property the client can assert whenever it likes, with no clock sync between processes.


func setup(_is_server: bool) -> void:
	timeout_s = 15.0


func server_step(t: float) -> void:
	if not spawn_once(t, COUNT):
		return
	var ents: Dictionary = main.entities()
	for i in COUNT:
		var e: Node3D = ents.get("Ent%d" % i)
		if e != null:
			e.position = home_of(i)


func client_step(t: float) -> void:
	var ents: Dictionary = main.entities()
	if ents.size() < COUNT:
		if t > 10.0:
			fail("only %d of %d entities arrived within 10s" % [ents.size(), COUNT])
			finish()
		return

	# All present — give the position stream a moment to settle before judging it.
	if t < 3.0:
		return

	check_eq(ents.size(), COUNT, "entity count")
	for i in COUNT:
		var e: Node3D = ents.get("Ent%d" % i)
		if check(e != null, "Ent%d present" % i):
			check_near(e.position, home_of(i), TOL, "Ent%d converged" % i)
	finish()
