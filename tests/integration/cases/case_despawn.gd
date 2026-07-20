extends "res://test_case.gd"
## Despawn lifecycle, including under sustained loss.
##
## Despawns are reliable-until-acked (`_reliable_include`): the server owes each peer the
## despawn until that peer acks a frame carrying it. If that bookkeeping is wrong, a
## despawn issued while the peer is deaf is lost forever and the client keeps rendering an
## entity the server has already freed. That's the shape of the dead-puppet-resurrect
## class of bug — the entity is gone server-side but visibly alive on the client.
##
## Timeline:
##
##   t=0.1  spawn Ent0, Ent1, Ent2
##   t=2.5  free Ent0 with a clear link       → client drops it        (assertion 1)
##   t=4.0  loss=100 — blackout begins
##   t=4.5  free Ent1 while the peer is deaf
##   t=6.5  loss=0 — blackout ends            → client drops Ent1      (assertion 2)
##   t≤10   Ent2 survives the whole run                                (assertion 3)
##
## Ent2 is the control. Without it, a client that simply dropped every entity — or a
## harness that lost the connection — would pass assertions 1 and 2 while proving nothing.

const TOL := 0.01
const FREE_VISIBLE_AT := 2.5
const BLACKOUT_START := 4.0
const FREE_HIDDEN_AT := 4.5
const BLACKOUT_END := 6.5
const DEADLINE := 10.0

var _saw_all_three := false
var _ent0_gone := false
var _ent1_gone := false


func setup(_is_server: bool) -> void:
	timeout_s = 16.0


func server_step(t: float) -> void:
	if not spawn_once(t, 3):
		return

	var ents: Dictionary = main.entities()
	for i in 3:
		var e: Node3D = ents.get("Ent%d" % i)
		if e != null:
			e.position = home_of(i)

	if at(t, FREE_VISIBLE_AT, "free_visible"):
		var e0: Node3D = ents.get("Ent0")
		if e0 != null:
			e0.queue_free()
			print("[server] freed Ent0 (link clear)")
	if at(t, BLACKOUT_START, "blackout_on"):
		goldnet().loss_percent = 100
		print("[server] blackout on")
	if at(t, FREE_HIDDEN_AT, "free_hidden"):
		var e1: Node3D = ents.get("Ent1")
		if e1 != null:
			e1.queue_free()
			print("[server] freed Ent1 during blackout")
	if at(t, BLACKOUT_END, "blackout_off"):
		goldnet().loss_percent = 0
		print("[server] blackout off")


func client_step(t: float) -> void:
	var ents: Dictionary = main.entities()

	# Everything must arrive before we can meaningfully assert anything leaves.
	if not _saw_all_three:
		if ents.size() >= 3:
			_saw_all_three = true
			print("[client] all 3 entities present at t=%.1f" % t)
		elif t > FREE_VISIBLE_AT - 0.1:
			fail("only %d of 3 entities arrived before the first despawn" % ents.size())
			finish()
		return

	# 1) A despawn over a clear link.
	if not _ent0_gone:
		if not ents.has("Ent0"):
			_ent0_gone = true
			print("[client] Ent0 removed at t=%.1f" % t)
		elif t > BLACKOUT_START:
			fail("Ent0 was freed on the server at t=2.5 but is still present at t=%.1f" % t)
			finish()
		return

	# 2) A despawn issued while the peer was deaf — the reliable-until-acked path.
	if not _ent1_gone:
		if not ents.has("Ent1"):
			_ent1_gone = true
			check(true, "Ent1 despawn survived the blackout and was redelivered")
			print("[client] Ent1 removed at t=%.1f" % t)
		elif t > DEADLINE:
			fail("Ent1 was freed during the blackout but never despawned on the client — the owed despawn was dropped rather than resent")
			finish()
		return

	# 3) The control: nothing removed anything it shouldn't have.
	check(ents.has("Ent2"), "Ent2 survived (despawns were targeted, not indiscriminate)")
	var e2: Node3D = ents.get("Ent2")
	if e2 != null:
		check_near(e2.position, home_of(2), TOL, "Ent2 still replicating after both despawns")
	finish()
