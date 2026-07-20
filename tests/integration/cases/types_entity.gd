extends Node3D
## Replicated entity for case_types: one property per wire tag, in one synchronizer.
##
## Not a case itself (run.sh only globs `case_*.gd`) — it's the fixture case_types builds
## its entity from. Kept as a real script rather than assembled inline because goldnet
## reads slot values through `get_indexed`, so the properties have to genuinely exist on
## the node.
##
## The declaration ORDER here is the slot order in the snapshot, which is the whole point:
## the tags are self-delimiting, so a tag that writes N bytes but reads M doesn't corrupt
## its own slot — it corrupts every slot after it. The `tail_*` pair at the end is the
## canary for exactly that (see case_types).
##
## Grouped by the tag each property is meant to exercise:
##
##   GN_T_FLOAT      f_float
##   GN_T_INT        i_small, i_neg, i_big          (zig-zag varint, 1..10 bytes)
##   GN_T_VEC3       v3
##   GN_T_BOOL       b_true, b_false
##   GN_T_VAR        s_text, v2, quat, col, packed, arr, dict, xform   (put_var fallback)
##   GN_T_ANGLE16    a_yaw                          (via the gn_quant hint)
##   GN_T_HALF       h_val                          (via the gn_quant hint)
##   GN_T_VEC3_HALF  vh                             (via the gn_quant hint)
##   -- canaries --  tail_int, tail_v3

# --- natively-tagged scalars ---
var f_float: float = 0.0
var i_small: int = 0
var i_neg: int = 0
var i_big: int = 0
var v3: Vector3 = Vector3.ZERO
var b_true: bool = false
var b_false: bool = true  # inverted default, so "arrived as false" can't be a no-op

# --- put_var fallback: everything without a dedicated tag ---
var s_text: String = ""
var v2: Vector2 = Vector2.ZERO
var quat: Quaternion = Quaternion()
var col: Color = Color(0, 0, 0, 0)
var packed: PackedByteArray = PackedByteArray()
var arr: Array = []
var dict: Dictionary = {}
var xform: Transform3D = Transform3D()

# --- quantization-hinted slots (lossy tags) ---
var a_yaw: float = 0.0
var h_val: float = 0.0
var vh: Vector3 = Vector3.ZERO

# --- canaries ---
#
# Last in slot order and deliberately cheap to verify. Any framing bug in a tag above
# shifts the stream and lands here as garbage, so these failing while the rest pass points
# at a width mismatch rather than at a value bug.
var tail_int: int = 0
var tail_v3: Vector3 = Vector3.ZERO


## Slot order, shared by the factory (to build the SceneReplicationConfig) and by the case
## (to assert on every one of them). Single source of truth so a property added above can't
## be silently left unreplicated or unasserted.
const SLOTS: Array[String] = [
	"f_float", "i_small", "i_neg", "i_big", "v3", "b_true", "b_false",
	"s_text", "v2", "quat", "col", "packed", "arr", "dict", "xform",
	"a_yaw", "h_val", "vh",
	"tail_int", "tail_v3",
]

## Which slots carry a quantization hint, and which hint. Read by the factory into the
## synchronizer's `gn_quant` meta; the decoder needs none (tags are self-describing).
const QUANT: Dictionary = {
	"a_yaw": "angle16",
	"h_val": "half",
	"vh": "vec3_half",
}
