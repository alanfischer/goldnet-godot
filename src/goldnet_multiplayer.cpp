#include "goldnet_multiplayer.h"
#include "goldnet_link.h"

#include <cmath>

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/multiplayer_spawner.hpp>
#include <godot_cpp/classes/scene_multiplayer.hpp>
#include <godot_cpp/classes/scene_replication_config.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/classes/stream_peer_buffer.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/vector3.hpp>

using namespace godot;

// Default snapshot cadence when a synchronizer reports no interval (30 Hz).
static const uint32_t DEFAULT_INTERVAL_MS = 33;

// Max PVS-leave markers per snapshot (4 bytes each on the wire). A fresh peer's relevance seed can
// queue hundreds of leaves (every out-of-PVS entity on a big map); emitting them all in one frame
// overruns the MTU, so it's dropped and never acked — and the same oversized frame resends forever.
// Capping keeps each frame under MTU; leaves are reliable-until-acked, so they drain over a few ticks.
static const uint16_t MAX_LEAVES_PER_SNAPSHOT = 32;

// Max sync properties per entity — the width of the u32 changed-field bitmask. Slot
// collection, mask building, and the apply loop all key off this.
static const int MAX_SYNC_SLOTS = 32;

// Snapshot wire-protocol magic + version, written as the first u32 of every snapshot. Bump the low
// byte on any wire-format change so a client talking to a mismatched build fails loudly (one clear
// warning + dropped snapshot) instead of misparsing every packet into a flood of decode errors.
// 'G''N''S' + version. Old builds (no magic) start with a small seq value, which never matches.
static const uint32_t GN_SNAPSHOT_MAGIC = 0x474E5302u; // "GNS" + v2 (v2 added spawn-lazy + leave section)

// An entity's cross-peer identity: a hash of the node's scene path. Server and client
// derive it the same way from the same path, so it matches without a handshake. Used for
// synchronizers (state), spawned nodes (spawn record), and spawners — must stay in sync
// everywhere, hence one helper.
static uint32_t net_id_for(Node *p_node) {
	return (uint32_t)String(p_node->get_path()).hash();
}

// Split a replication-config property path (relative to the synchronizer's root
// node, e.g. ".:position" or "NetInterp:net_pos") into the target node under
// `p_root` and the property path to get_indexed/set_indexed on it. Returns null
// if the target node can't be resolved.
static Node *resolve_property(Node *p_root, const NodePath &p_cfg_path, NodePath &r_prop) {
	String node_path;
	int64_t nc = p_cfg_path.get_name_count();
	for (int64_t i = 0; i < nc; i++) {
		String n = p_cfg_path.get_name(i);
		if (n == ".") {
			continue;
		}
		node_path += (node_path.is_empty() ? String() : String("/")) + n;
	}
	Node *target = node_path.is_empty() ? p_root : p_root->get_node_or_null(NodePath(node_path));
	if (!target) {
		return nullptr;
	}
	String prop;
	int64_t sc = p_cfg_path.get_subname_count();
	for (int64_t i = 0; i < sc; i++) {
		prop += String(":") + p_cfg_path.get_subname(i);
	}
	r_prop = NodePath(prop);
	return target;
}

// Compact, self-delimiting wire encoding for per-slot delta values. Stock Variant
// put_var tags every value with a 4-byte type header and stores floats/ints at 64-bit
// width — unaffordable on the hot per-tick delta path, where the same handful of
// property types (float, int, Vector3, bool) recur across every entity. We pack those
// behind a 1-byte tag using 32-bit floats and zig-zag varint ints (a moving player's
// ~8 props drop from ~112 to ~48 bytes), and fall back to put_var for anything else so
// the stream stays self-delimiting and any Variant still round-trips.
//
// Optional per-property QUANTIZATION (Phase 5): a synchronizer may hint that a slot is an angle,
// or that a float / Vector3 tolerates half precision, via its "gn_quant" meta (see _read_quant).
// We then pack it lossily behind its own tag — angles to a u16 (~0.0055° steps), floats/Vector3 to
// IEEE binary16. The tags stay self-describing, so the DECODER needs no hint (it reads the tag and
// knows the width, preserving the "consume even for unknown ids" invariant); only the SENDER
// consults the hint. A moving player's yaw+pitch drop 10→6 B, net_pos (if hinted) 13→7 B.
enum GNValueTag : uint8_t {
	GN_T_VAR = 0, GN_T_FLOAT = 1, GN_T_INT = 2, GN_T_VEC3 = 3, GN_T_BOOL = 4,
	GN_T_ANGLE16 = 5,   // float radians -> u16 over [0, TAU)
	GN_T_HALF = 6,      // float -> IEEE binary16
	GN_T_VEC3_HALF = 7, // Vector3 -> 3x binary16
};
// Sentinel for "no quantization hint on this slot — pick the tag from the runtime type".
static const uint8_t GN_Q_AUTO = 255;
static const float GN_TAU = 6.28318530717958647692f;

static void gn_put_varint(const Ref<StreamPeerBuffer> &buf, int64_t p_v) {
	uint64_t u = ((uint64_t)p_v << 1) ^ (uint64_t)(p_v >> 63); // zig-zag: small magnitudes → few bytes
	while (u >= 0x80) {
		buf->put_u8((uint8_t)u | 0x80);
		u >>= 7;
	}
	buf->put_u8((uint8_t)u);
}

static int64_t gn_get_varint(const Ref<StreamPeerBuffer> &buf) {
	uint64_t u = 0;
	int shift = 0;
	uint8_t b;
	do {
		b = buf->get_u8();
		u |= (uint64_t)(b & 0x7F) << shift;
		shift += 7;
	} while (b & 0x80);
	return (int64_t)(u >> 1) ^ -(int64_t)(u & 1); // un-zig-zag
}

// IEEE binary16 via StreamPeer's built-in half codec (round-to-nearest, handles subnormals).
static void gn_put_half(const Ref<StreamPeerBuffer> &buf, float f) { buf->put_half(f); }
static float gn_get_half(const Ref<StreamPeerBuffer> &buf) { return buf->get_half(); }

static void gn_put_angle16(const Ref<StreamPeerBuffer> &buf, float radians) {
	float t = fmodf(radians, GN_TAU);
	// NaN/inf guard: fmodf(NaN or ±inf, TAU) == NaN, and casting NaN — or the boundary value that
	// rounds to exactly 65536.0 — straight to uint16 is undefined behaviour (a debug build traps it,
	// crashing the server). Sanitize to 0 and go through uint32+mask so the wrap is well-defined.
	if (!(t == t)) { // NaN
		t = 0.0f;
	} else if (t < 0.0f) {
		t += GN_TAU;
	}
	// [0,TAU) → [0,65536); cast through uint32 (always in range) then mask to 16 bits so the boundary
	// 65536 wraps to 0 without the out-of-range float→uint16 cast the comment used to rely on.
	buf->put_u16((uint16_t)((uint32_t)((t / GN_TAU) * 65536.0f) & 0xFFFFu));
}
static float gn_get_angle16(const Ref<StreamPeerBuffer> &buf) {
	return ((float)buf->get_u16() / 65536.0f) * GN_TAU;
}

// Map a "gn_quant" hint name to its tag, or GN_Q_AUTO if the name is unknown.
static uint8_t gn_quant_from_name(const String &name) {
	if (name == "angle16") {
		return GN_T_ANGLE16;
	}
	if (name == "half") {
		return GN_T_HALF;
	}
	if (name == "vec3_half") {
		return GN_T_VEC3_HALF;
	}
	return GN_Q_AUTO;
}

static void gn_put_value(const Ref<StreamPeerBuffer> &buf, const Variant &v, uint8_t quant = GN_Q_AUTO) {
	switch (quant) {
		case GN_T_ANGLE16:
			buf->put_u8(GN_T_ANGLE16);
			gn_put_angle16(buf, (float)(double)v);
			return;
		case GN_T_HALF:
			buf->put_u8(GN_T_HALF);
			gn_put_half(buf, (float)(double)v);
			return;
		case GN_T_VEC3_HALF: {
			buf->put_u8(GN_T_VEC3_HALF);
			Vector3 vv = v;
			gn_put_half(buf, vv.x);
			gn_put_half(buf, vv.y);
			gn_put_half(buf, vv.z);
			return;
		}
		default:
			break; // GN_Q_AUTO (or a hint that can't apply) → type-based encoding below
	}
	switch (v.get_type()) {
		case Variant::FLOAT:
			buf->put_u8(GN_T_FLOAT);
			buf->put_float((float)(double)v);
			break;
		case Variant::INT:
			buf->put_u8(GN_T_INT);
			gn_put_varint(buf, (int64_t)v);
			break;
		case Variant::VECTOR3: {
			buf->put_u8(GN_T_VEC3);
			Vector3 vv = v;
			buf->put_float(vv.x);
			buf->put_float(vv.y);
			buf->put_float(vv.z);
		} break;
		case Variant::BOOL:
			buf->put_u8(GN_T_BOOL);
			buf->put_u8((bool)v ? 1 : 0);
			break;
		default:
			buf->put_u8(GN_T_VAR);
			buf->put_var(v);
			break;
	}
}

