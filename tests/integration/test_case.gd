extends RefCounted
## Base for goldnet integration cases.
##
## Deliberately no `class_name`: global class names resolve from a cache the editor writes
## during an import pass, which a bare `--headless --path` run doesn't perform. Cases
## `extends "res://test_case.gd"` by path instead, so the suite runs from a clean checkout
## with no import step.
##
## Each case runs as a pair of processes — one server, one client — connected over a real
## ENetMultiplayerPeer with a real GoldNetMultiplayer installed. That's the point: these
## exercise the snapshot/ack/spawn paths end to end, which the standalone unit tests in
## ../ cannot reach (godot-cpp's Variant and StreamPeerBuffer only work inside a running
## engine).
##
## The CLIENT is the driver: it holds the assertions and its exit code is the verdict.
## The server is a cooperative peer that does whatever the case needs, then waits to be
## killed by the runner. Splitting it that way keeps the pass/fail signal in one process.
##
## Cases override `server_step`/`client_step`, called once per frame with the elapsed
## seconds since the peer connected. Assertions accumulate failures rather than aborting
## so one run reports everything wrong, not just the first thing.

## Seconds a case may run after connecting before it's declared hung. Cases that need
## longer (a blackout longer than the ring, say) raise this in `setup`.
var timeout_s: float = 15.0

## How many clients this case requires. Cases needing more override it AND declare the
## same number in a `## @clients N` line that run.sh greps to know how many to launch.
## The harness cross-checks the two at startup and fails loudly on a mismatch — without
## that check a missed grep silently degrades a multi-client case to one client, where
## `multipeer` and `pvs_per_peer` both pass while testing nothing.
var required_clients: int = 1

## Grace before the first spawn. goldnet must poll once to wrap the spawner's
## spawn_function; anything spawned before that never enters the snapshot stream and the
## case fails looking like a protocol bug. Load-bearing — see spawn_once().
const SPAWN_GRACE_S := 0.1

var _failures: Array[String] = []
var _checks: int = 0
var _done: bool = false

## The Main node, for scene-tree access (get_tree(), add_child, spawner).
var main: Node = null

## Which client this process is, for cases declaring `## @clients N`. Always 0 on the
## server and in single-client cases. Note the server can't map a peer id to this index
## without the client telling it — prefer designing cases so neither side needs to know.
var client_index: int = 0


## The installed GoldNetMultiplayer, for cases that drive its knobs (loss_percent,
## sim_seed, snapshot_interval_ms, ...). Returned untyped on purpose: those properties
## live on the GDExtension class, not on the MultiplayerAPI base, so a static type here
## would reject them at parse time.
func goldnet():
	return main.multiplayer


# --- overrides ---

## Called once after the peer is created, before any frame runs. Use to set timeout_s or
## configure the GoldNetMultiplayer (loss_percent, sim_seed, ...).
func setup(_is_server: bool) -> void:
	pass

## Called once per frame on the server. `t` is seconds since a peer connected.
func server_step(_t: float) -> void:
	pass

## Called once per frame on the client. `t` is seconds since connecting to the server.
func client_step(_t: float) -> void:
	pass

## Set false so entities are gated per peer rather than visible to everyone. Cases doing
## PVS work need this — with public visibility on, the per-peer bit is never consulted.
var public_visibility: bool = true


## Entity factory, run identically on both peers so scene paths (and goldnet net ids)
## match. Cases that spawn nothing can leave this alone.
func make_entity(data: Variant) -> Node:
	var n := Node3D.new()
	n.name = "Ent%d" % int(data)
	var cfg := SceneReplicationConfig.new()
	cfg.add_property(^".:position")
	cfg.property_set_sync(^".:position", true)
	var sync := MultiplayerSynchronizer.new()
	sync.name = "Sync"
	sync.replication_config = cfg
	sync.replication_interval = 1.0 / 30.0
	sync.set_visibility_public(public_visibility)
	n.add_child(sync)
	return n


## Where entity i lives by convention, for cases that just need distinct positions.
static func home_of(i: int) -> Vector3:
	return Vector3(float(i) + 1.0, 0.0, 0.0)


# --- timeline helpers ---
#
# Cases are frame-driven state machines over elapsed seconds. These absorb the two
# patterns every case was hand-rolling, so a case reads as its documented timeline
# rather than as a pile of boolean latches.

var _spawn_done := false
var _latches := {}


## Spawn entities 0..count-1 once, after the grace period. Returns true once spawning is
## behind us, so the caller's usual shape is:
##
##     if not spawn_once(t, 3):
##         return
func spawn_once(t: float, count: int) -> bool:
	if _spawn_done:
		return true
	if t < SPAWN_GRACE_S:
		return false
	for i in count:
		main.spawn(i)
	_spawn_done = true
	return false # let the spawn settle a frame before the caller acts on it


## One-shot trigger: true exactly once, on the first call at or after `when`. Replaces the
## `if t >= X and not _flag: _flag = true` latch pairs.
func at(t: float, when: float, key: String) -> bool:
	if t < when or _latches.has(key):
		return false
	_latches[key] = true
	return true


## Whether an `at()` trigger has already fired, for cases that branch on it later.
func fired(key: String) -> bool:
	return _latches.has(key)


# --- assertions ---

func check(cond: bool, msg: String) -> bool:
	_checks += 1
	if not cond:
		_failures.append(msg)
	return cond


func check_eq(got: Variant, want: Variant, msg: String) -> bool:
	return check(got == want, "%s (got %s, want %s)" % [msg, got, want])


## Vector3 comparison with a tolerance — replicated positions go through half-precision
## quantization on some paths, so exact equality is the wrong test.
func check_near(got: Vector3, want: Vector3, tol: float, msg: String) -> bool:
	return check(got.distance_to(want) <= tol,
		"%s (got %.3v, want %.3v, tol %.3f)" % [msg, got, want, tol])


func fail(msg: String) -> void:
	_checks += 1
	_failures.append(msg)


## Ends the case. Cases call this once their assertions are complete; without it the
## harness runs to timeout_s and reports a hang.
func finish() -> void:
	_done = true


# --- harness plumbing ---

func is_done() -> bool:
	return _done


func failures() -> Array[String]:
	return _failures


func check_count() -> int:
	return _checks
