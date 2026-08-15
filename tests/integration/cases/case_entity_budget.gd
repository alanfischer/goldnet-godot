extends "res://test_case.gd"
## Entity-delta byte budget: sustained overflow must not permanently starve anything.
##
## `_server_tick` caps the per-peer entity-delta body at whatever a safe UDP payload has
## left (SAFE_PACKET_BYTES minus the header and the spawn/despawn/leave sections). When
## more entities change in one tick than fit, the losers are deliberately left out of
## `included` — the peer's next baseline — so the acked-baseline compare sees them as still
## changed and retries them on a later tick.
##
## That "free retry" is the whole safety argument for the budget, and it is exactly the
## kind of claim that is true right up until an off-by-one puts a dropped entity into
## `included` anyway. If it did, an ack would fold the entity into the baseline, the change
## would never be resent, and that entity would sit at a stale value forever while every
## other entity kept updating — invisible in a smoke test, permanent in a real game.
##
## So: enough entities to overflow the budget several times over, all moving every frame,
## then frozen. Every one of them must arrive at its final value.
##
## Timeline (server drives state, client asserts):
##
##   t=0.1   spawn COUNT entities
##   t<3.0   entities arrive; the spawn section drains and stops competing for the budget
##   t=3..7  server moves ALL of them every frame — sustained budget overflow
##   t=7.0   server freezes each at final_of(i)
##   t≥10.0  every entity is at final_of(i)              → the assertions
##
## COUNT is chosen so the changed set is several times the budget: at ~18 B per entity
## (net_id + varint mask + tag + Vector3) 80 entities is ~1440 B against a ~1180 B ceiling,
## so a third of them lose every tick and the retry path runs continuously for four seconds.
##
## SCOPE: this asserts the *behavioural* contract (nothing is starved, everything converges),
## not the byte ceiling itself — a snapshot's size isn't observable from GDScript, so a
## regression that kept convergence while overrunning the MTU would pass here. The ceiling
## arithmetic is covered by reading `peer_budget` in _server_tick, and the leave section's
## own cap has the same property. Verified by mutation: putting a budget-dropped entity into
## `included` (the bug described above) fails this case, and only this case.

const COUNT := 80
const TOL := 0.01

const CHURN_START := 3.0
const FREEZE_AT := 7.0
const JUDGE_AT := 10.0


## Where entity i is parked once the churn stops. Distinct per entity so a mixed-up id
## shows as a wrong position rather than as a pass.
static func final_of(i: int) -> Vector3:
	return Vector3(float(i) + 1.0, 100.0, 0.0)


func setup(_is_server: bool) -> void:
	timeout_s = 20.0


func server_step(t: float) -> void:
	if not spawn_once(t, COUNT):
		return
	var ents: Dictionary = main.entities()
	for i in COUNT:
		var e: Node3D = ents.get("Ent%d" % i)
		if e == null:
			continue
		if t < CHURN_START:
			e.position = home_of(i)
		elif t < FREEZE_AT:
			# Every entity changes every frame: the budget can never cover the full set, so
			# something is dropped on every tick for four seconds straight.
			e.position = Vector3(float(i) + 1.0, 0.0, t * 10.0)
		else:
			e.position = final_of(i)


func client_step(t: float) -> void:
	if t < JUDGE_AT:
		return

	var ents: Dictionary = main.entities()
	if not check_eq(ents.size(), COUNT, "all %d entities arrived" % COUNT):
		finish()
		return

	# Named individually so a failure says which entity was starved, not just how many.
	var stale := 0
	for i in COUNT:
		var e: Node3D = ents.get("Ent%d" % i)
		if e == null:
			fail("Ent%d missing at judge time" % i)
			continue
		if e.position.distance_to(final_of(i)) > TOL:
			stale += 1
			# One line per starved entity would drown the log if the retry path were broken
			# wholesale; the count assertion below is the verdict, this is the diagnosis.
			if stale <= 5:
				print("[client] Ent%d stale: got %.3v, want %.3v" % [i, e.position, final_of(i)])
	check_eq(stale, 0,
		"every entity converged after sustained budget overflow (%d of %d still stale — "
		% [stale, COUNT] + "a dropped entity was folded into the baseline and never resent)")
	finish()