static Variant gn_get_value(const Ref<StreamPeerBuffer> &buf) {
	uint8_t t = buf->get_u8();
	switch (t) {
		case GN_T_FLOAT:
			return (double)buf->get_float();
		case GN_T_INT:
			return gn_get_varint(buf);
		case GN_T_VEC3: {
			float x = buf->get_float();
			float y = buf->get_float();
			float z = buf->get_float();
			return Vector3(x, y, z);
		}
		case GN_T_BOOL:
			return buf->get_u8() != 0;
		case GN_T_ANGLE16:
			return (double)gn_get_angle16(buf);
		case GN_T_HALF:
			return (double)gn_get_half(buf);
		case GN_T_VEC3_HALF: {
			float x = gn_get_half(buf);
			float y = gn_get_half(buf);
			float z = gn_get_half(buf);
			return Vector3(x, y, z);
		}
		default:
			return buf->get_var();
	}
}

GoldNetMultiplayer::GoldNetMultiplayer() {
	dbg = getenv("GOLDNET_DEBUG") != nullptr;
	{
		const char *loss = getenv("GOLDNET_LOSS");
		dbg_loss = loss ? atoi(loss) : 0;
	}
	{
		// GOLDNET_LATENCY=min,max (per-leg ms range) or a single fixed value; GOLDNET_SPIKE=ms,interval,duration.
		// These let a headless server simulate its own send leg with no admin console (see netsim_plan §3.5).
		// Route through the setters (not raw fields) so clamping/side-effects live in exactly one place.
		const char *lat = getenv("GOLDNET_LATENCY");
		if (lat) {
			PackedStringArray parts = String(lat).split(",");
			if (parts.size() >= 2) {
				set_latency_min_ms(parts[0].to_int());
				set_latency_max_ms(parts[1].to_int());
			} else if (parts.size() == 1 && !parts[0].is_empty()) {
				set_latency_min_ms(parts[0].to_int());
				set_latency_max_ms(parts[0].to_int());
			}
		}
		const char *spike = getenv("GOLDNET_SPIKE");
		if (spike) {
			PackedStringArray parts = String(spike).split(",");
			if (parts.size() >= 1 && !parts[0].is_empty()) {
				set_spike_ms(parts[0].to_int());
			}
			if (parts.size() >= 2) {
				set_spike_interval_s((float)parts[1].to_float());
			}
			if (parts.size() >= 3) {
				set_spike_duration_s((float)parts[2].to_float());
			}
		}
	}
	inner = MultiplayerAPI::create_default_interface(); // a SceneMultiplayer

	SceneMultiplayer *sm = Object::cast_to<SceneMultiplayer>(inner.ptr());
	if (sm) {
		sm->set_root_path(NodePath("/root"));
	}

	inner->connect("peer_connected", callable_mp(this, &GoldNetMultiplayer::_relay_peer_connected));
	inner->connect("peer_disconnected", callable_mp(this, &GoldNetMultiplayer::_relay_peer_disconnected));
	inner->connect("connected_to_server", callable_mp(this, &GoldNetMultiplayer::_relay_connected_to_server));
	inner->connect("connection_failed", callable_mp(this, &GoldNetMultiplayer::_relay_connection_failed));
	inner->connect("server_disconnected", callable_mp(this, &GoldNetMultiplayer::_relay_server_disconnected));
}

GoldNetMultiplayer::~GoldNetMultiplayer() {}

void GoldNetMultiplayer::_reset_client_state() {
	for (int i = 0; i < RING; i++) {
		client_frames[i].clear();
		client_frame_seq[i] = 0;
	}
	client_last_seq = 0;
	client_has = false;
	defer_streak = 0;
	client_spawned.clear(); // spawner nodes are torn down with the session
}

void GoldNetMultiplayer::_relay_peer_connected(int64_t p_id) { emit_signal("peer_connected", p_id); }
void GoldNetMultiplayer::_relay_peer_disconnected(int64_t p_id) {
	peer_rings.erase((int32_t)p_id); // drop the server-side send history for this peer
	// Drop the peer from any despawn needer-set so those despawns can still retire.
	Vector<uint32_t> emptied;
	for (KeyValue<uint32_t, HashSet<int32_t>> &kv : despawn_pending) {
		kv.value.erase((int32_t)p_id);
		if (kv.value.is_empty()) {
			emptied.push_back(kv.key);
		}
	}
	for (int i = 0; i < emptied.size(); i++) {
		despawn_pending.erase(emptied[i]);
	}
	emit_signal("peer_disconnected", p_id);
}
void GoldNetMultiplayer::_relay_connected_to_server() {
	_reset_client_state(); // fresh session — old baselines are meaningless
	emit_signal("connected_to_server");
}
void GoldNetMultiplayer::_relay_connection_failed() { emit_signal("connection_failed"); }
void GoldNetMultiplayer::_relay_server_disconnected() {
	_reset_client_state();
	emit_signal("server_disconnected");
}

// We own every synchronizer that carries at least one sync (per-tick) property. Since
// Phase 3 also owns the MultiplayerSpawners, there is no longer a map-static vs.
// spawner-managed split — movers, projectiles, and players all stream their state
// through the same delta path. (Spawn-only configs, if any, carry no per-tick state
// and are left alone.)
bool GoldNetMultiplayer::_should_intercept(MultiplayerSynchronizer *p_sync) const {
	Ref<SceneReplicationConfig> cfg = p_sync->get_replication_config();
	if (cfg.is_null()) {
		return false;
	}
	TypedArray<NodePath> props = cfg->get_properties();
	for (int i = 0; i < props.size(); i++) {
		if (cfg->property_get_sync(props[i])) {
			return true;
		}
	}
	return false;
}

// Find (or lazily create) the snapshot carrier at /root/__GoldNetLink. Stateless —
// resolved by path each call so a freed/reloaded node is never dereferenced. The
// node lives under the Window root, which outlives map scene changes.
GoldNetLink *GoldNetMultiplayer::_ensure_link() {
	SceneTree *tree = Object::cast_to<SceneTree>(Engine::get_singleton()->get_main_loop());
	if (!tree) {
		return nullptr;
	}
	Window *root = tree->get_root();
	if (!root) {
		return nullptr;
	}
	// Catch spawners already in the tree and every one added afterward, so their spawn_function is
	// wrapped before any spawn() call. This runs on our first poll — but a game that spawns during
	// _ready (before any poll) must call capture_spawners() right after installing us, or those early
	// spawns use the unwrapped function and are never replicated.
	capture_spawners();
	GoldNetLink *l = Object::cast_to<GoldNetLink>(root->get_node_or_null(NodePath("__GoldNetLink")));
	if (!l) {
		l = memnew(GoldNetLink);
		l->set_name("__GoldNetLink");
		root->add_child(l);
		l->set_multiplayer_authority(1);
		Dictionary rpc_cfg;
		rpc_cfg["rpc_mode"] = MultiplayerAPI::RPC_MODE_ANY_PEER;
		rpc_cfg["transfer_mode"] = MultiplayerPeer::TRANSFER_MODE_UNRELIABLE;
		rpc_cfg["call_local"] = false;
		rpc_cfg["channel"] = 0;
		l->rpc_config("_gn_recv", rpc_cfg);

		// Client → server frame ack. unreliable_ordered so a newer ack always
		// supersedes an older one and a lost ack self-heals on the next.
		Dictionary ack_cfg;
		ack_cfg["rpc_mode"] = MultiplayerAPI::RPC_MODE_ANY_PEER;
		ack_cfg["transfer_mode"] = MultiplayerPeer::TRANSFER_MODE_UNRELIABLE_ORDERED;
		ack_cfg["call_local"] = false;
		ack_cfg["channel"] = 0;
		l->rpc_config("_gn_ack", ack_cfg);
	}
	return l;
}

// Resolve a synchronizer from its stable ObjectID, returning null if it has been
// freed (godot-cpp has no is_instance_valid; ObjectDB::get_instance does the check).
static MultiplayerSynchronizer *sync_from_objid(uint64_t p_objid) {
	return Object::cast_to<MultiplayerSynchronizer>(ObjectDB::get_instance(p_objid));
}

// Collect the instance ids of every MultiplayerSynchronizer at or under p_node (used to gate a spawn
// on the entity's per-peer visibility — see SpawnRecord::sync_objids).
static void collect_syncs(Node *p_node, Vector<uint64_t> &r_out) {
	if (Object::cast_to<MultiplayerSynchronizer>(p_node)) {
		r_out.push_back(p_node->get_instance_id());
	}
	TypedArray<Node> children = p_node->get_children();
	for (int i = 0; i < children.size(); i++) {
		Node *c = Object::cast_to<Node>((Object *)children[i]);
		if (c) {
			collect_syncs(c, r_out);
		}
	}
}

