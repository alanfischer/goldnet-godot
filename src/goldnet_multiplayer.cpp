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
}

void GoldNetMultiplayer::_relay_peer_connected(int64_t p_id) { emit_signal("peer_connected", p_id); }
void GoldNetMultiplayer::_relay_peer_disconnected(int64_t p_id) {
	peer_rings.erase((int32_t)p_id); // drop the server-side send history for this peer
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
	for (int i = 0; i < props.size() && r_paths.size() < 32; i++) {
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

	struct Vis {
		MultiplayerSynchronizer *sync;
		uint32_t net_id;
	};

	for (int pi = 0; pi < peers.size(); pi++) {
		int peer = peers[pi];
		PeerRing &pr = peer_rings[peer]; // default-constructs on first use

		// Gather the entities visible to this peer (see Phase-1 visibility note:
		// we honor the exposed public/explicit visibility, not filter callbacks).
		Vector<Vis> visible;
		for (const KeyValue<uint64_t, SyncEntry> &kv : owned_syncs) {
			MultiplayerSynchronizer *sync = sync_from_objid(kv.key);
			if (!sync || !sync->is_inside_tree() || !sync->is_multiplayer_authority()) {
				continue;
			}
			if (!(sync->is_visibility_public() || sync->get_visibility_for(peer))) {
				continue;
			}
			visible.push_back({ sync, kv.value.net_id });
		}

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

		// Build this frame's full value set (stored as the next baseline) and, in
		// parallel, the delta records against `base`.
		FrameData frame;
		Ref<StreamPeerBuffer> body;
		body.instantiate();
		uint16_t changed = 0;
		for (int i = 0; i < visible.size(); i++) {
			MultiplayerSynchronizer *sync = visible[i].sync;
			uint32_t net_id = visible[i].net_id;
			Vector<NodePath> slots;
			get_sync_slots(sync, slots);
			Vector<Variant> vals;
			read_slot_values(sync, slots, vals);
			frame[net_id] = vals;

			const Vector<Variant> *bvals = base ? base->getptr(net_id) : nullptr;
			uint32_t mask = 0;
			if (!bvals || bvals->size() != vals.size()) {
				mask = slots.size() >= 32 ? 0xFFFFFFFFu : ((1u << slots.size()) - 1u); // new → all slots
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
					body->put_var(vals[s]);
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
	int count = buf->get_u16();

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
		for (int s = 0; s < 32; s++) {
			if (!(mask & (1u << s))) {
				continue;
			}
			Variant v = buf->get_var(); // self-delimiting — always consume
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

	// Drop entities no longer registered here (e.g. after a map change) so the client
	// baseline can't diverge from the server's, which only tracks live entities.
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
