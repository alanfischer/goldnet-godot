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
#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/variant/callable.hpp>
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

	// --- Phase 3: spawn / despawn (agnostic — projectiles & players stream like movers) ---
	// We own every MultiplayerSpawner too, so runtime entities flow through this stream
	// instead of the inner. Spawns and their STATE use independent hash-id spaces that both
	// match across peers: the spawn record is keyed by the spawned NODE's path hash; its
	// child synchronizer registers under its OWN path hash and streams state exactly like a
	// map mover. Creating the node on the client makes the child synchronizer auto-register;
	// freeing it auto-unregisters. Nothing distinguishes a mover from a projectile downstream.
	struct SpawnerEntry {
		uint32_t net_id = 0; // hash of the spawner's path
		Callable orig_fn;    // the game's real spawn_function (we wrap it to capture spawn data)
	};
	HashMap<uint64_t, SpawnerEntry> spawners;       // spawner ObjectID -> entry
	HashMap<uint32_t, uint64_t> spawner_netid_to_objid;

	// Server: spawn data captured by the trampoline before the node is in the tree.
	struct PendingSpawn {
		uint64_t spawner_objid = 0;
		Variant data;
	};
	HashMap<uint64_t, PendingSpawn> pending_spawns; // spawned-node ObjectID -> captured data

	// Server: live spawn records (one per currently-spawned runtime entity).
	struct SpawnRecord {
		uint32_t spawner_net_id = 0;
		uint64_t node_objid = 0;   // to poll-detect despawn (node freed)
		Variant data;
	};
	HashMap<uint32_t, SpawnRecord> spawn_records;   // node net_id -> record

	// Server: despawns awaiting delivery. Value = the peers that still need it (reliable-
	// until-acked); erased once every such peer acks a frame carrying it.
	HashMap<uint32_t, HashSet<int32_t>> despawn_pending; // node net_id -> peers still needing it

	// Client: nodes we spawned, so a despawn can free the right one. Also lets us emit the
	// spawner's spawned/despawned signals (the game hangs bookkeeping off them).
	HashMap<uint32_t, uint64_t> client_spawned;     // node net_id -> node ObjectID

	// --- Phase 2: delta-against-acked-baseline ---
	// A frame is { net_id -> its sync-property values, in slot order }.
	typedef HashMap<uint32_t, Vector<Variant>> FrameData;
	static const int RING = 32;   // power of two; ~1s of frames at 30 Hz — covers ack RTT + loss

	// Per-peer send history (server). We delta each new frame against the frame the
	// peer last acked; the ring lets a promoted ack become the next baseline and lets
	// a lost ack self-heal (we keep diffing against the same acked frame). Slots are
	// indexed by seq & (RING-1); frame_seq[] guards against a stale (aged-out) slot.
	struct PeerRing {
		FrameData frames[RING];
		uint16_t frame_seq[RING] = {}; // 0 = empty
		uint16_t next_seq = 1;         // 0 is reserved for "no baseline / full state"
		uint16_t last_acked = 0;
		bool has_ack = false;
		// Reliable-until-acked spawn/despawn delivery. net_id -> first seq we (re)sent it in
		// the current unacked run; once last_acked >= that seq the peer has it and we stop.
		HashMap<uint32_t, uint16_t> spawn_wait;
		HashMap<uint32_t, uint16_t> despawn_wait;
	};
	HashMap<int32_t, PeerRing> peer_rings;       // server: peer_id -> ring

	// Client receive history — mirror ring so a delta can be reconstructed against any
	// recent baseline the server might diff against.
	FrameData client_frames[RING];
	uint16_t client_frame_seq[RING] = {};
	uint16_t client_last_seq = 0;
	bool client_has = false;

	uint64_t last_send_ms = 0;                   // server send throttle
	uint64_t dbg_last_ms = 0;                    // throttle for the GOLDNET_DEBUG stats print
	uint64_t dbg_bytes = 0;                      // bytes sent since last stats print
	bool dbg = false;                            // GOLDNET_DEBUG=1 → periodic snapshot stats
	int dbg_loss = 0;                            // GOLDNET_LOSS=<pct> → drop that % of snapshots

	void _reset_client_state();

	// Re-emit the inner API's lifecycle signals on ourselves (the game listens on us).
	void _relay_peer_connected(int64_t p_id);
	void _relay_peer_disconnected(int64_t p_id);
	void _relay_connected_to_server();
	void _relay_connection_failed();
	void _relay_server_disconnected();

	// Internals.
	bool _should_intercept(MultiplayerSynchronizer *p_sync) const;  // has streamable sync props
	GoldNetLink *_ensure_link();                                 // create/find /root/__GoldNetLink
	void _server_tick();                                         // build + send delta snapshots
	uint32_t _min_interval_ms() const;
	static bool _seq_newer(uint16_t a, uint16_t b) { return (int16_t)(a - b) > 0; }
	static bool _seq_le(uint16_t a, uint16_t b) { return !_seq_newer(a, b); }

	// Phase 3 spawn/despawn.
	bool spawners_scanned = false;
	void _wrap_spawner(class MultiplayerSpawner *p_spawner);  // capture its spawn_function
	void _on_node_added(Node *p_node);                        // SceneTree.node_added → wrap new spawners
	void _scan_spawners();                                    // one-time: wrap spawners already in tree
	Variant _spawn_trampoline(Variant p_data, int64_t p_spawner_objid); // wraps the game's spawn_function
	void _drain_pending_spawns();  // promote captured spawns (now in-tree) to spawn_records
	void _detect_despawns();       // poll: spawned nodes that were freed become despawns
	void _apply_spawn(uint32_t p_net_id, uint32_t p_spawner_net_id, const Variant &p_data); // client
	void _apply_despawn(uint32_t p_net_id);                                                  // client

protected:
	static void _bind_methods() {}

public:
	GoldNetMultiplayer();
	~GoldNetMultiplayer();

	// Client-side snapshot apply (called by GoldNetLink::_gn_recv).
	void apply_snapshot(const PackedByteArray &p_bytes);

	// Server-side: a client acked the newest frame it received (called by
	// GoldNetLink::_gn_ack). Advances that peer's delta baseline.
	void on_ack(int32_t p_peer, int32_t p_seq);

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