bool GoldNetMultiplayer::_reliable_include(HashMap<uint32_t, uint16_t> &p_wait, uint32_t p_net_id,
		uint16_t p_seq, uint16_t p_last_acked, bool p_has_ack) {
	uint16_t fs;
	if (p_wait.has(p_net_id)) {
		fs = p_wait[p_net_id];
	} else {
		fs = p_seq;
		p_wait[p_net_id] = p_seq;
	}
	if (p_has_ack && _seq_le(fs, p_last_acked)) {
		p_wait.erase(p_net_id);
		return false; // delivered — stop resending
	}
	return true;
}

void GoldNetMultiplayer::_retire_acked(HashMap<uint32_t, uint16_t> &p_wait, uint16_t p_last_acked,
		Vector<uint32_t> &r_retired) {
	for (const KeyValue<uint32_t, uint16_t> &kv : p_wait) {
		if (_seq_le(kv.value, p_last_acked)) {
			r_retired.push_back(kv.key);
		}
	}
	for (int i = 0; i < r_retired.size(); i++) {
		p_wait.erase(r_retired[i]);
	}
}

// --- Phase 3: spawn / despawn ---

// Replace a spawner's spawn_function with our trampoline, so a server spawn(data) hands us
// the reconstruction data. We must do this BEFORE the first spawn — the spawn itself is the
// first time object_configuration_add reveals the spawner, which is too late — so we hook
// SceneTree.node_added (and a one-time scan for spawners already in the tree). Idempotent.
void GoldNetMultiplayer::_wrap_spawner(MultiplayerSpawner *p_spawner) {
	if (!p_spawner) {
		return;
	}
	uint64_t objid = p_spawner->get_instance_id();
	if (spawners.has(objid)) {
		return;
	}
	SpawnerEntry e;
	e.net_id = net_id_for(p_spawner);
	e.orig_fn = p_spawner->get_spawn_function();
	spawners[objid] = e;
	spawner_netid_to_objid[e.net_id] = objid;
	p_spawner->set_spawn_function(
			callable_mp(this, &GoldNetMultiplayer::_spawn_trampoline).bind((int64_t)objid));
}

void GoldNetMultiplayer::_on_node_added(Node *p_node) {
	MultiplayerSpawner *sp = Object::cast_to<MultiplayerSpawner>(p_node);
	if (sp) {
		_wrap_spawner(sp);
	}
}

void GoldNetMultiplayer::_scan_spawners() {
	SceneTree *tree = Object::cast_to<SceneTree>(Engine::get_singleton()->get_main_loop());
	if (!tree || !tree->get_root()) {
		return;
	}
	TypedArray<Node> found = tree->get_root()->find_children("*", "MultiplayerSpawner", true, false);
	for (int i = 0; i < found.size(); i++) {
		_wrap_spawner(Object::cast_to<MultiplayerSpawner>(found[i]));
	}
}

// Arm spawner capture: wrap every spawner currently in the tree and connect node_added so future ones
// are wrapped on entry. Idempotent. Called on our first poll, but a game that spawns during _ready must
// call this immediately after set_multiplayer() so the spawn_function is wrapped before those spawns.
void GoldNetMultiplayer::capture_spawners() {
	if (spawners_scanned) {
		return;
	}
	SceneTree *tree = Object::cast_to<SceneTree>(Engine::get_singleton()->get_main_loop());
	if (!tree || !tree->get_root()) {
		return; // tree not ready yet — the first poll will retry
	}
	spawners_scanned = true;
	tree->connect("node_added", callable_mp(this, &GoldNetMultiplayer::_on_node_added));
	_scan_spawners();
}

// The wrapper installed over each spawner's spawn_function. On the server, spawn(data)
// calls this; we run the game's real function to build the node, stash the reconstruction
// data (the node isn't in the tree yet — _drain_pending_spawns promotes it once it is), and
// return the node so spawn() proceeds normally.
Variant GoldNetMultiplayer::_spawn_trampoline(Variant p_data, int64_t p_spawner_objid) {
	uint64_t sobj = (uint64_t)p_spawner_objid;
	Variant node_v;
	if (spawners.has(sobj)) {
		node_v = spawners[sobj].orig_fn.callv(Array::make(p_data));
	}
	Node *node = Object::cast_to<Node>(node_v);
	if (node) {
		PendingSpawn ps;
		ps.spawner_objid = sobj;
		ps.data = p_data;
		pending_spawns[node->get_instance_id()] = ps;
	}
	return node_v;
}

// Server: promote captured spawns to spawn_records once the node is in the tree (so its
// path — and thus net_id — is stable).
void GoldNetMultiplayer::_drain_pending_spawns() {
	if (pending_spawns.is_empty()) {
		return;
	}
	Vector<uint64_t> done;
	for (const KeyValue<uint64_t, PendingSpawn> &kv : pending_spawns) {
		Node *node = Object::cast_to<Node>(ObjectDB::get_instance(kv.key));
		if (!node) {
			done.push_back(kv.key); // freed before it entered the tree
			continue;
		}
		if (!node->is_inside_tree()) {
			continue; // wait for spawn() to add it
		}
		uint32_t net_id = net_id_for(node);
		SpawnRecord rec;
		rec.spawner_net_id = spawners.has(kv.value.spawner_objid) ? spawners[kv.value.spawner_objid].net_id : 0;
		rec.node_objid = kv.key;
		rec.data = kv.value.data;
		collect_syncs(node, rec.sync_objids); // for lazy, visibility-gated spawning
		spawn_records[net_id] = rec;
		despawn_pending.erase(net_id); // a reused net_id is a fresh spawn, not a despawn
		done.push_back(kv.key);
	}
	for (int i = 0; i < done.size(); i++) {
		pending_spawns.erase(done[i]);
	}
}

// Server: a spawned node that has been freed becomes a despawn owed to every peer that RECEIVED the
// spawn. With lazy spawning, peers that never saw the entity never got the spawn, so they're owed
// nothing — scope the despawn to peers with the node in spawn_acked (delivered) or spawn_wait (in flight).
void GoldNetMultiplayer::_detect_despawns() {
	if (spawn_records.is_empty()) {
		return;
	}
	Vector<uint32_t> gone;
	for (const KeyValue<uint32_t, SpawnRecord> &kv : spawn_records) {
		if (!ObjectDB::get_instance(kv.value.node_objid)) {
			gone.push_back(kv.key);
		}
	}
	if (gone.is_empty()) {
		return;
	}
	for (int i = 0; i < gone.size(); i++) {
		uint32_t net_id = gone[i];
		spawn_records.erase(net_id);
		HashSet<int32_t> needers;
		for (KeyValue<int32_t, PeerRing> &pr : peer_rings) {
			if (pr.value.spawn_acked.has(net_id) || pr.value.spawn_wait.has(net_id)) {
				needers.insert(pr.key); // this peer has (or is mid-receiving) the node → owes a despawn
			}
			pr.value.spawn_wait.erase(net_id);  // stop resending the (now void) spawn
			pr.value.spawn_acked.erase(net_id); // and forget delivery, so a reused id re-spawns cleanly
		}
		if (!needers.is_empty()) {
			despawn_pending[net_id] = needers;
		}
	}
}

// Client: build a spawned node from its recipe, add it under the spawner's spawn_path, and
// fire the spawner's `spawned` signal so the game runs its per-node bookkeeping. The node's
// child synchronizer auto-registers on enter-tree and streams state like any other entity.
void GoldNetMultiplayer::_apply_spawn(uint32_t p_net_id, uint32_t p_spawner_net_id, const Variant &p_data) {
	if (client_spawned.has(p_net_id)) {
		return; // already have it (redundant reliable-until-acked resend)
	}
	if (!spawner_netid_to_objid.has(p_spawner_net_id)) {
		return; // spawner not registered here yet — a later resend will land
	}
	uint64_t sobj = spawner_netid_to_objid[p_spawner_net_id];
	MultiplayerSpawner *spawner = Object::cast_to<MultiplayerSpawner>(ObjectDB::get_instance(sobj));
	if (!spawner || !spawners.has(sobj)) {
		return;
	}
	Node *spawn_root = spawner->get_node_or_null(spawner->get_spawn_path());
	if (!spawn_root) {
		return;
	}
	Variant node_v = spawners[sobj].orig_fn.callv(Array::make(p_data));
	Node *node = Object::cast_to<Node>(node_v);
	if (!node) {
		return;
	}
	spawn_root->add_child(node);
	client_spawned[p_net_id] = node->get_instance_id();
	spawner->emit_signal("spawned", node);
}

