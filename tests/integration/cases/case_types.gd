extends "res://test_case.gd"
## Every wire tag in `gn_put_value` / `gn_get_value`, in one synchronizer, across a delta.
##
## The rest of the suite replicates exactly one property type: `Vector3 position` (plus the
## one `vec3_half` hint in `quant`). So the tag dispatch — GN_T_FLOAT / INT / VEC3 / BOOL,
## the ANGLE16 and HALF quantized tags, and the put_var fallback that carries every other
## Variant — has had no coverage at all. That's the codec every replicated property in a
## real game goes through.
##
## The arithmetic underneath is unit-tested in ../../test_codec.cpp; what needs a running
## engine is the dispatch and, above all, the FRAMING. Tags are self-delimiting: a tag that
## writes N bytes but reads M doesn't corrupt its own slot, it corrupts every slot after it
## in the snapshot. So the shape of this case is one entity carrying every type at once,
## with `tail_int` / `tail_v3` last in slot order as canaries — if a width is wrong upstream
## the tails land as garbage.
##
## Timeline (the second phase is what makes this more than a smoke test):
##
##   t=0.1  spawn Ent0, server writes VALUES_A to all 20 slots
##   t=3.0  client asserts every slot arrived as A                     (phase A)
##   t=4.0  server rewrites only the CHANGED_IN_B slots
##   t=7.0  client asserts the changed slots are B and, just as
##          importantly, the untouched ones are STILL A                (phase B)
##
## Phase B is the delta path. Only changed slots go on the wire, so the untouched ones have
## to be reconstructed from the peer's acked baseline — with mixed types and mixed widths in
## the same frame. Asserting the unchanged slots is what makes this test the delta rather
## than a second full-state send.

const Fixture := preload("res://cases/types_entity.gd")

# One angle16 step (~0.0055°). The encoder truncates, so round-trip error is in [0, STEP).
const STEP := TAU / 65536.0

# Deliberately mid-step: sitting halfway between two representable angles guarantees a
# non-trivial quantization error, so "the hint was silently dropped" (which would arrive at
# full float32 precision) is distinguishable from "the hint was applied".
const YAW_A := 10000.5 * STEP
const YAW_B := 40000.5 * STEP

# Magnitudes where binary16's step is ~1.0, so half precision is unmistakably lossy here
# while float32 is exact to well under 0.01 — same trick as `quant`, for the same reason.
const HALF_A := 1234.567
const VH_A := Vector3(1234.567, 0.0, 0.0)

const ANGLE_TOL := STEP          # one step: the encoder truncates, never more
const ANGLE_MIN_ERR := 1.0e-5    # ...but it must be lossy at all, or the hint was dropped
const HALF_TOL := 2.0
const HALF_MIN_ERR := 0.01

const PHASE_A_AT := 3.0
const WRITE_B_AT := 4.0
const PHASE_B_AT := 7.0

## Full state written at spawn. Floats are values float32 holds exactly, so the unhinted
## slots can be asserted with `==` rather than a tolerance — a width regression there should
## fail loudly, not hide under an epsilon.
##
## `var` rather than `const`: PackedByteArray and Transform3D literals aren't constant
## expressions in GDScript. Treat both of these as immutable regardless.
var VALUES_A := {
	"f_float": 2.5,
	"i_small": 42,                        # 1-byte varint
	"i_neg": -1000,                       # negative: the zig-zag path
	"i_big": 1234567890123456789,         # near-full-width: the 9-10 byte path
	"v3": Vector3(1.5, -2.25, 3.75),
	"b_true": true,
	"b_false": false,
	"s_text": "goldnet",
	"v2": Vector2(1.5, -2.5),
	"quat": Quaternion(0.5, 0.5, 0.5, 0.5),
	"col": Color(0.25, 0.5, 0.75, 1.0),
	"packed": PackedByteArray([1, 2, 3, 255, 0]),
	"arr": [1, "two", 3.5],
	"dict": {"k": 1, "n": Vector3(1, 2, 3)},
	"xform": Transform3D(Basis(), Vector3(7, 8, 9)),
	"a_yaw": YAW_A,
	"h_val": HALF_A,
	"vh": VH_A,
	"tail_int": 7777,
	"tail_v3": Vector3(0.5, 0.5, 0.5),
}

## The subset rewritten at WRITE_B_AT. Everything absent from here must survive phase B
## unchanged, reconstructed from the baseline rather than re-sent. Spans a tagged scalar,
## a varint, a vector, a bool, a put_var value and a quantized slot, so the delta carries a
## mix of widths rather than a uniform one.
var CHANGED_IN_B := {
	"f_float": 6.25,
	"i_small": 100,
	"v3": Vector3(10.5, 20.25, -30.75),
	"b_true": false,
	"s_text": "goldnet-phase-b",
	"a_yaw": YAW_B,
}

