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
## A command whose layout something else owns (a struct's fields) can instead be one Dictionary
## argument, declared by a field table in SyncSpec's shape: each name, in order, with its options.
## The owner keeps the table, and the RPC carries its fields by name:
##
##   const FIELDS := {"seq": {"type": TYPE_INT}, "yaw": {"type": TYPE_INT}, "head_x": {"type": TYPE_FLOAT}}
##   func submit_input(fields: Dictionary) -> void: ...
##   CommandStream.mark(self, &"submit_input", FIELDS)
##   submit_input.rpc_id(1, {"seq": seq, "yaw": yaw, "head_x": head_x})
##
## Field options: "type" (a Variant.Type; absent = any Variant). The names never go on the wire, and
## the table's order is the coding order, so put the fields that change most first. A call must give
## exactly the declared names. Stock sends the Dictionary as it is.
static func mark(node: Node, method: StringName, fields: Dictionary = {}) -> void:
	var methods: Array = node.get_meta(&"gn_commands", [])
	if not methods.has(method):
		methods.append(method)
	node.set_meta(&"gn_commands", methods)
	for field: Variant in fields:
		var opts: Variant = fields[field]
		if typeof(opts) != TYPE_DICTIONARY:
			push_error("CommandStream: field '%s' of '%s' needs an options Dictionary" % [field, method])
			continue
		for key: Variant in opts:
			if not key in _FIELD_KEYS:
				push_error("CommandStream: field '%s' of '%s' has an unknown option '%s'" % [field, method, key])
	var tables: Dictionary = node.get_meta(&"gn_command_fields", {})
	if fields.is_empty():
		tables.erase(method)
	else:
		tables[method] = fields.duplicate(true)
	node.set_meta(&"gn_command_fields", tables)


const _FIELD_KEYS := ["type"]