// Client: free a spawned node and fire the spawner's `despawned` signal.
void GoldNetMultiplayer::_apply_despawn(uint32_t p_net_id) {
	if (!client_spawned.has(p_net_id)) {
		return;
	}
	uint64_t objid = client_spawned[p_net_id];
	client_spawned.erase(p_net_id);
	Node *node = Object::cast_to<Node>(ObjectDB::get_instance(objid));
	if (!node) {
		return;
	}
	Node *parent = node->get_parent();
	for (const KeyValue<uint64_t, SpawnerEntry> &kv : spawners) {
		MultiplayerSpawner *s = Object::cast_to<MultiplayerSpawner>(ObjectDB::get_instance(kv.key));
		if (s && s->get_node_or_null(s->get_spawn_path()) == parent) {
			s->emit_signal("despawned", node);
			break;
		}
	}
	node->queue_free();
}

uint32_t GoldNetMultiplayer::_min_interval_ms() const {
	if (snapshot_interval_override > 0) {
		return (uint32_t)snapshot_interval_override; // config override pins one global cadence
	}
	uint32_t best = 0;
	for (const KeyValue<uint64_t, SyncEntry> &kv : owned_syncs) {
		MultiplayerSynchronizer *s = sync_from_objid(kv.key);
		if (!s) {
			continue;
		}
		double sec = s->get_replication_interval();
		uint32_t ms = sec > 0.0 ? (uint32_t)(sec * 1000.0) : DEFAULT_INTERVAL_MS;
		if (ms == 0) {
			ms = 1;
		}
		if (best == 0 || ms < best) {
			best = ms;
		}
	}
	return best == 0 ? DEFAULT_INTERVAL_MS : best;
}

// --- Config surface (Phase 5) ---

void GoldNetMultiplayer::set_snapshot_interval_ms(int p_ms) {
	snapshot_interval_override = p_ms > 0 ? p_ms : 0;
	cached_min_interval_ms = _min_interval_ms();
}
int GoldNetMultiplayer::get_snapshot_interval_ms() const {
	return snapshot_interval_override;
}
void GoldNetMultiplayer::set_debug_enabled(bool p_enabled) {
	dbg = p_enabled;
}
bool GoldNetMultiplayer::is_debug_enabled() const {
	return dbg;
}
void GoldNetMultiplayer::set_loss_percent(int p_pct) {
	dbg_loss = p_pct < 0 ? 0 : (p_pct > 100 ? 100 : p_pct);
}
int GoldNetMultiplayer::get_loss_percent() const {
	return dbg_loss;
}
void GoldNetMultiplayer::set_relevance_events(bool p_enabled) {
	relevance_events_enabled = p_enabled;
}
bool GoldNetMultiplayer::get_relevance_events() const {
	return relevance_events_enabled;
}
void GoldNetMultiplayer::set_latency_min_ms(int p_ms) {
	latency_min_ms = p_ms < 0 ? 0 : p_ms;
}
int GoldNetMultiplayer::get_latency_min_ms() const {
	return latency_min_ms;
}
void GoldNetMultiplayer::set_latency_max_ms(int p_ms) {
	latency_max_ms = p_ms < 0 ? 0 : p_ms;
}
int GoldNetMultiplayer::get_latency_max_ms() const {
	return latency_max_ms;
}
void GoldNetMultiplayer::set_spike_ms(int p_ms) {
	spike_ms = p_ms < 0 ? 0 : p_ms;
	if (spike_ms == 0) { // disabling clears any in-flight spike
		_spike_active = false;
		_spike_timer = 0.0f;
		_spike_elapsed = 0.0f;
	}
}
int GoldNetMultiplayer::get_spike_ms() const {
	return spike_ms;
}
void GoldNetMultiplayer::set_spike_interval_s(float p_s) {
	spike_interval_s = p_s < 0.0f ? 0.0f : p_s;
}
float GoldNetMultiplayer::get_spike_interval_s() const {
	return spike_interval_s;
}
void GoldNetMultiplayer::set_spike_duration_s(float p_s) {
	spike_duration_s = p_s < 0.0f ? 0.0f : p_s;
}
float GoldNetMultiplayer::get_spike_duration_s() const {
	return spike_duration_s;
}
void GoldNetMultiplayer::sim_reset() {
	_sim_queue.clear();
	_last_fire_at = 0;
	_spike_active = false;
	_spike_timer = 0.0f;
	_spike_elapsed = 0.0f;
	_rpc_unreliable_cache.clear();
}

// --- Send-side sim engine (port of net_latency_sim.gd) ---

// Advance the WiFi-spike state machine by one poll's worth of elapsed time. A spike is a brief
// window where every send is held by spike_ms instead of the normal latency range, modelling a
// power-save stall that releases a burst of packets at once.
void GoldNetMultiplayer::_sim_update_spike(float p_delta) {
	if (spike_ms <= 0) {
		return;
	}
	if (_spike_active) {
		_spike_elapsed += p_delta;
		if (_spike_elapsed >= spike_duration_s) {
			_spike_active = false;
			_spike_elapsed = 0.0f;
			_spike_timer = 0.0f;
		}
	} else {
		_spike_timer += p_delta;
		if (_spike_timer >= spike_interval_s) {
			_spike_active = true;
			_spike_elapsed = 0.0f;
			_spike_timer = 0.0f;
		}
	}
}

// The delay (ms) to apply to the current send: the full spike latency while a spike is active,
// otherwise a random draw from [latency_min_ms, latency_max_ms]. 0 ⇒ send immediately. Unlike the
// GDScript original there is no >>1 half-split — this is the full per-leg delay (netsim_plan §4b).
int GoldNetMultiplayer::_sim_delay_ms() {
	if (_spike_active && spike_ms > 0) {
		return spike_ms > 1 ? spike_ms : 1;
	}
	if (latency_max_ms <= 0) {
		return 0;
	}
	return (int)UtilityFunctions::randi_range(latency_min_ms, latency_max_ms);
}

// The shared tail of both send funnels (the snapshot send and _rpc, minus their own loss gates):
// apply this send's latency/spike, sending immediately when there's none or queuing it otherwise.
Error GoldNetMultiplayer::_sim_send(int32_t p_peer, Object *p_object, const StringName &p_method, const Array &p_args) {
	int d = _sim_delay_ms();
	if (d <= 0) {
		return inner->rpc(p_peer, p_object, p_method, p_args);
	}
	_sim_queue_send(d, p_peer, p_object, p_method, p_args);
	return OK;
}

// Queue inner->rpc(peer,object,method,args) to fire after p_delay_ms, clamped so packets never
// overtake earlier ones (a pipe, not per-packet jitter): a late-released send fires no sooner than
// the last one queued. One cursor suffices — every send routes through here, so there's no separate
// reliable stream to order against.
void GoldNetMultiplayer::_sim_queue_send(int p_delay_ms, int32_t p_peer, Object *p_object, const StringName &p_method, const Array &p_args) {
	uint64_t fire_at = Time::get_singleton()->get_ticks_msec() + (uint64_t)p_delay_ms;
	if (fire_at < _last_fire_at) {
		fire_at = _last_fire_at;
	}
	_last_fire_at = fire_at;
	PendingSend ps;
	ps.fire_at_ms = fire_at;
	ps.peer = p_peer;
	ps.object_id = p_object->get_instance_id();
	ps.method = p_method;
	ps.args = p_args;
	_sim_queue.push_back(ps);
}

// Called every poll (client and server): advance the spike timer by the real elapsed delta and
// replay every queued send whose fire time has passed. A target freed during its delay is a no-op.
void GoldNetMultiplayer::_sim_pump(uint64_t p_now_ms) {
	float delta = _sim_last_poll_ms != 0 ? (float)(p_now_ms - _sim_last_poll_ms) / 1000.0f : 0.0f;
	_sim_last_poll_ms = p_now_ms;
	_sim_update_spike(delta);

	int i = 0;
	while (i < _sim_queue.size()) {
		if (_sim_queue[i].fire_at_ms <= p_now_ms) {
			PendingSend ps = _sim_queue[i];
			_sim_queue.remove_at(i);
			Object *o = ObjectDB::get_instance(ps.object_id);
			if (o) {
				inner->rpc(ps.peer, o, ps.method, ps.args);
			}
		} else {
			i++;
		}
	}
}

// Look p_method up in one rpc-config Dictionary (method name → per-method config carrying a
// "transfer_mode" MultiplayerPeer::TransferMode). Sets r_unreliable and returns true if the method
// is present; returns false (r_unreliable untouched) if absent or the config isn't a Dictionary.
static bool rpc_cfg_lookup(const Variant &p_config, const StringName &p_method, bool &r_unreliable) {
	if (p_config.get_type() != Variant::DICTIONARY) {
		return false;
	}
	Dictionary cfg = p_config;
	if (!cfg.has(p_method)) {
		return false;
	}
	Dictionary mc = cfg[p_method];
	int tm = (int)mc.get("transfer_mode", (int)MultiplayerPeer::TRANSFER_MODE_RELIABLE);
	r_unreliable = (tm == MultiplayerPeer::TRANSFER_MODE_UNRELIABLE ||
			tm == MultiplayerPeer::TRANSFER_MODE_UNRELIABLE_ORDERED);
	return true;
}

