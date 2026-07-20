extends "res://test_case.gd"
## Quantization hints (`gn_quant` meta → `_read_quant` → the codec's lossy tags).
##
## A synchronizer can declare that a property tolerates reduced precision, and the sender
## packs it behind a lossy tag — vec3_half is 7 bytes where a full Vector3 is 13. The unit
## tests in ../ cover the codec arithmetic; what they can't reach is the wiring: reading
## the meta off a real synchronizer, mapping hint names to tags, and lining the tags up
## with the right slots.
##
## The failure mode this guards is silent. If the hint is dropped, everything still works
## — just at full width, and the bandwidth win quietly evaporates. If the hint is applied
## to the WRONG slot, values are mangled in a way that looks like a gameplay bug.
##
## Both entities are set to the same position. The hinted one must come back visibly
## lossy; the unhinted one must come back precise. Asserting both directions is what makes
## this a test of the hint rather than of replication in general:
##
##   Ent0  gn_quant {position: vec3_half}  → within HALF_TOL, but NOT within EXACT_TOL
##   Ent1  no hint                          → within EXACT_TOL

# Chosen so half precision is unambiguously lossy here: binary16 has a ~1.0 step at this
# magnitude, while float32 is exact to well under 0.01.
const VALUE := Vector3(1234.567, 0.0, 0.0)
const HALF_TOL := 2.0    # generous: we assert "close", the point is the next line
const EXACT_TOL := 0.01  # float32 path must beat this; the half path must not

func make_entity(data: Variant) -> Node:
	var n: Node = super.make_entity(data)
	if int(data) == 0:
		# The sender reads this meta; the decoder needs no hint (tags are self-describing).
		n.get_node("Sync").set_meta("gn_quant", {"position": "vec3_half"})
	return n


func setup(_is_server: bool) -> void:
	timeout_s = 15.0


func server_step(t: float) -> void:
	if not spawn_once(t, 2):
		return
	var ents: Dictionary = main.entities()
	for i in 2:
		var e: Node3D = ents.get("Ent%d" % i)
		if e != null:
			e.position = VALUE


func client_step(t: float) -> void:
	var ents: Dictionary = main.entities()
	if ents.size() < 2:
		if t > 8.0:
			fail("only %d of 2 entities arrived within 8s" % ents.size())
			finish()
		return

	# Let the stream settle so we're judging a delivered value, not an unpopulated one.
	if t < 3.0:
		return

	var hinted: Node3D = ents.get("Ent0")
	var plain: Node3D = ents.get("Ent1")
	if not check(hinted != null and plain != null, "both entities present"):
		finish()
		return

	var hinted_err := hinted.position.distance_to(VALUE)
	var plain_err := plain.position.distance_to(VALUE)
	print("[client] hinted err=%.4f  plain err=%.4f" % [hinted_err, plain_err])

	# The unhinted control proves the transport is precise by default...
	check(plain_err <= EXACT_TOL,
		"unhinted position arrived at full precision (err %.4f <= %.4f)" % [plain_err, EXACT_TOL])
	# ...so the hinted one being lossy is the hint taking effect, not general sloppiness.
	check(hinted_err > EXACT_TOL,
		"vec3_half hint actually reduced precision (err %.4f > %.4f — if this fails the hint was dropped)"
			% [hinted_err, EXACT_TOL])
	check(hinted_err <= HALF_TOL,
		"vec3_half hint stayed within half precision (err %.4f <= %.4f — a larger error means the tag hit the wrong slot)"
			% [hinted_err, HALF_TOL])
	finish()
