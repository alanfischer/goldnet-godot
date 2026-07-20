extends Node
## Bootstrap for goldnet integration cases. Run as:
##
##   godot --headless --path tests/integration -- --server --case ring_expiry
##   godot --headless --path tests/integration -- --client --case ring_expiry
##
## See run.sh, which orchestrates both halves and reports the verdict.

const PORT := 9978

var _is_server := false
var _case: RefCounted = null # a "res://test_case.gd" instance; see that file on class_name
var _case_name := ""
var _spawner: MultiplayerSpawner
var _connected := false
var _t := 0.0
var _wait_t := 0.0

## Seconds to wait for the two processes to find each other before giving up. Generous:
## a cold start on a loaded CI box is slower than it looks locally.
const CONNECT_TIMEOUT_S := 20.0


func _ready() -> void:
	var args := OS.get_cmdline_user_args()
	_is_server = args.has("--server")
	var ci := args.find("--case")
	if ci == -1 or ci + 1 >= args.size():
		_bail("missing --case <name>")
		return
	_case_name = args[ci + 1]

	# Multi-client cases get an index so a case can vary behavior per client. Cases that
	# don't need it ignore it; run.sh always passes it.
	var xi := args.find("--client-index")
	var client_index := int(args[xi + 1]) if xi != -1 and xi + 1 < args.size() else 0
	var ni := args.find("--clients")
	var launched_clients := int(args[ni + 1]) if ni != -1 and ni + 1 < args.size() else 1

	var path := "res://cases/case_%s.gd" % _case_name
	if not ResourceLoader.exists(path):
		_bail("no such case: %s (expected %s)" % [_case_name, path])
		return
	_case = (load(path) as GDScript).new()
	_case.main = self
	_case.client_index = client_index

	# goldnet must be installed BEFORE any synchronizer or spawner enters the tree.
	if not ClassDB.class_exists(&"GoldNetMultiplayer"):
		_bail("GoldNetMultiplayer missing — run ./build.sh first")
		return
	get_tree().set_multiplayer(ClassDB.instantiate(&"GoldNetMultiplayer"))

	var peer := ENetMultiplayerPeer.new()
	var err := peer.create_server(PORT, 8) if _is_server else peer.create_client("127.0.0.1", PORT)
	if err != OK:
		_bail("peer setup failed: %s" % error_string(err))
		return
	multiplayer.multiplayer_peer = peer

	_case.setup(_is_server)

	# Cross-check the case's stated requirement against what run.sh actually launched. The
	# `## @clients N` line run.sh greps lives in a comment, so a reworded docstring or a
	# different invocation path silently yields one client — and a multi-client case run
	# with one client passes while testing nothing. Fail instead. (After setup(), which is
	# where cases declare the requirement.)
	if launched_clients != _case.required_clients:
		_bail("case needs %d client(s) but %d were launched — check its `## @clients N` line"
			% [_case.required_clients, launched_clients])
		return

	# A stock MultiplayerSpawner; goldnet folds its spawns into the snapshot stream.
	_spawner = MultiplayerSpawner.new()
	_spawner.name = "Spawner"
	_spawner.spawn_path = ^".."
	_spawner.spawn_function = _case.make_entity
	add_child(_spawner)

	if _is_server:
		multiplayer.peer_connected.connect(func(_id: int): _connected = true)
	else:
		multiplayer.connected_to_server.connect(func():
			_connected = true
			_register_client_index.rpc_id(1, _case.client_index))
		multiplayer.connection_failed.connect(func(): _bail("connection failed"))
		multiplayer.server_disconnected.connect(func(): _bail("server disconnected"))

	print("[%s] case=%s ready on udp:%d" % ["server" if _is_server else "client", _case_name, PORT])


## Spawn through the case's factory. Server-side only — the client reconstructs from the
## snapshot stream. Cases call this rather than touching the spawner directly.
func spawn(data: Variant) -> Node:
	return _spawner.spawn(data)


# --- peer index registry ---
#
# Peer ids are assigned by ENet and tell the server nothing about which client process it
# is talking to. Asymmetric multi-client cases ("hide this from client 0 only") need that
# mapping, so each client reports its --client-index on connect. Rides the inner
# SceneMultiplayer like any other @rpc; goldnet only replaces state replication.

var _peer_index: Dictionary = {}  # peer id -> client index (server only)


@rpc("any_peer", "reliable")
func _register_client_index(idx: int) -> void:
	if not multiplayer.is_server():
		return
	_peer_index[multiplayer.get_remote_sender_id()] = idx


## Peer id for a client index, or 0 if that client hasn't registered yet. Server-side.
func peer_for_index(idx: int) -> int:
	for peer in _peer_index:
		if _peer_index[peer] == idx:
			return peer
	return 0


## How many clients have reported in. Cases wait on this before acting asymmetrically.
func registered_count() -> int:
	return _peer_index.size()


## The replicated entities this peer currently has, by name.
func entities() -> Dictionary:
	var out := {}
	for c in get_children():
		if c.name.begins_with("Ent"):
			out[String(c.name)] = c
	return out


func _process(delta: float) -> void:
	if _case == null:
		return

	if not _connected:
		# Neither half can do anything useful until the peers meet; failing here rather
		# than running the case against a dead link keeps the error honest.
		_wait_t += delta
		if _wait_t > CONNECT_TIMEOUT_S:
			_bail("timed out after %.0fs waiting for the peer to connect" % CONNECT_TIMEOUT_S)
		return

	_t += delta
	if _is_server:
		_case.server_step(_t)
		return

	_case.client_step(_t)

	if _case.is_done():
		_report()
	elif _t > _case.timeout_s:
		_case.fail("case timed out after %.1fs without finishing" % _case.timeout_s)
		_report()


## Print the verdict and exit. Exit code is the signal run.sh keys on; the printed lines
## are for a human reading a failure.
func _report() -> void:
	var fails: Array = _case.failures()
	for f in fails:
		print("  FAIL: %s" % f)
	if fails.is_empty():
		print("PASS %s (%d checks)" % [_case_name, _case.check_count()])
		get_tree().quit(0)
	else:
		print("FAIL %s (%d of %d checks failed)" % [_case_name, fails.size(), _case.check_count()])
		get_tree().quit(1)


func _bail(msg: String) -> void:
	push_error("[harness] %s" % msg)
	print("FAIL %s: %s" % [_case_name if _case_name else "?", msg])
	get_tree().quit(1)