// Resolve whether an @rpc method is unreliable (loss-droppable), caching per (object, method). The
// mode lives in two places the engine merges: node-level runtime rpc_config() (an explicit per-node
// override) and the script's @rpc annotation config (the common case — WizardWars uses only these,
// and get_node_rpc_config() does NOT surface them, so the script lookup is essential). A method in
// neither, or a scriptless target, is treated as reliable → never dropped (safe default).
bool GoldNetMultiplayer::_rpc_is_unreliable(Object *p_object, const StringName &p_method) {
	uint64_t oid = p_object->get_instance_id();
	HashMap<uint64_t, HashMap<StringName, bool>>::Iterator oit = _rpc_unreliable_cache.find(oid);
	if (oit != _rpc_unreliable_cache.end()) {
		HashMap<StringName, bool>::Iterator mit = oit->value.find(p_method);
		if (mit != oit->value.end()) {
			return mit->value;
		}
	}

	bool unreliable = false;
	Node *n = Object::cast_to<Node>(p_object);
	// Node-level override takes priority; fall back to the script's annotation config.
	if (!(n && rpc_cfg_lookup(n->get_node_rpc_config(), p_method, unreliable))) {
		Ref<Script> scr = p_object->get_script();
		if (scr.is_valid()) {
			rpc_cfg_lookup(scr->get_rpc_config(), p_method, unreliable);
		}
	}
	_rpc_unreliable_cache[oid][p_method] = unreliable;
	return unreliable;
}

void GoldNetMultiplayer::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_snapshot_interval_ms", "ms"), &GoldNetMultiplayer::set_snapshot_interval_ms);
	ClassDB::bind_method(D_METHOD("get_snapshot_interval_ms"), &GoldNetMultiplayer::get_snapshot_interval_ms);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "snapshot_interval_ms"), "set_snapshot_interval_ms", "get_snapshot_interval_ms");
	ClassDB::bind_method(D_METHOD("set_debug_enabled", "enabled"), &GoldNetMultiplayer::set_debug_enabled);
	ClassDB::bind_method(D_METHOD("is_debug_enabled"), &GoldNetMultiplayer::is_debug_enabled);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debug_enabled"), "set_debug_enabled", "is_debug_enabled");
	ClassDB::bind_method(D_METHOD("set_loss_percent", "pct"), &GoldNetMultiplayer::set_loss_percent);
	ClassDB::bind_method(D_METHOD("get_loss_percent"), &GoldNetMultiplayer::get_loss_percent);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "loss_percent"), "set_loss_percent", "get_loss_percent");
	ClassDB::bind_method(D_METHOD("set_relevance_events", "enabled"), &GoldNetMultiplayer::set_relevance_events);
	ClassDB::bind_method(D_METHOD("get_relevance_events"), &GoldNetMultiplayer::get_relevance_events);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "relevance_events"), "set_relevance_events", "get_relevance_events");
	ClassDB::bind_method(D_METHOD("set_latency_min_ms", "ms"), &GoldNetMultiplayer::set_latency_min_ms);
	ClassDB::bind_method(D_METHOD("get_latency_min_ms"), &GoldNetMultiplayer::get_latency_min_ms);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "latency_min_ms"), "set_latency_min_ms", "get_latency_min_ms");
	ClassDB::bind_method(D_METHOD("set_latency_max_ms", "ms"), &GoldNetMultiplayer::set_latency_max_ms);
	ClassDB::bind_method(D_METHOD("get_latency_max_ms"), &GoldNetMultiplayer::get_latency_max_ms);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "latency_max_ms"), "set_latency_max_ms", "get_latency_max_ms");
	ClassDB::bind_method(D_METHOD("set_spike_ms", "ms"), &GoldNetMultiplayer::set_spike_ms);
	ClassDB::bind_method(D_METHOD("get_spike_ms"), &GoldNetMultiplayer::get_spike_ms);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "spike_ms"), "set_spike_ms", "get_spike_ms");
	ClassDB::bind_method(D_METHOD("set_spike_interval_s", "s"), &GoldNetMultiplayer::set_spike_interval_s);
	ClassDB::bind_method(D_METHOD("get_spike_interval_s"), &GoldNetMultiplayer::get_spike_interval_s);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "spike_interval_s"), "set_spike_interval_s", "get_spike_interval_s");
	ClassDB::bind_method(D_METHOD("set_spike_duration_s", "s"), &GoldNetMultiplayer::set_spike_duration_s);
	ClassDB::bind_method(D_METHOD("get_spike_duration_s"), &GoldNetMultiplayer::get_spike_duration_s);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "spike_duration_s"), "set_spike_duration_s", "get_spike_duration_s");
	ClassDB::bind_method(D_METHOD("sim_reset"), &GoldNetMultiplayer::sim_reset);
	ClassDB::bind_method(D_METHOD("capture_spawners"), &GoldNetMultiplayer::capture_spawners);
	// Emitted on a client when an owned MultiplayerSynchronizer leaves this peer's PVS (the server
	// stopped sending it). The game hides/deactivates the entity in response. Entry is not signalled —
	// a re-entering entity arrives as a full delta and fires the synchronizer's own `synchronized`.
	ADD_SIGNAL(MethodInfo("entity_relevance_lost", PropertyInfo(Variant::OBJECT, "sync")));
	// Emitted on a client for every received snapshot, carrying the server's send-time (ms, the header
	// clock). An always-on server-time feed regardless of entity traffic — feed it to a clock estimator
	// (see the ServerClock addon helper) instead of running a separate server-time beacon RPC.
	ADD_SIGNAL(MethodInfo("server_time_received", PropertyInfo(Variant::INT, "server_time_ms")));
}

// The ordered list of a synchronizer's *synced* replication-config property paths.
// This "slot" order (config order, sync props only) is identical on server and
// client — the changed-field bitmask indexes into it. Capped at 32 (u32 mask).
static void get_sync_slots(MultiplayerSynchronizer *p_sync, Vector<NodePath> &r_paths) {
	Ref<SceneReplicationConfig> cfg = p_sync->get_replication_config();
	if (cfg.is_null()) {
		return;
	}
	TypedArray<NodePath> props = cfg->get_properties();
	for (int i = 0; i < props.size() && r_paths.size() < MAX_SYNC_SLOTS; i++) {
		NodePath p = props[i];
		if (cfg->property_get_sync(p)) {
			r_paths.push_back(p);
		}
	}
}

// Read the current value of every slot of a synchronizer into r_vals (sized to the
// slot count; unresolved targets yield a nil Variant).
static void read_slot_values(MultiplayerSynchronizer *p_sync, const Vector<NodePath> &p_slots, Vector<Variant> &r_vals) {
	Node *root = p_sync->get_node_or_null(p_sync->get_root_path());
	r_vals.resize(p_slots.size());
	for (int s = 0; s < p_slots.size(); s++) {
		NodePath prop;
		Node *target = root ? resolve_property(root, p_slots[s], prop) : nullptr;
		r_vals.write[s] = target ? target->get_indexed(prop) : Variant();
	}
}

// Build the per-slot quantization tags from a synchronizer's "gn_quant" meta: a Dictionary keyed
// by the sync property's leaf name (e.g. "net_yaw") -> hint name ("angle16" | "half" | "vec3_half").
// Leaves r_quant empty when there's no meta or no recognized hint (the common case = all-auto).
void GoldNetMultiplayer::_read_quant(MultiplayerSynchronizer *p_sync, Vector<uint8_t> &r_quant) {
	r_quant.clear();
	const StringName meta_key("gn_quant");
	if (!p_sync->has_meta(meta_key)) {
		return;
	}
	Variant mv = p_sync->get_meta(meta_key);
	if (mv.get_type() != Variant::DICTIONARY) {
		return;
	}
	Dictionary hints = mv;
	Vector<NodePath> slots;
	get_sync_slots(p_sync, slots);
	Vector<uint8_t> q;
	q.resize(slots.size());
	bool any = false;
	for (int s = 0; s < slots.size(); s++) {
		uint8_t code = GN_Q_AUTO;
		int sc = slots[s].get_subname_count();
		if (sc > 0) {
			String leaf = slots[s].get_subname(sc - 1);
			if (hints.has(leaf)) {
				code = gn_quant_from_name(String(hints[leaf]));
			}
		}
		q.write[s] = code;
		if (code != GN_Q_AUTO) {
			any = true;
		}
	}
	if (any) {
		r_quant = q;
	}
}

