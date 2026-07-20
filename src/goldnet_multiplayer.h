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

#include "goldnet_codec.h"

#include <godot_cpp/classes/multiplayer_api.hpp>
#include <godot_cpp/classes/multiplayer_api_extension.hpp>
#include <godot_cpp/classes/multiplayer_peer.hpp>
#include <godot_cpp/classes/multiplayer_synchronizer.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/templates/vector.hpp>
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
		// Per-slot quantization tag (GN_Q_AUTO = none), aligned to the sync-property slots. Read
		// lazily on the first tick (so the game has set the "gn_quant" meta by then) and cached;
		// empty means all-auto.
		Vector<uint8_t> quant;
		bool quant_read = false;
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
		// The spawned node's own + descendant synchronizers, for lazy spawning: the spawn is only sent
		// to a peer once one of these is visible to it (GoldSrc: an entity is created client-side the
		// first time it enters your PVS, then persists until the server destroys it). Empty ⇒ no sync
		// ⇒ no visibility info ⇒ send to everyone (fail open, matching the old always-spawn behaviour).
		Vector<uint64_t> sync_objids;
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
		// Spawns already acked by this peer. Unlike despawns (one-shot — the record is dropped
		// once delivered), a spawn's source record lives as long as the entity, so "absent from
		// spawn_wait" can't mean "delivered" — it would re-arm the spawn every frame. This set is
		// the durable "peer already has this node" marker; a net_id here is never re-sent until
		// the entity despawns (which clears it).
		HashSet<uint32_t> spawn_acked;
		// Per-peer relevance (PVS) for owned_syncs. `relevant` is the set of owned-sync net_ids
		// visible to this peer as of the last snapshot; diffing it each tick yields the entities that
		// LEFT this peer's PVS, delivered reliable-until-acked in `leave_wait` so the client can hide
		// them. (Enter needs no event — an entering entity re-appears in the changed set with a full
		// baseline, which fires the synchronizer's `synchronized` signal the game already listens to.)
		HashSet<uint32_t> relevant;
		HashMap<uint32_t, uint16_t> leave_wait;
		// Seed: on a peer's first snapshot, `relevant` is initialized to ALL owned syncs so the very
		// first leave-diff emits a leave for everything currently out of this peer's PVS. The client
		// defaults entities to present, so without this seed the initially-out-of-PVS ones would render
		// through walls until they first entered+left. Mirrors the game's old first-pass "all present".
		bool relevance_seeded = false;
	};
	HashMap<int32_t, PeerRing> peer_rings;       // server: peer_id -> ring

	// Client receive history — mirror ring so a delta can be reconstructed against any
	// recent baseline the server might diff against.
	FrameData client_frames[RING];
	uint16_t client_frame_seq[RING] = {};
	uint16_t client_last_seq = 0;
	bool client_has = false;
	bool warned_protocol_mismatch = false; // one-shot guard for the wire-version-mismatch warning

	// Client: consecutive frames deferred because a KNOWN entity's node wasn't in the tree yet
	// (recv-nodes warmup — see the defer block in _client_apply). Capped so a node that never
	// resolves can't stall the stream forever; after the cap we accept the frame and move on.
	uint32_t defer_streak = 0;
	static const uint32_t MAX_DEFER_STREAK = RING; // ~1s at 30 Hz before giving up on a warmup entity

	uint64_t last_send_ms = 0;                   // server send throttle
	uint32_t cached_min_interval_ms = 33;        // send cadence; refreshed on config add/remove, not per poll
	int32_t snapshot_interval_override = 0;      // config: >0 overrides the synchronizer-derived send cadence (ms)
	uint64_t dbg_last_ms = 0;                    // throttle for the GOLDNET_DEBUG stats print
	uint64_t dbg_bytes = 0;                      // bytes sent since last stats print
	bool dbg = false;                            // GOLDNET_DEBUG=1 → periodic snapshot stats
	int dbg_loss = 0;                            // GOLDNET_LOSS=<pct> → drop that % of snapshots
	uint32_t sim_seed = 0;                       // GOLDNET_SIM_SEED=<n> → 0 = engine RNG (nondeterministic)
	uint32_t _sim_rng = 0;                       // xorshift state; only advanced when sim_seed != 0
	// Every random draw the sim makes goes through here, so one seed replays a whole
	// session. Unseeded falls through to the engine RNG — behavior identical to before.
	uint32_t _sim_rand();
	int _sim_rand_range(int p_lo, int p_hi);
	// One loss roll: true = drop this packet.
	bool _loss_roll();

	// --- Network-condition simulation (send-side) — see docs/netsim_plan.md ---
	// Applied at the two send funnels goldnet owns: _rpc (every game @rpc) and the snapshot
	// send in _server_tick. Latency/spike defer the outgoing packet through _sim_queue; loss
	// (dbg_loss, above) drops it. All send-side: the receiver runs its handler on arrival, so
	// no receive hook is needed. A direct C++ port of the old net_latency_sim.gd (minus the
	// per-leg >>1 half-split: latency here is the full per-leg delay, configured at each sender).
	int   latency_min_ms   = 0;                  // GOLDNET_LATENCY=min,max (or a single fixed value)
	int   latency_max_ms   = 0;
	int   spike_ms         = 0;                  // GOLDNET_SPIKE=ms,interval,duration — one-way spike latency
	float spike_interval_s = 10.0f;              // average seconds between spikes
	float spike_duration_s = 0.2f;               // how long each spike lasts
	bool  _spike_active    = false;
	float _spike_timer     = 0.0f;               // counts up to spike_interval_s while idle
	float _spike_elapsed   = 0.0f;               // counts up to spike_duration_s while active
	uint64_t _sim_last_poll_ms = 0;              // last _sim_pump time, for the spike timer's frame delta

	// A deferred send: replay inner->rpc(peer, <object>, method, args) once fire_at_ms elapses.
	// The target is stored by ObjectID so a free during the (few-ms) delay is a safe no-op —
	// matching the null-safe Callable capture the GDScript sim relied on.
	struct PendingSend {
		uint64_t fire_at_ms = 0;
		int32_t  peer = 0;
		uint64_t object_id = 0;
		StringName method;
		Array args;
	};
	Vector<PendingSend> _sim_queue;
	uint64_t _last_fire_at = 0;                  // ordering cursor: a late-released packet still fires
	                                             // after earlier ones (models a pipe, not per-packet jitter)

	void _sim_update_spike(float p_delta);       // advance the spike state machine by the poll delta
	int  _sim_delay_ms();                        // this send's latency (spike-aware); 0 = send immediately
	void _sim_pump(uint64_t p_now_ms);           // update spike + fire due entries from _sim_queue
	// Shared tail of both send funnels: apply latency/spike, then send now or queue. Returns the
	// inner->rpc Error for the immediate path (OK for the queued path — the deferral can't fail here).
	Error _sim_send(int32_t p_peer, Object *p_object, const StringName &p_method, const Array &p_args);
	// Queue inner->rpc(peer,object,method,args) for delayed replay, preserving send order.
	void _sim_queue_send(int p_delay_ms, int32_t p_peer, Object *p_object, const StringName &p_method, const Array &p_args);
	// True iff (object, method)'s @rpc transfer mode is one of the unreliable modes — i.e. loss may
	// drop it without corrupting state (ENet retransmits reliable RPCs, so dropping one desyncs).
	// Resolved from the node's rpc config and cached per (object, method). Only consulted when loss
	// is armed (dbg_loss > 0), so normal play pays nothing and the cache stays empty. Unknown/non-node
	// targets return false (treated reliable → never dropped), the safe default.
	bool _rpc_is_unreliable(Object *p_object, const StringName &p_method);
	HashMap<uint64_t, HashMap<StringName, bool>> _rpc_unreliable_cache; // object_id -> method -> is-unreliable

	// Opt-in relevance events (see the leave block in _server_tick). Off by default so goldnet stays a
	// pure state-replication transport; a consuming game that wants PVS render-relevance through the
	// snapshot (instead of its own reliable RPC) sets this true and connects `entity_relevance_lost`.
	bool relevance_events_enabled = false;

	void _reset_client_state();

	// Re-emit the inner API's lifecycle signals on ourselves (the game listens on us).
	void _relay_peer_connected(int64_t p_id);
	void _relay_peer_disconnected(int64_t p_id);
	void _relay_connected_to_server();
	void _relay_connection_failed();
	void _relay_server_disconnected();

	// Internals.
	bool _should_intercept(MultiplayerSynchronizer *p_sync) const;  // has streamable sync props
	// Build the per-slot quantization tags from a synchronizer's "gn_quant" meta (see gn_put_value).
	static void _read_quant(MultiplayerSynchronizer *p_sync, Vector<uint8_t> &r_quant);
	GoldNetLink *_ensure_link();                                 // create/find /root/__GoldNetLink
	void _server_tick();                                         // build + send delta snapshots
	uint32_t _min_interval_ms() const;
	// Defined in goldnet_codec.h so the standalone tests can pin the rollover behavior.
	static bool _seq_newer(uint16_t a, uint16_t b) { return goldnet::seq_newer(a, b); }
	static bool _seq_le(uint16_t a, uint16_t b) { return goldnet::seq_le(a, b); }

	// Reliable-until-acked delivery shared by the spawn and despawn sections. `wait[net_id]`
	// records the first seq of the current unacked run; _reliable_include returns whether to
	// (re)send this record to the peer and stops tracking once it's acked; _retire_acked drops
	// the records an ack confirms delivered.
	static bool _reliable_include(HashMap<uint32_t, uint16_t> &p_wait, uint32_t p_net_id, uint16_t p_seq, uint16_t p_last_acked, bool p_has_ack);
	static void _retire_acked(HashMap<uint32_t, uint16_t> &p_wait, uint16_t p_last_acked, Vector<uint32_t> &r_retired);

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
	static void _bind_methods();

