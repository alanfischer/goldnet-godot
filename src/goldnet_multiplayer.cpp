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

using namespace godot;

// Default snapshot cadence when a synchronizer reports no interval (30 Hz).
static const uint32_t DEFAULT_INTERVAL_MS = 33;

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

GoldNetMultiplayer::GoldNetMultiplayer() {
	dbg = getenv("GOLDNET_DEBUG") != nullptr;
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

void GoldNetMultiplayer::_relay_peer_connected(int64_t p_id) { emit_signal("peer_connected", p_id); }
void GoldNetMultiplayer::_relay_peer_disconnected(int64_t p_id) { emit_signal("peer_disconnected", p_id); }
void GoldNetMultiplayer::_relay_connected_to_server() { emit_signal("connected_to_server"); }
void GoldNetMultiplayer::_relay_connection_failed() { emit_signal("connection_failed"); }
void GoldNetMultiplayer::_relay_server_disconnected() { emit_signal("server_disconnected"); }

// A synchronizer we can fully own. We intercept map-static synchronizers (doors,
// platforms — present in the loaded scene identically on both peers) and leave
// spawner-managed ones (players, projectiles) on the inner: their spawn identity
// rides the inner's MultiplayerSpawner, which Phase 3 will fold into our stream.
//
// The discriminator is *not* the config's spawn flag — add_property() defaults spawn
// to true, so every runtime-built config (including the movers) has it. Instead we
// ask whether the synchronizer's owner node is a direct child of a spawner's
// spawn-parent: spawned nodes land directly under the spawn_path node (Main), while
// map entities are deeper in the scene (Main/<map>/<entity>).
bool GoldNetMultiplayer::_should_intercept(MultiplayerSynchronizer *p_sync) const {
	Ref<SceneReplicationConfig> cfg = p_sync->get_replication_config();
	if (cfg.is_null() || cfg->get_properties().is_empty()) {
		return false;
	}
	Node *owner = p_sync->get_node_or_null(p_sync->get_root_path());
	if (!owner) {
		return false;
	}
	Node *parent = owner->get_parent();
	if (parent && spawner_parents.has(parent->get_instance_id())) {
		return false; // spawner-managed — leave to the inner
	}
	return true;
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
	}
	return l;
}