void GoldNetMultiplayer::_server_tick() {
	GoldNetLink *l = _ensure_link();
	if (!l) {
		return;
	}
	PackedInt32Array peers = inner->get_peers();
	uint32_t now = (uint32_t)Time::get_singleton()->get_ticks_msec();

	// Read every owned entity's current state ONCE per tick — the values are the server's
	// authoritative state, identical for all peers. Only the delta mask (vs. each peer's
	// baseline) and the visibility filter below are peer-specific, so those stay in the
	// per-peer loop; the expensive get_indexed / path resolution does not.
	struct TickEnt {
		MultiplayerSynchronizer *sync;
		uint32_t net_id;
		Vector<Variant> vals;
		const Vector<uint8_t> *quant; // per-slot quantization tags (may be empty = all-auto)
	};
	Vector<TickEnt> ents;
	for (KeyValue<uint64_t, SyncEntry> &kv : owned_syncs) {
		MultiplayerSynchronizer *sync = sync_from_objid(kv.key);
		if (!sync || !sync->is_inside_tree() || !sync->is_multiplayer_authority()) {
			continue;
		}
		// Read the gn_quant hint once, on first tick — by now the game has set the meta.
		if (!kv.value.quant_read) {
			_read_quant(sync, kv.value.quant);
			kv.value.quant_read = true;
		}
		Vector<NodePath> slots;
		get_sync_slots(sync, slots);
		TickEnt e;
		e.sync = sync;
		e.net_id = kv.value.net_id;
		e.quant = &kv.value.quant;
		read_slot_values(sync, slots, e.vals);
		ents.push_back(e);
	}

	for (int pi = 0; pi < peers.size(); pi++) {
		int peer = peers[pi];
		PeerRing &pr = peer_rings[peer]; // default-constructs on first use

		// Baseline = the frame this peer last acked, if we still hold it. Otherwise
		// send a full frame (baseline seq 0) — first send, or an ack aged out of the
		// ring under heavy loss.
		uint16_t base_seq = 0;
		FrameData *base = nullptr;
		if (pr.has_ack) {
			int bslot = pr.last_acked & (RING - 1);
			if (pr.frame_seq[bslot] == pr.last_acked && pr.last_acked != 0) {
				base_seq = pr.last_acked;
				base = &pr.frames[bslot];
			}
		}

		uint16_t seq = pr.next_seq++;
		if (pr.next_seq == 0) {
			pr.next_seq = 1; // 0 is reserved for "no baseline"
		}

		// Spawns / despawns owed to this peer, reliable-until-acked (see _reliable_include).
		Ref<StreamPeerBuffer> spawn_body;
		spawn_body.instantiate();
		uint16_t spawn_ct = 0;
		for (const KeyValue<uint32_t, SpawnRecord> &kv : spawn_records) {
			if (pr.spawn_acked.has(kv.key)) {
				continue; // peer already has this node — don't re-arm the spawn every frame
			}
			// Lazy spawn (GoldSrc): defer sending the spawn to this peer until the entity is visible to
			// it — created client-side the first time it enters your PVS. Once delivered it PERSISTS
			// (spawn_acked keeps it; despawn only on server-free), so there's no create/destroy churn as
			// it moves in and out of view. A spawn with no synchronizers has no visibility info, so it
			// ships to everyone (fail open, matching the old always-spawn behaviour).
			if (!kv.value.sync_objids.is_empty()) {
				bool visible = false;
				for (int si = 0; si < kv.value.sync_objids.size(); si++) {
					MultiplayerSynchronizer *s = sync_from_objid(kv.value.sync_objids[si]);
					if (s && (s->is_visibility_public() || s->get_visibility_for(peer))) {
						visible = true;
						break;
					}
				}
				if (!visible) {
					continue; // not in this peer's PVS yet — defer the spawn
				}
			}
			// on_ack is the sole populator of spawn_acked: it retires+marks a record the moment
			// last_acked reaches it, before _reliable_include here could ever see it delivered.
			if (!_reliable_include(pr.spawn_wait, kv.key, seq, pr.last_acked, pr.has_ack)) {
				continue;
			}
			spawn_body->put_u32(kv.key);
			spawn_body->put_u32(kv.value.spawner_net_id);
			spawn_body->put_var(kv.value.data);
			spawn_ct++;
		}
		Ref<StreamPeerBuffer> despawn_body;
		despawn_body.instantiate();
		uint16_t despawn_ct = 0;
		for (KeyValue<uint32_t, HashSet<int32_t>> &kv : despawn_pending) {
			if (!kv.value.has(peer)) {
				continue;
			}
			if (!_reliable_include(pr.despawn_wait, kv.key, seq, pr.last_acked, pr.has_ack)) {
				kv.value.erase(peer); // delivered → this peer no longer needs it
				continue;
			}
			despawn_body->put_u32(kv.key);
			despawn_ct++;
		}

		// Per-peer entity delta: filter the pre-read entities by visibility, store the
		// visible subset as this peer's next baseline, and emit only the changed slots.
		FrameData frame;
		Ref<StreamPeerBuffer> body;
		body.instantiate();
		uint16_t changed = 0;
		for (int i = 0; i < ents.size(); i++) {
			MultiplayerSynchronizer *sync = ents[i].sync;
			// Per-peer PVS. The game drives each synchronizer's peer_visibility via set_visibility_for
			// once per net tick (NetworkManager.push_pvs_visibility), so this native read is the whole
			// gate — public_visibility for map-static "visible to all" entities, else the per-peer bit.
			// (We can't use MultiplayerSynchronizer's visibility *filters* — is_visible_to isn't bound.)
			if (!(sync->is_visibility_public() || sync->get_visibility_for(peer))) {
				continue;
			}
			uint32_t net_id = ents[i].net_id;
			const Vector<Variant> &vals = ents[i].vals;
			frame[net_id] = vals;

			const Vector<Variant> *bvals = base ? base->getptr(net_id) : nullptr;
			uint32_t mask = 0;
			if (!bvals || bvals->size() != vals.size()) {
				mask = vals.size() >= MAX_SYNC_SLOTS ? 0xFFFFFFFFu : ((1u << vals.size()) - 1u); // new → all slots
			} else {
				for (int s = 0; s < vals.size(); s++) {
					if (vals[s] != (*bvals)[s]) {
						mask |= (1u << s);
					}
				}
			}
			if (mask == 0) {
				continue; // unchanged since the acked baseline — costs nothing
			}
			body->put_u32(net_id);
			body->put_u32(mask);
			const Vector<uint8_t> &q = *ents[i].quant;
			for (int s = 0; s < vals.size(); s++) {
				if (mask & (1u << s)) {
					gn_put_value(body, vals[s], s < q.size() ? q[s] : GN_Q_AUTO);
				}
			}
			changed++;
		}

		// Relevance leaves (OPT-IN — off unless the consumer game enables relevance_events): owned_syncs
		// that were in this peer's PVS last tick but aren't in `frame` now, delivered reliable-until-acked
		// so the client can hide them. Enters need no event — an entering entity re-appears in the changed
		// set above with a full baseline, firing the synchronizer's `synchronized` signal the game already
		// listens to. Dropping the entity from `frame` when invisible also drops it from this peer's next
		// baseline, so re-entry is a clean all-slots delta (matching the client, which erases the same
		// net_id from its reconstructed frame on the leave). When disabled, no diff runs and leave_ct is 0
		// (2 bytes on the wire) — goldnet stays agnostic and games that don't opt in are unaffected.
		Ref<StreamPeerBuffer> leave_body;
		leave_body.instantiate();
		uint16_t leave_ct = 0;
		if (relevance_events_enabled && (pr.relevance_seeded || !frame.is_empty())) {
			// Seed on first contact with an EMPTY relevant-set: the client defaults each owned sync ABSENT
			// (GoldSrc-faithful — see EntityBase._setup_mover_sync) and materializes it only when its state
			// first arrives, so there is nothing to hide up front. This replaces the old "seed everything
			// relevant, then leave every out-of-PVS entity" scheme, whose first diff emitted a leave per
			// out-of-PVS entity — hundreds on a big map, overrunning the MTU so the snapshot was dropped and
			// never acked (the backlog then resent forever). Now leaves only fire for genuine PVS exits of
			// entities the client actually saw. The MAX_LEAVES_PER_SNAPSHOT cap remains as a backstop.
			pr.relevance_seeded = true;
			for (const uint32_t &rid : pr.relevant) {
				if (!frame.has(rid) && !pr.leave_wait.has(rid)) {
					pr.leave_wait[rid] = 0; // 0 = queued, not yet sent (seq 0 is reserved elsewhere)
				}
			}
			for (const KeyValue<uint32_t, Vector<Variant>> &kv : frame) {
				pr.leave_wait.erase(kv.key); // re-entered → cancel any pending leave
			}
			// Emit leaves bounded per snapshot: a fresh peer's seed can queue hundreds of leaves at once
			// (every out-of-PVS entity on a big map), and dumping them all in one frame blows past the MTU,
			// so the snapshot is dropped, never acked, and the same oversized frame resends forever —
			// nothing is ever delivered. Cap the count so each frame fits; the rest ride the next frames.
			// Stamp each SENT leave with the seq that actually carries it (not queue time), so a leave held
			// back by the cap isn't retired by an ack for a frame it was never in. Reliable-until-acked, so
			// spreading them out loses nothing — they drain over a few ticks.
			Vector<uint32_t> retired_leaves;
			for (KeyValue<uint32_t, uint16_t> &kv : pr.leave_wait) {
				if (kv.value != 0 && pr.has_ack && _seq_le(kv.value, pr.last_acked)) {
					retired_leaves.push_back(kv.key); // sent and acked → delivered
				} else if (leave_ct < MAX_LEAVES_PER_SNAPSHOT) {
					leave_body->put_u32(kv.key);
					leave_ct++;
					kv.value = seq; // stamp with the seq we're sending it in
				}
			}
			for (int ri = 0; ri < retired_leaves.size(); ri++) {
				pr.leave_wait.erase(retired_leaves[ri]);
			}
			pr.relevant.clear();
			for (const KeyValue<uint32_t, Vector<Variant>> &kv : frame) {
				pr.relevant.insert(kv.key);
			}
		}

		int slot = seq & (RING - 1);
		pr.frames[slot] = frame;
		pr.frame_seq[slot] = seq;

		Ref<StreamPeerBuffer> buf;
		buf.instantiate();
		buf->put_u32(GN_SNAPSHOT_MAGIC);
		buf->put_u16(seq);
		buf->put_u16(base_seq);
		buf->put_u32(now);
		buf->put_u16(spawn_ct);
		buf->put_data(spawn_body->get_data_array());
		buf->put_u16(despawn_ct);
		buf->put_data(despawn_body->get_data_array());
		buf->put_u16(leave_ct);
		buf->put_data(leave_body->get_data_array());
		buf->put_u16(changed);
		buf->put_data(body->get_data_array());
		PackedByteArray bytes = buf->get_data_array();

		// GOLDNET_LOSS=<pct> drops snapshots to exercise self-heal: the peer's ack
		// stalls, so the next frame keeps diffing against the same acked baseline and
		// re-carries whatever changed since — no desync, no retransmit. Surviving snapshots
		// route through the send-side sim (latency/spike) — this is one of goldnet's two send
		// funnels (see netsim_plan §3.4); the snapshot stream is unreliable.
		if (dbg_loss == 0 || (UtilityFunctions::randi() % 100) >= dbg_loss) {
			_sim_send(peer, l, "_gn_recv", Array::make(bytes));
		}
		dbg_bytes += bytes.size();
	}

	if (dbg && now - dbg_last_ms > 2000) {
		double kbps = (double)dbg_bytes / (double)(now - dbg_last_ms); // bytes/ms == KB/s
		UtilityFunctions::print("[goldnet] send owned=", (int)owned_syncs.size(),
				" peers=", peers.size(), " ~", kbps, " KB/s (all peers)");
		dbg_last_ms = now;
		dbg_bytes = 0;
	}
}

