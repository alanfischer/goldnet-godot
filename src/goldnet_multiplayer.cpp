#include "goldnet_multiplayer.h"
#include "goldnet_link.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/multiplayer_spawner.hpp>
#include <godot_cpp/classes/scene_multiplayer.hpp>
#include <godot_cpp/classes/scene_replication_config.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/stream_peer_buffer.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/vector3.hpp>

using namespace godot;

// Default snapshot cadence when a synchronizer reports no interval (30 Hz).
static const uint32_t DEFAULT_INTERVAL_MS = 33;

// Max sync properties per entity — the width of the u32 changed-field bitmask. Slot
// collection, mask building, and the apply loop all key off this.
static const int MAX_SYNC_SLOTS = 32;

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
enum GNValueTag : uint8_t { GN_T_VAR = 0, GN_T_FLOAT = 1, GN_T_INT = 2, GN_T_VEC3 = 3, GN_T_BOOL = 4 };

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

static void gn_put_value(const Ref<StreamPeerBuffer> &buf, const Variant &v) {
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
	// One-time: catch spawners already in the tree (created during _ready, before our first
	// poll) and every spawner added afterward, so their spawn_function is wrapped before any
	// spawn() call.
	if (!spawners_scanned) {
		spawners_scanned = true;
		tree->connect("node_added", callable_mp(this, &GoldNetMultiplayer::_on_node_added));
		_scan_spawners();
	}
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
		spawn_records[net_id] = rec;
		despawn_pending.erase(net_id); // a reused net_id is a fresh spawn, not a despawn
		done.push_back(kv.key);
	}
	for (int i = 0; i < done.size(); i++) {
		pending_spawns.erase(done[i]);
	}
}

// Server: a spawned node that has been freed becomes a despawn owed to every current peer.
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
	PackedInt32Array peers = inner->get_peers();
	for (int i = 0; i < gone.size(); i++) {
		uint32_t net_id = gone[i];
		spawn_records.erase(net_id);
		HashSet<int32_t> needers;
		for (int p = 0; p < peers.size(); p++) {
			needers.insert(peers[p]);
		}
		if (!needers.is_empty()) {
			despawn_pending[net_id] = needers;
		}
		for (KeyValue<int32_t, PeerRing> &pr : peer_rings) {
			pr.value.spawn_wait.erase(net_id);  // stop resending the (now void) spawn
			pr.value.spawn_acked.erase(net_id); // and forget delivery, so a reused id re-spawns cleanly
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
	};
	Vector<TickEnt> ents;
	for (const KeyValue<uint64_t, SyncEntry> &kv : owned_syncs) {
		MultiplayerSynchronizer *sync = sync_from_objid(kv.key);
		if (!sync || !sync->is_inside_tree() || !sync->is_multiplayer_authority()) {
			continue;
		}
		Vector<NodePath> slots;
		get_sync_slots(sync, slots);
		TickEnt e;
		e.sync = sync;
		e.net_id = kv.value.net_id;
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
			for (int s = 0; s < vals.size(); s++) {
				if (mask & (1u << s)) {
					gn_put_value(body, vals[s]);
				}
			}
			changed++;
		}

		int slot = seq & (RING - 1);
		pr.frames[slot] = frame;
		pr.frame_seq[slot] = seq;

		Ref<StreamPeerBuffer> buf;
		buf.instantiate();
		buf->put_u16(seq);
		buf->put_u16(base_seq);
		buf->put_u32(now);
		buf->put_u16(spawn_ct);
		buf->put_data(spawn_body->get_data_array());
		buf->put_u16(despawn_ct);
		buf->put_data(despawn_body->get_data_array());
		buf->put_u16(changed);
		buf->put_data(body->get_data_array());
		PackedByteArray bytes = buf->get_data_array();

		// GOLDNET_LOSS=<pct> drops snapshots to exercise self-heal: the peer's ack
		// stalls, so the next frame keeps diffing against the same acked baseline and
		// re-carries whatever changed since — no desync, no retransmit.
		if (dbg_loss == 0 || (UtilityFunctions::randi() % 100) >= dbg_loss) {
			inner->rpc(peer, l, "_gn_recv", Array::make(bytes));
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

	uint16_t seq = buf->get_u16();
	uint16_t base_seq = buf->get_u16();
	uint32_t server_time = buf->get_u32(); // header (game carries its own per-entity stamp prop)
	(void)server_time;

	// Drop stale/reordered snapshots (unreliable transport). We only advance forward.
	if (client_has && !_seq_newer(seq, client_last_seq)) {
		return;
	}

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

	int count = buf->get_u16();

	// New frame = baseline carried forward, with this delta's changed fields applied.
	FrameData frame;
	if (base) {
		frame = *base;
	}

	for (int i = 0; i < count; i++) {
		uint32_t net_id = buf->get_u32();
		uint32_t mask = buf->get_u32();

		MultiplayerSynchronizer *sync = nullptr;
		if (netid_to_objid.has(net_id)) {
			MultiplayerSynchronizer *s = sync_from_objid(netid_to_objid[net_id]);
			if (s && s->is_inside_tree()) {
				sync = s;
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
	if (inner->get_unique_id() == 1) {
		_drain_pending_spawns(); // every poll — pick up spawns promptly
		_detect_despawns();
		if (inner->get_peers().size() > 0) {
			uint64_t now = Time::get_singleton()->get_ticks_msec();
			if (now - last_send_ms >= cached_min_interval_ms) {
				last_send_ms = now;
				_server_tick();
			}
		}
	}
	return e;
}

void GoldNetMultiplayer::_set_multiplayer_peer(const Ref<MultiplayerPeer> &p_peer) {
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
	return inner->rpc(p_peer, p_object, p_method, p_args);
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
