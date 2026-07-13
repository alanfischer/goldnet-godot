extends Node
## Minimal goldnet demo — proves the drop-in reuse contract with stock nodes only.
##
## Run two headless instances from this directory:
##   godot --headless --path . -- --server
##   godot --headless --path . -- --client
##
## The server installs GoldNetMultiplayer, spawns 3 cubes through a stock
## MultiplayerSpawner, and moves them every frame. The client installs the same API,
## connects, and receives the cubes + their moving positions over GoldSrc ack-delta
## snapshots — with NO goldnet-specific code: just a MultiplayerSpawner and
## MultiplayerSynchronizers, exactly as with Godot's built-in replication.

const PORT := 9977

var _is_server := false
var _spawner: MultiplayerSpawner
var _cubes: Array = []
var _t := 0.0


func _ready() -> void:
	_is_server = OS.get_cmdline_user_args().has("--server")
	var role := "server" if _is_server else "client"

	# 1) Install goldnet as the tree's MultiplayerAPI, BEFORE any synchronizer/spawner
	#    enters the tree. That single line is the whole opt-in.
	if not ClassDB.class_exists(&"GoldNetMultiplayer"):
		push_error("GoldNetMultiplayer missing — run ../build.sh and ensure the .gdextension is registered")
		get_tree().quit(1)
		return
	get_tree().set_multiplayer(ClassDB.instantiate(&"GoldNetMultiplayer"))

	# 2) Transport: a stock ENetMultiplayerPeer. goldnet delegates handshake/peers to it.
	var peer := ENetMultiplayerPeer.new()
	var err := peer.create_server(PORT, 8) if _is_server else peer.create_client("127.0.0.1", PORT)
	if err != OK:
		push_error("peer setup failed: %s" % error_string(err))
		get_tree().quit(1)
		return
	multiplayer.multiplayer_peer = peer

	# 3) A stock MultiplayerSpawner. goldnet folds its spawns — and the spawned entities'
	#    child MultiplayerSynchronizers — into the snapshot stream with no special handling.
	_spawner = MultiplayerSpawner.new()
	_spawner.name = "Spawner"
	_spawner.spawn_path = ^".."              # spawn cubes under this Main node
	_spawner.spawn_function = _make_cube
	add_child(_spawner)

	print("[%s] goldnet installed on udp:%d" % [role, PORT])

	if _is_server:
		multiplayer.peer_connected.connect(func(id: int): print("[server] peer %d connected" % id))
		# Let goldnet poll once so it wraps the spawner's spawn_function before we spawn — otherwise
		# these spawns happen before interception and never enter the stream. (In a real game entities
		# spawn well after setup, so this only matters for spawning in the same frame as the spawner.)
		await get_tree().process_frame
		for i in 3:
			_cubes.append(_spawner.spawn(i))
		print("[server] spawned %d cubes" % _cubes.size())
	else:
		multiplayer.connected_to_server.connect(func(): print("[client] connected"))
		multiplayer.connection_failed.connect(func():
			push_error("[client] connection failed"); get_tree().quit(1))


## Spawnable entity, built in code (no scene file): a Node3D whose `position` a stock
## MultiplayerSynchronizer replicates. Runs on the server to create the cube and on the
## client (via goldnet's spawn stream) to reconstruct it — identical on both, so their
## scene paths (and thus goldnet's net ids) match.
func _make_cube(data: Variant) -> Node:
	var cube := Node3D.new()
	cube.name = "Cube%d" % int(data)
	var cfg := SceneReplicationConfig.new()
	cfg.add_property(^".:position")
	cfg.property_set_sync(^".:position", true)
	var sync := MultiplayerSynchronizer.new()
	sync.name = "Sync"
	sync.replication_config = cfg
	sync.replication_interval = 1.0 / 30.0
	cube.add_child(sync)
	return cube


func _process(delta: float) -> void:
	_t += delta
	var tick := fmod(_t, 1.0) < delta   # ~once per second
	if _is_server:
		for i in _cubes.size():
			var c: Node3D = _cubes[i]
			if is_instance_valid(c):
				c.position = Vector3(cos(_t + i), 0.0, sin(_t + i)) * (2.0 + float(i))
		if tick and not _cubes.is_empty():
			print("[server] t=%.0f cube0.pos=%.2v" % [_t, (_cubes[0] as Node3D).position])
	elif tick:
		var cubes := get_children().filter(func(n: Node): return n.name.begins_with("Cube"))
		if cubes.is_empty():
			print("[client] t=%.0f (no cubes yet)" % _t)
		else:
			var s := ""
			for c in cubes:
				s += " %s=%.2v" % [c.name, (c as Node3D).position]
			print("[client] t=%.0f received %d cubes:%s" % [_t, cubes.size(), s])