void GoldNetMultiplayer::apply_snapshot(const PackedByteArray &p_bytes) {
	Ref<StreamPeerBuffer> buf;
	buf.instantiate();
	buf->set_data_array(p_bytes);
	buf->seek(0);

	// Protocol guard: a mismatched goldnet build (different wire format) would misparse every field.
	// Fail loudly once and drop, rather than flooding the log with decode errors.
	if (buf->get_u32() != GN_SNAPSHOT_MAGIC) {
		if (!warned_protocol_mismatch) {
			warned_protocol_mismatch = true;
			UtilityFunctions::push_error("goldnet: snapshot protocol mismatch — the server and client are "
					"running different goldnet builds. Rebuild/redeploy both to the same version.");
		}
		return;
	}

	uint16_t seq = buf->get_u16();
	uint16_t base_seq = buf->get_u16();
	uint32_t server_time = buf->get_u32();

	// Drop stale/reordered snapshots (unreliable transport). We only advance forward.
	if (client_has && !_seq_newer(seq, client_last_seq)) {
		return;
	}

	// Surface the header clock: every snapshot the server ticks out carries its send-time, so this is an
	// always-on server-time feed (independent of entity traffic) a client can drive a clock estimator from
	// — no separate beacon RPC needed on the goldnet path. Emitted after the stale-seq drop (a reordered
	// snapshot's time is <= one we've already fed, so ServerClock would reject it — no point dispatching)
	// but before the baseline-resolution drop below, so a forward snapshot we can't reconstruct for state
	// still contributes its valid clock sample. See ServerClock.
	emit_signal("server_time_received", (int64_t)server_time);

	// Resolve the baseline frame we reconstruct this delta against. base_seq 0 means
	// full state. If the server diffed against a frame we no longer hold, we can't
	// reconstruct unchanged fields — re-ack our last good frame so the server falls
	// back to a baseline we have (eventually a full frame), and drop this one.
	FrameData *base = nullptr;
	if (base_seq != 0) {
		int bslot = base_seq & (RING - 1);
		if (client_frame_seq[bslot] == base_seq) {
			base = &client_frames[bslot];
		} else {
			if (client_has) {
				GoldNetLink *l = _ensure_link();
				if (l) {
					inner->rpc(1, l, "_gn_ack", Array::make((int)client_last_seq));
				}
			}
			return;
		}
	}

	// Spawns first: create the nodes so their child synchronizers register before the
	// entity deltas below reference them (add_child fires enter-tree synchronously).
	int spawn_ct = buf->get_u16();
	for (int i = 0; i < spawn_ct; i++) {
		uint32_t nid = buf->get_u32();
		uint32_t spawner_nid = buf->get_u32();
		Variant data = buf->get_var();
		_apply_spawn(nid, spawner_nid, data);
	}
	// Despawns: collect now (wire order), free after the entity deltas are applied.
	int despawn_ct = buf->get_u16();
	Vector<uint32_t> despawns;
	for (int i = 0; i < despawn_ct; i++) {
		despawns.push_back(buf->get_u32());
	}
	// Relevance leaves: owned_syncs that left this peer's PVS. Collect now (wire order),
	// apply after the entity deltas (drop from the reconstructed frame + signal the game).
	int leave_ct = buf->get_u16();
	Vector<uint32_t> leaves;
	for (int i = 0; i < leave_ct; i++) {
		leaves.push_back(buf->get_u32());
	}

	int count = buf->get_u16();

	// New frame = baseline carried forward, with this delta's changed fields applied.
	FrameData frame;
	if (base) {
		frame = *base;
	}

	// Set if a KNOWN entity (registered net_id, live object) couldn't be applied because its node
	// isn't in the tree yet — the recv-nodes warmup window. We must not ack this frame as delivered
	// (see the defer block after the loop), or the server folds the entity into this peer's baseline
	// and — for an idle mover whose state never changes again — never resends it, stranding it hidden.
	bool defer_ack = false;

	for (int i = 0; i < count; i++) {
		uint32_t net_id = buf->get_u32();
		uint32_t mask = buf->get_u32();

		MultiplayerSynchronizer *sync = nullptr;
		if (netid_to_objid.has(net_id)) {
			MultiplayerSynchronizer *s = sync_from_objid(netid_to_objid[net_id]);
			if (s && s->is_inside_tree()) {
				sync = s;
			} else if (s && mask != 0) {
				// Registered and still alive, but not in the tree yet: a warmup miss we should retry
				// rather than silently swallow. Freed/unknown ids fall through (s == null) — nothing
				// to wait for — so they don't defer and can't stall the stream.
				defer_ack = true;
			}
		}
		Vector<NodePath> slots;
		Node *root = nullptr;
		if (sync) {
			get_sync_slots(sync, slots);
			root = sync->get_node_or_null(sync->get_root_path());
		}

		// Start from this entity's baseline values (carried in `frame`), then overlay
		// the changed slots. Values are read for every set bit regardless of whether
		// we can resolve the node, so the stream stays aligned even for unknown ids.
		Vector<Variant> *cur = frame.getptr(net_id);
		Vector<Variant> vals;
		if (cur) {
			vals = *cur;
		}
		if (sync && vals.size() != slots.size()) {
			vals.resize(slots.size());
		}
		bool applied = false;
		for (int s = 0; s < MAX_SYNC_SLOTS; s++) {
			if (!(mask & (1u << s))) {
				continue;
			}
			Variant v = gn_get_value(buf); // self-delimiting — always consume
			if (s < vals.size()) {
				vals.write[s] = v;
			}
			if (sync && root && s < slots.size()) {
				NodePath prop;
				Node *target = resolve_property(root, slots[s], prop);
				if (target) {
					target->set_indexed(prop, v);
					applied = true;
				}
			}
		}
		if (!vals.is_empty()) {
			frame[net_id] = vals;
		}
		// The game reacts to replicated state in the synchronizer's `synchronized`
		// signal handler, so re-emit it (only when something actually changed).
		if (sync && applied) {
			sync->emit_signal("synchronized");
		}
	}

	// Now free despawned nodes (after entity deltas so nothing applies to a node we then
	// free — queue_free is deferred anyway, but this keeps the ordering clean).
	for (int i = 0; i < despawns.size(); i++) {
		_apply_despawn(despawns[i]);
	}

	// Drop entities no longer registered here (e.g. after a map change / despawn) so the
	// client baseline can't diverge from the server's, which only tracks live entities.
	{
		Vector<uint32_t> stale;
		for (const KeyValue<uint32_t, Vector<Variant>> &kv : frame) {
			if (!netid_to_objid.has(kv.key)) {
				stale.push_back(kv.key);
			}
		}
		for (int i = 0; i < stale.size(); i++) {
			frame.erase(stale[i]);
		}
	}

	// Relevance leaves: drop from the reconstructed frame (so it matches the server's baseline,
	// which also dropped these) and signal the game to hide the entity. Enter needs no signal — a
	// re-entering entity arrives as a full delta above and fires the synchronizer's `synchronized`.
	for (int i = 0; i < leaves.size(); i++) {
		uint32_t net_id = leaves[i];
		frame.erase(net_id);
		if (netid_to_objid.has(net_id)) {
			MultiplayerSynchronizer *s = sync_from_objid(netid_to_objid[net_id]);
			if (s) {
				emit_signal("entity_relevance_lost", s);
			}
		}
	}

	// Warmup defer: a known entity's node wasn't in the tree yet, so we couldn't apply (reveal) it.
	// Committing + acking this seq would advance the server's baseline past the entity; an idle mover
	// that never changes again would then never be resent — stranded hidden. Instead re-ack our last
	// good frame so the server keeps resending its full baseline against a frame we hold, and drop
	// this one uncommitted; we'll apply it once the node finishes entering the tree (usually 1-2
	// frames). Capped so a node that never resolves can't stall the stream forever. Needs a prior good
	// frame to fall back to (client_has); on the very first frame we just accept it.
	if (defer_ack && client_has && defer_streak < MAX_DEFER_STREAK) {
		defer_streak++;
		GoldNetLink *dl = _ensure_link();
		if (dl) {
			inner->rpc(1, dl, "_gn_ack", Array::make((int)client_last_seq));
		}
		return;
	}
	defer_streak = 0;

	// Commit this frame to the client ring and ack it (newest wins, unreliable).
	int slot = seq & (RING - 1);
	client_frames[slot] = frame;
	client_frame_seq[slot] = seq;
	client_last_seq = seq;
	client_has = true;

	GoldNetLink *l = _ensure_link();
	if (l) {
		inner->rpc(1, l, "_gn_ack", Array::make((int)seq));
	}

	if (dbg) {
		uint64_t now = Time::get_singleton()->get_ticks_msec();
		if (now - dbg_last_ms > 2000) {
			dbg_last_ms = now;
			UtilityFunctions::print("[goldnet] recv seq=", seq, " base=", base_seq,
					" spawn=", spawn_ct, " despawn=", despawn_ct,
					" changed=", count, " bytes=", (int)p_bytes.size(),
					" registered=", (int)owned_syncs.size());
		}
	}
}

