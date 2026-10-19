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
## method is an ordinary RPC. Mark before the first call, since the answer is cached per node. A
## lone PackedByteArray argument goes as its own bytes, which suits a game packing its own command;
## goldnet delta-codes each command against the previous one, so a fixed layout compresses best.
## The pace is set by GoldNetMultiplayer.command_rate (GoldSrc cl_cmdrate).
static func mark(node: Node, method: StringName) -> void:
	var methods: Array = node.get_meta(&"gn_commands", [])
	if not methods.has(method):
		methods.append(method)
	node.set_meta(&"gn_commands", methods)