var _phase_a_done := false


## Build the fixture on both peers. Identical structure on each side is what keeps scene
## paths — and therefore goldnet net ids — in agreement.
func make_entity(data: Variant) -> Node:
	var n := Node3D.new()
	n.set_script(Fixture)
	n.name = "Ent%d" % int(data)

	var cfg := SceneReplicationConfig.new()
	for slot in Fixture.SLOTS:
		var p := NodePath(".:%s" % slot)
		cfg.add_property(p)
		cfg.property_set_sync(p, true)

	var sync := MultiplayerSynchronizer.new()
	sync.name = "Sync"
	sync.replication_config = cfg
	sync.replication_interval = 1.0 / 30.0
	sync.set_visibility_public(public_visibility)
	# The sender reads this; the decoder needs no hint (tags are self-describing).
	sync.set_meta("gn_quant", Fixture.QUANT)
	n.add_child(sync)
	return n


func setup(_is_server: bool) -> void:
	timeout_s = 16.0


func server_step(t: float) -> void:
	if not spawn_once(t, 1):
		return
	var e: Node = main.entities().get("Ent0")
	if e == null:
		return

	if not fired("write_b"):
		for slot in VALUES_A:
			e.set(slot, VALUES_A[slot])
	if at(t, WRITE_B_AT, "write_b"):
		for slot in CHANGED_IN_B:
			e.set(slot, CHANGED_IN_B[slot])
		print("[server] rewrote %d of %d slots" % [CHANGED_IN_B.size(), Fixture.SLOTS.size()])


func client_step(t: float) -> void:
	var e: Node = main.entities().get("Ent0")
	if e == null:
		if t > PHASE_A_AT:
			fail("entity never arrived")
			finish()
		return

	# Phase A: full state. Must be judged before the server rewrites anything at t=4.
	if not _phase_a_done:
		if t < PHASE_A_AT:
			return
		_phase_a_done = true
		_assert_slots(e, VALUES_A, "A")
		return

	# Phase B: the delta. Changed slots take their new value; everything else must still
	# hold its phase-A value, rebuilt from the baseline.
	if t < PHASE_B_AT:
		return
	var expected := VALUES_A.duplicate()
	expected.merge(CHANGED_IN_B, true)
	_assert_slots(e, expected, "B")
	finish()


## Assert every slot on `e` against `expected`, dispatching on how that slot is carried.
## Walks Fixture.SLOTS rather than the expectation dictionary so a property that was added
## to the fixture but never given an expected value fails here instead of going unchecked.
func _assert_slots(e: Node, expected: Dictionary, phase: String) -> void:
	for slot in Fixture.SLOTS:
		if not check(expected.has(slot), "phase %s: slot %s has an expected value" % [phase, slot]):
			continue
		var want: Variant = expected[slot]
		var got: Variant = e.get(slot)
		match slot:
			"a_yaw":
				_assert_lossy(got, want, ANGLE_TOL, ANGLE_MIN_ERR, phase, slot, "angle16")
			"h_val":
				_assert_lossy(got, want, HALF_TOL, HALF_MIN_ERR, phase, slot, "half")
			"vh":
				# vec3_half: compare on the vector, but the lossiness argument is the same.
				var err: float = (got as Vector3).distance_to(want as Vector3)
				check(err <= HALF_TOL and err > HALF_MIN_ERR,
					"phase %s: %s round-tripped through vec3_half (err %.4f, want (%.2f, %.2f])"
						% [phase, slot, err, HALF_MIN_ERR, HALF_TOL])
			"packed", "arr", "dict", "xform":
				# Container/composite types go through put_var. Compared by serialized form:
				# Dictionary `==` is not a deep compare in Godot 4, so it would pass on two
				# unequal dictionaries.
				check_eq(var_to_str(got), var_to_str(want),
					"phase %s: %s round-tripped through put_var" % [phase, slot])
			_:
				# Exactly-representable values on purpose — no epsilon to hide a width bug.
				check_eq(got, want, "phase %s: %s round-tripped exactly" % [phase, slot])


## A quantized slot must land close to the source value (the tag decoded correctly) AND
## measurably off it (the hint was actually applied — a dropped hint arrives at full
## precision and would sail through a tolerance-only check).
func _assert_lossy(got: Variant, want: Variant, tol: float, min_err: float,
		phase: String, slot: String, tag: String) -> void:
	var err: float = absf(float(got) - float(want))
	check(err <= tol and err > min_err,
		"phase %s: %s round-tripped through %s (err %.6f, want (%.6f, %.6f])"
			% [phase, slot, tag, err, min_err, tol])