void GoldNetMultiplayer::on_ack(int32_t p_peer, int32_t p_seq) {
	uint16_t seq = (uint16_t)p_seq;
	PeerRing *pr = peer_rings.getptr(p_peer);
	if (!pr) {
		return;
	}
	if (!pr->has_ack || _seq_newer(seq, pr->last_acked)) {
		pr->last_acked = seq;
		pr->has_ack = true;
	}
	// Retire spawn/despawn records this ack confirms delivered.
	Vector<uint32_t> retired;
	_retire_acked(pr->spawn_wait, pr->last_acked, retired);
	for (int i = 0; i < retired.size(); i++) {
		pr->spawn_acked.insert(retired[i]); // durably remember delivery (spawn source record persists)
	}
	retired.clear();
	_retire_acked(pr->despawn_wait, pr->last_acked, retired);
	for (int i = 0; i < retired.size(); i++) {
		uint32_t nid = retired[i];
		HashSet<int32_t> *needers = despawn_pending.getptr(nid);
		if (needers) {
			needers->erase(p_peer);
			if (needers->is_empty()) {
				despawn_pending.erase(nid); // all peers have it — done
			}
		}
	}
}

Error GoldNetMultiplayer::_poll() {
	Error e = inner->poll();
	_ensure_link();
	uint64_t now = Time::get_singleton()->get_ticks_msec();
	// Pump the send-side sim every poll on BOTH ends: a client delays its input RPCs here (the
	// client→server leg), a server its snapshots + RPCs (server→client). Also advances the spike timer.
	_sim_pump(now);
	if (inner->get_unique_id() == 1) {
		_drain_pending_spawns(); // every poll — pick up spawns promptly
		_detect_despawns();
		if (inner->get_peers().size() > 0) {
			if (now - last_send_ms >= cached_min_interval_ms) {
				last_send_ms = now;
				_server_tick();
			}
		}
	}
	return e;
}

void GoldNetMultiplayer::_set_multiplayer_peer(const Ref<MultiplayerPeer> &p_peer) {
	// Backstop: arm spawner capture the moment a session starts, so a game that spawns after setting the
	// peer (but before our first poll) doesn't need to call capture_spawners() itself. Idempotent.
	capture_spawners();
	inner->set_multiplayer_peer(p_peer);
}

Ref<MultiplayerPeer> GoldNetMultiplayer::_get_multiplayer_peer() {
	return inner->get_multiplayer_peer();
}

int32_t GoldNetMultiplayer::_get_unique_id() const {
	return inner->get_unique_id();
}

PackedInt32Array GoldNetMultiplayer::_get_peer_ids() const {
	return inner->get_peers();
}

Error GoldNetMultiplayer::_rpc(int32_t p_peer, Object *p_object, const StringName &p_method, const Array &p_args) {
	// The single chokepoint every game @rpc passes through — goldnet's other send funnel. Apply
	// send-side loss (unreliable only) + latency/spike here; the game can't observe the few-ms
	// deferral (same contract the old net_latency_sim.gd send wrappers had). Loss drops only
	// UNRELIABLE RPCs — dropping a reliable one would stop ENet retransmitting it and desync state
	// (netsim_plan §4a). This is what lets /sim_loss exercise the input-command redundancy ring.
	if (dbg_loss > 0 && _rpc_is_unreliable(p_object, p_method) &&
			(int)((uint32_t)UtilityFunctions::randi() % 100) < dbg_loss) {
		return OK; // simulated drop
	}
	return _sim_send(p_peer, p_object, p_method, p_args);
}

int32_t GoldNetMultiplayer::_get_remote_sender_id() const {
	return inner->get_remote_sender_id();
}

Error GoldNetMultiplayer::_object_configuration_add(Object *p_object, const Variant &p_config) {
	// A spawn is reported to the MultiplayerAPI as object_configuration_add(spawned_node,
	// spawner). We already learned the reconstruction data from the wrapped spawn_function
	// (see _wrap_spawner / _spawn_trampoline), so nothing to do here but claim it — do not
	// forward to the inner.
	if (Object::cast_to<MultiplayerSpawner>(p_config)) {
		return OK;
	}

	MultiplayerSynchronizer *sync = Object::cast_to<MultiplayerSynchronizer>(p_config);
	if (sync && _should_intercept(sync)) {
		uint64_t objid = sync->get_instance_id();
		SyncEntry entry;
		entry.net_id = net_id_for(sync);
		owned_syncs[objid] = entry;
		netid_to_objid[entry.net_id] = objid;
		cached_min_interval_ms = _min_interval_ms(); // intervals are set before the sync enters the tree
		return OK; // we own it — do not forward to the inner
	}
	return inner->object_configuration_add(p_object, p_config);
}

Error GoldNetMultiplayer::_object_configuration_remove(Object *p_object, const Variant &p_config) {
	// Despawn is reported as object_configuration_remove(node, spawner); we detect the freed
	// node by polling instead (a node can be freed without this firing), so just claim it.
	if (Object::cast_to<MultiplayerSpawner>(p_config)) {
		return OK;
	}

	MultiplayerSynchronizer *sync = Object::cast_to<MultiplayerSynchronizer>(p_config);
	if (sync) {
		uint64_t objid = sync->get_instance_id();
		if (owned_syncs.has(objid)) {
			netid_to_objid.erase(owned_syncs[objid].net_id);
			owned_syncs.erase(objid);
			cached_min_interval_ms = _min_interval_ms();
			return OK;
		}
	}
	return inner->object_configuration_remove(p_object, p_config);
}
