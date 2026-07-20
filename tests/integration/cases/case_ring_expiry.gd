extends "res://test_case.gd"
## PeerRing baseline expiry — the case the standalone unit tests can't reach.
##
## The server keeps the last RING (32) snapshots per peer so it can delta against whatever
## the peer last acked. If a peer goes quiet for longer than that, its baseline is evicted
## and the server MUST fall back to sending full state. If that fallback is broken, the
## server keeps diffing against a baseline the client no longer has, and any change made
## during the blackout is never re-carried — the client is permanently, silently stale.
##
## That failure only appears after ~1s of sustained loss, which is why it survives casual
## playtesting: brief loss self-heals from a live baseline and looks fine.
##
## Timeline (server drives state, client asserts):
##
##   t=0.1  spawn Ent0 at HOME
##   t<2.0  client converges to HOME                      → assertion 1
##   t=2.0  server loss_percent=100 — blackout begins
##   t=2.5  server moves Ent0 to AWAY (client must not see it)
##   t=4.8  client still at HOME                          → assertion 2 (guards the test)
##   t=5.0  server loss_percent=0 — blackout ends, ~90 snapshots lost (RING is 32)
##   t≤9.0  client reaches AWAY                           → assertion 3 (the real one)
##
## Assertion 2 matters as much as 3: without it, a harness that silently failed to apply
## loss would sail through assertion 3 and report a pass having tested nothing.

const HOME := Vector3(1.0, 0.0, 0.0)
const AWAY := Vector3(999.0, 0.0, 0.0)
const TOL := 0.01

const BLACKOUT_START := 2.0
const MOVE_AT := 2.5
const BLACKOUT_END := 5.0   # 3s ≈ 90 snapshots at 30Hz, well past RING=32
const OBSERVE_STALE := 4.8
const DEADLINE := 9.0

var _saw_home := false
var _checked_stale := false


func setup(_is_server: bool) -> void:
	timeout_s = 14.0


func server_step(t: float) -> void:
	if not spawn_once(t, 1):
		return

	var e: Node3D = main.entities().get("Ent0")
	if e == null:
		return

	if not fired("move"):
		e.position = HOME
	if at(t, BLACKOUT_START, "blackout_on"):
		goldnet().loss_percent = 100
		print("[server] blackout on (loss=100)")
	if at(t, MOVE_AT, "move"):
		e.position = AWAY
		print("[server] moved Ent0 to AWAY during blackout")
	if at(t, BLACKOUT_END, "blackout_off"):
		goldnet().loss_percent = 0
		print("[server] blackout off (loss=0)")


func client_step(t: float) -> void:
	var e: Node3D = main.entities().get("Ent0")

	# 1) Baseline replication works before we break anything.
	if not _saw_home:
		if e != null and e.position.distance_to(HOME) <= TOL:
			_saw_home = true
			print("[client] converged to HOME at t=%.1f" % t)
		elif t > BLACKOUT_START:
			fail("never converged to HOME before the blackout (t=%.1f)" % t)
			finish()
		return

	# 2) The blackout really blacked out. If this fails, loss isn't being applied and
	#    assertion 3 below would prove nothing.
	if not _checked_stale:
		if t < OBSERVE_STALE:
			return
		_checked_stale = true
		if check(e != null, "Ent0 still present during blackout"):
			check_near(e.position, HOME, TOL,
				"Ent0 stayed at HOME during blackout (server's AWAY move must not have arrived)")
		return

	# 3) The baseline was evicted, so the post-blackout snapshot has to carry full state.
	#    Reaching AWAY is the client learning a change made entirely while it was deaf.
	if e != null and e.position.distance_to(AWAY) <= TOL:
		check(true, "Ent0 recovered to AWAY after baseline eviction")
		print("[client] recovered to AWAY at t=%.1f" % t)
		finish()
	elif t > DEADLINE:
		fail("never recovered to AWAY after the blackout — baseline eviction did not fall back to full state (last position %.3v)"
			% (e.position if e != null else Vector3.ZERO))
		finish()
