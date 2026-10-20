class_name CommandStream
extends RefCounted

## Marks an @rpc method as a command stream: a client's input to the server, one call per command,
## the way Quake sends usercmds. Pure GDScript like the other addon helpers, and inert without
## goldnet: it only sets the node's `gn_commands` meta, which stock SceneMultiplayer never reads.
##
##   @rpc("any_peer", "call_remote", "reliable")
##   func submit_input(cmd: PackedByteArray) -> void: ...
##
##   func _ready() -> void:
##       CommandStream.mark(self, &"submit_input")
##
## The contract is a reliable RPC's, so declare it reliable: every call reaches the server once, in
## order, with get_remote_sender_id() naming the client. Stock keeps it with ENet's reliable
## channel, which stalls behind each loss. goldnet keeps it with unreliable packets that repeat
## every unacked command, acked from the snapshot, so a loss costs nothing until it's a long one.
## A command still unacked after COMMAND_RING (about a second at 60 Hz) is given up on.
##
## Only a client's calls to the server (rpc_id(1, ...)) are carried this way; any other call to the
## method is an ordinary RPC. Mark before the first call, since the answer is cached per node.
##
## The method's arguments are the command's fields, coded field by field against the previous call
## the way GoldSrc's delta.lst coded a usercmd, so a call costs only what changed. Declare their
## types: an int field costs its step from the last call (a byte for a small one), a float its f32
## when it changes, a bool nothing but its flip. A PackedByteArray goes bytewise, and an untyped or
## other-typed argument as its Variant encoding, bytewise. Quantize to ints yourself where you can,
## and predict with the same values the server will get: a float travels as f32. Every call must
## pass every argument. The pace is set by GoldNetMultiplayer.command_rate (GoldSrc cl_cmdrate).
##
## A command whose layout something else owns (a struct's fields, in its own order) can instead be
## one Array argument, with the element types given here; each element is then a field, coded as
## above. The owner keeps the order and the types in one place, and the RPC just carries the array:
##
##   func submit_input(fields: Array) -> void: ...
##   CommandStream.mark(self, &"submit_input", [TYPE_INT, TYPE_INT, TYPE_FLOAT])
##   submit_input.rpc_id(1, [seq, yaw, head_x])
##
## Every call must then pass exactly that many elements. Stock sends the Array as it is.
static func mark(node: Node, method: StringName, fields: Array = []) -> void:
	var methods: Array = node.get_meta(&"gn_commands", [])
	if not methods.has(method):
		methods.append(method)
	node.set_meta(&"gn_commands", methods)
	var typed: Dictionary = node.get_meta(&"gn_command_fields", {})
	if fields.is_empty():
		typed.erase(method)
	else:
		typed[method] = fields.duplicate()
	node.set_meta(&"gn_command_fields", typed)