// Resolve a synchronizer from its stable ObjectID, returning null if it has been
// freed (godot-cpp has no is_instance_valid; ObjectDB::get_instance does the check).
static MultiplayerSynchronizer *sync_from_objid(uint64_t p_objid) {
	return Object::cast_to<MultiplayerSynchronizer>(ObjectDB::get_instance(p_objid));
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

void GoldNetMultiplayer::_write_entity(StreamPeerBuffer *p_buf, MultiplayerSynchronizer *sync, uint32_t p_net_id) {
	Node *root = sync->get_node_or_null(sync->get_root_path());
	Ref<SceneReplicationConfig> cfg = sync->get_replication_config();
	TypedArray<NodePath> props = cfg->get_properties();

	// Collect (index, value) for synced properties, then write. Index is the
	// property's position in the config list — identical on server & client.
	Vector<int> idxs;
	Vector<Variant> vals;
	for (int i = 0; i < props.size(); i++) {
		NodePath cp = props[i];
		if (!cfg->property_get_sync(cp)) {
			continue;
		}
		NodePath prop;
		Node *target = root ? resolve_property(root, cp, prop) : nullptr;
		if (!target) {
			continue;
		}
		idxs.push_back(i);
		vals.push_back(target->get_indexed(prop));
	}

	p_buf->put_u32(p_net_id);
	p_buf->put_u8((uint8_t)idxs.size());
	for (int i = 0; i < idxs.size(); i++) {
		p_buf->put_u8((uint8_t)idxs[i]);
		p_buf->put_var(vals[i]);
	}
}

void GoldNetMultiplayer::_server_tick() {
	GoldNetLink *l = _ensure_link();
	if (!l) {
		return;
	}
	PackedInt32Array peers = inner->get_peers();
	uint32_t now = (uint32_t)Time::get_singleton()->get_ticks_msec();

	struct Vis {
		MultiplayerSynchronizer *sync;
		uint32_t net_id;
	};

	for (int pi = 0; pi < peers.size(); pi++) {
		int peer = peers[pi];

		// Gather the entities visible to this peer.
		Vector<Vis> visible;
		for (const KeyValue<uint64_t, SyncEntry> &kv : owned_syncs) {
			MultiplayerSynchronizer *sync = sync_from_objid(kv.key);
			if (!sync || !sync->is_inside_tree()) {
				continue;
			}
			if (!sync->is_multiplayer_authority()) {
				continue; // only the authority streams its state
			}
			// Honor the exposed visibility API: public flag OR an explicit per-peer
			// set_visibility_for override. Godot does NOT bind is_visible_to, so a
			// GDExtension MultiplayerAPI cannot evaluate add_visibility_filter()
			// callbacks (WizardWars' BSP-PVS cull rides those). We therefore stream
			// to any peer the node is publicly visible to — correctness-safe here
			// because a client silently drops net_ids it hasn't registered yet and
			// self-heals once its map loads (no path cache to poison). The lost PVS
			// cull is a bandwidth-only regression that Phase 2's delta makes moot for
			// the static movers that dominate the set. See docs / follow-up.
			if (!(sync->is_visibility_public() || sync->get_visibility_for(peer))) {
				continue;
			}
			visible.push_back({ sync, kv.value.net_id });
		}

		Ref<StreamPeerBuffer> buf;
		buf.instantiate();
		buf->put_u32(now);
		buf->put_u16((uint16_t)visible.size());
		for (int i = 0; i < visible.size(); i++) {
			_write_entity(buf.ptr(), visible[i].sync, visible[i].net_id);
		}
		PackedByteArray bytes = buf->get_data_array();
		inner->rpc(peer, l, "_gn_recv", Array::make(bytes));
		if (dbg && now - dbg_last_ms > 2000) {
			UtilityFunctions::print("[goldnet] send peer=", peer, " ents=", (int)visible.size(),
					" owned=", (int)owned_syncs.size(), " bytes=", (int)bytes.size());
		}
	}
	if (dbg && now - dbg_last_ms > 2000) {
		dbg_last_ms = now;
	}
}

void GoldNetMultiplayer::apply_snapshot(const PackedByteArray &p_bytes) {
	Ref<StreamPeerBuffer> buf;
	buf.instantiate();
	buf->set_data_array(p_bytes);
	buf->seek(0);

	uint32_t count_time = buf->get_u32(); // server_time_ms (the game carries its own
	(void)count_time;                     // per-entity stamp prop; header kept for Phase 2)
	int count = buf->get_u16();

	int matched = 0;
	int applied_ct = 0;

	for (int i = 0; i < count; i++) {
		uint32_t net_id = buf->get_u32();
		int pc = buf->get_u8();

		MultiplayerSynchronizer *sync = nullptr;
		if (netid_to_objid.has(net_id)) {
			uint64_t objid = netid_to_objid[net_id];
			MultiplayerSynchronizer *s = sync_from_objid(objid);
			if (s && s->is_inside_tree()) {
				sync = s;
			}
		}

		Node *root = nullptr;
		TypedArray<NodePath> props;
		if (sync) {
			root = sync->get_node_or_null(sync->get_root_path());
			Ref<SceneReplicationConfig> cfg = sync->get_replication_config();
			if (cfg.is_valid()) {
				props = cfg->get_properties();
			}
		}

		bool applied = false;
		for (int j = 0; j < pc; j++) {
			int idx = buf->get_u8();
			Variant v = buf->get_var(); // self-delimiting — always consume
			if (sync && root && idx >= 0 && idx < props.size()) {
				NodePath prop;
				Node *target = resolve_property(root, props[idx], prop);
				if (target) {
					target->set_indexed(prop, v);
					applied = true;
				}
			}
		}
		// The game applies replicated state by reacting to the synchronizer's
		// `synchronized` signal (it reads the net_* props there and feeds its interp
		// buffer), so writing the props isn't enough — re-emit it ourselves.
		if (sync && applied) {
			sync->emit_signal("synchronized");
			applied_ct++;
		}
		if (sync) {
			matched++;
		}
	}
	if (dbg) {
		uint64_t now = Time::get_singleton()->get_ticks_msec();
		if (now - dbg_last_ms > 2000) {
			dbg_last_ms = now;
			UtilityFunctions::print("[goldnet] recv ents=", count, " matched=", matched,
					" applied=", applied_ct, " registered=", (int)owned_syncs.size());
		}
	}
}

Error GoldNetMultiplayer::_poll() {
	Error e = inner->poll();
	_ensure_link();
	if (inner->get_unique_id() == 1 && inner->get_peers().size() > 0) {
		uint64_t now = Time::get_singleton()->get_ticks_msec();
		if (now - last_send_ms >= _min_interval_ms()) {
			last_send_ms = now;
			_server_tick();
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
	// Track spawners so _should_intercept can tell spawner-managed nodes from
	// map-static ones. Always forward the spawner itself to the inner (it still owns
	// spawn/despawn in Phase 1).
	MultiplayerSpawner *spawner = Object::cast_to<MultiplayerSpawner>(p_config);
	if (spawner) {
		Node *parent = spawner->get_node_or_null(spawner->get_spawn_path());
		if (parent) {
			spawner_parents[parent->get_instance_id()] = spawner->get_instance_id();
		}
		return inner->object_configuration_add(p_object, p_config);
	}

	MultiplayerSynchronizer *sync = Object::cast_to<MultiplayerSynchronizer>(p_config);
	if (sync && _should_intercept(sync)) {
		uint64_t objid = sync->get_instance_id();
		SyncEntry entry;
		entry.net_id = (uint32_t)String(sync->get_path()).hash();
		owned_syncs[objid] = entry;
		netid_to_objid[entry.net_id] = objid;
		return OK; // we own it — do not forward to the inner
	}
	return inner->object_configuration_add(p_object, p_config);
}

Error GoldNetMultiplayer::_object_configuration_remove(Object *p_object, const Variant &p_config) {
	MultiplayerSpawner *spawner = Object::cast_to<MultiplayerSpawner>(p_config);
	if (spawner) {
		Node *parent = spawner->get_node_or_null(spawner->get_spawn_path());
		if (parent) {
			spawner_parents.erase(parent->get_instance_id());
		}
		return inner->object_configuration_remove(p_object, p_config);
	}

	MultiplayerSynchronizer *sync = Object::cast_to<MultiplayerSynchronizer>(p_config);
	if (sync) {
		uint64_t objid = sync->get_instance_id();
		if (owned_syncs.has(objid)) {
			netid_to_objid.erase(owned_syncs[objid].net_id);
			owned_syncs.erase(objid);
			return OK;
		}
	}
	return inner->object_configuration_remove(p_object, p_config);
}
