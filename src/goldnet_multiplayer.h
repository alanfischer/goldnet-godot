#pragma once

// GoldNetMultiplayer — a drop-in Godot MultiplayerAPI implementing GoldSrc-style
// snapshot replication behind the stock MultiplayerSynchronizer / MultiplayerSpawner
// API.
//
// COMPOSITION: we hold an inner SceneMultiplayer and delegate handshake, auth, peer
// lifecycle, and ALL @rpc traffic to it. We replace only the *state replication*
// layer.
//
// PHASE 0: pure pass-through — every virtual forwarded to the inner.
// PHASE 1 (this file): we intercept the synchronizers we can fully own — those whose
//   replication config is *sync-only* (no spawn-marked properties) — into our own
//   registry, and stream their full state ourselves each tick as an unreliable
//   snapshot (via GoldNetLink), honoring the synchronizer's visibility. Synchronizers
//   with spawn-marked properties stay coupled to the inner's spawner (Phase 3 folds
//   spawns into our stream), as do MultiplayerSpawners and all RPCs.
//   This is "old netcode (ALWAYS full-state) on our path" — parity, not yet the delta.

#include <godot_cpp/classes/multiplayer_api.hpp>
#include <godot_cpp/classes/multiplayer_api_extension.hpp>
#include <godot_cpp/classes/multiplayer_peer.hpp>
#include <godot_cpp/classes/multiplayer_synchronizer.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>

namespace godot {

class GoldNetLink;

class GoldNetMultiplayer : public MultiplayerAPIExtension {
	GDCLASS(GoldNetMultiplayer, MultiplayerAPIExtension)

	// The engine's stock SceneMultiplayer (MultiplayerAPI::create_default_interface()).
	// Delegate for everything except the state-replication of intercepted synchronizers.
	Ref<MultiplayerAPI> inner;

	// Synchronizers we fully own (sync-only config). Keyed by the synchronizer's stable
	// ObjectID; net_id is a hash of its scene path (identical on server & client, since
	// the same node lives at the same path on both — map entities and spawner-created
	// nodes alike).
	struct SyncEntry {
		uint32_t net_id = 0;
	};
	HashMap<uint64_t, SyncEntry> owned_syncs;    // ObjectID -> entry
	HashMap<uint32_t, uint64_t> netid_to_objid;  // net_id   -> ObjectID (client apply lookup)

	// ObjectIDs of the nodes MultiplayerSpawners spawn *into* (their resolved
	// spawn_path). A synchronizer whose owner node is a direct child of one of these
	// is spawner-managed (players, projectiles) — its spawn identity rides the inner's
	// spawner, so we leave it (and its state) on the inner until Phase 3. Everything
	// else (map-static movers) we intercept.
	HashMap<uint64_t, uint64_t> spawner_parents;  // spawn-parent ObjectID -> spawner ObjectID

	uint64_t last_send_ms = 0;                   // server send throttle
	uint64_t dbg_last_ms = 0;                    // throttle for the GOLDNET_DEBUG stats print
	bool dbg = false;                            // GOLDNET_DEBUG=1 → periodic snapshot stats

	// Re-emit the inner API's lifecycle signals on ourselves (the game listens on us).
	void _relay_peer_connected(int64_t p_id);
	void _relay_peer_disconnected(int64_t p_id);
	void _relay_connected_to_server();
	void _relay_connection_failed();
	void _relay_server_disconnected();

	// Phase 1 internals.
	bool _should_intercept(MultiplayerSynchronizer *p_sync) const;  // map-static, not spawner-managed
	GoldNetLink *_ensure_link();                                 // create/find /root/__GoldNetLink
	void _server_tick();                                         // build + send snapshots
	void _write_entity(class StreamPeerBuffer *p_buf, MultiplayerSynchronizer *p_sync, uint32_t p_net_id);
	uint32_t _min_interval_ms() const;

protected:
	static void _bind_methods() {}

public:
	GoldNetMultiplayer();
	~GoldNetMultiplayer();

	// Client-side snapshot apply (called by GoldNetLink::_gn_recv).
	void apply_snapshot(const PackedByteArray &p_bytes);

	// MultiplayerAPIExtension virtuals.
	virtual Error _poll() override;
	virtual void _set_multiplayer_peer(const Ref<MultiplayerPeer> &p_peer) override;
	virtual Ref<MultiplayerPeer> _get_multiplayer_peer() override;
	virtual int32_t _get_unique_id() const override;
	virtual PackedInt32Array _get_peer_ids() const override;
	virtual Error _rpc(int32_t p_peer, Object *p_object, const StringName &p_method, const Array &p_args) override;
	virtual int32_t _get_remote_sender_id() const override;
	virtual Error _object_configuration_add(Object *p_object, const Variant &p_config) override;
	virtual Error _object_configuration_remove(Object *p_object, const Variant &p_config) override;
};

} // namespace godot