public:
	GoldNetMultiplayer();

	// --- Config surface (Phase 5) — settable from GDScript on the installed API instance ---
	// Snapshot send cadence override in ms; 0 (default) derives it from the synchronizers'
	// replication_interval (min across owned syncs). Set >0 to pin one global tick rate.
	void set_snapshot_interval_ms(int p_ms);
	int get_snapshot_interval_ms() const;
	// Periodic per-peer snapshot stats to stdout (also enabled by GOLDNET_DEBUG=1).
	void set_debug_enabled(bool p_enabled);
	bool is_debug_enabled() const;
	// Drop this percent of outbound snapshots server-side to exercise the ack self-heal without a
	// real lossy network (also settable via GOLDNET_LOSS=<pct>).
	void set_loss_percent(int p_pct);
	int get_loss_percent() const;
	// Seed for the whole network-condition sim — loss rolls AND latency draws (also
	// settable via GOLDNET_SIM_SEED=<n>). 0 = use the engine RNG, i.e. a different pattern
	// every run. Any non-zero value makes the session reproducible, which is what a
	// regression test needs. Setting it resets the generator, so seed before the session
	// starts rather than mid-run.
	void set_sim_seed(int p_seed);
	int get_sim_seed() const;
	// Send-side network-condition simulation (see docs/netsim_plan.md). Latency is the full
	// per-leg delay applied at THIS endpoint's send funnels; the opposite leg is configured on
	// its own endpoint. Also settable via GOLDNET_LATENCY / GOLDNET_SPIKE env vars (headless).
	void set_latency_min_ms(int p_ms);
	int get_latency_min_ms() const;
	void set_latency_max_ms(int p_ms);
	int get_latency_max_ms() const;
	void set_spike_ms(int p_ms);
	int get_spike_ms() const;
	void set_spike_interval_s(float p_s);
	float get_spike_interval_s() const;
	void set_spike_duration_s(float p_s);
	float get_spike_duration_s() const;
	// Clear runtime sim state (pending sends, ordering cursors, spike machine). Leaves config intact.
	void sim_reset();
	// Opt into PVS render-relevance events: the server sends reliable-until-acked "leave" markers for
	// owned syncs that drop out of a peer's PVS, and the client emits `entity_relevance_lost` for them.
	void set_relevance_events(bool p_enabled);
	bool get_relevance_events() const;
	// Arm spawner capture now (wrap existing spawners + hook node_added). Call right after installing
	// GoldNet if the game spawns during _ready, before the first poll would otherwise do it.
	void capture_spawners();
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
