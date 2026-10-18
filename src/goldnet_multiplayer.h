#pragma once

// GoldNetMultiplayer — a drop-in Godot MultiplayerAPI implementing GoldSrc-style
// snapshot replication behind the stock MultiplayerSynchronizer / MultiplayerSpawner
// API.
//
// COMPOSITION: we hold an inner SceneMultiplayer and delegate handshake, auth, peer
// lifecycle, and the game's @rpc traffic to it. We replace the *state replication* layer.
//
// We intercept every synchronizer that has at least one per-tick property (sync or watch)
// into our own registry, and stream those ourselves each tick as an unreliable snapshot
// (via GoldNetLink), honoring the synchronizer's visibility. Each peer gets a delta
// against the last frame it acked, and a full frame only on its first send or when that
// ack has aged out. Spawn-only configs are left to the inner. MultiplayerSpawners are
// wrapped so their spawns and despawns are streamed through the same path.

#include "goldnet/client.h"
#include "goldnet/server.h"

#include <vector>

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

	// The protocol itself (goldnet core): change tracking, per-peer deltas, acks, spawns, leaves
	// and the packet budget on the server; baseline reconstruction on the client. Everything below
	// is the Godot side of it — synchronizers, spawners, Variants, and the inner SceneMultiplayer.
	//
	// The server registers every synchronizer we own (sync-only config), keyed by its stable
	// ObjectID; the wire net_id is a hash of its scene path (identical on server & client, since the
	// same node lives at the same path on both — map entities and spawner-created nodes alike).
	struct SyncData {
		// Refreshed each tick by ServerHost::live, so the per-peer passes need no ObjectDB lookup.
		MultiplayerSynchronizer *sync = nullptr;
		// Resolved read plan, built once in ServerHost::prepare. The config and the node layout are
		// fixed once the entity is in the tree, so all that has to happen per tick is the
		// get_indexed itself. An unresolved slot is a hole (id 0), never dropped: the slot INDEX is
		// the wire position.
		Vector<uint64_t> slot_target_id; // object id owning each slot's property
		Vector<NodePath> slot_prop;      // property path to read on that object
	};
	typedef goldnet::Server<Variant, SyncData> CoreServer;
	CoreServer server;
	goldnet::Client<Variant> client;
	struct ServerHost;
	struct ClientHost;
	HashMap<uint32_t, uint64_t> netid_to_objid;  // net_id -> synchronizer ObjectID (client apply lookup)

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

	// Client: nodes we spawned, so a despawn can free the right one. Also lets us emit the
	// spawner's spawned/despawned signals (the game hangs bookkeeping off them).
	HashMap<uint32_t, uint64_t> client_spawned;     // node net_id -> node ObjectID

	// mark_dirty() accepts either a synchronizer or the node whose state it replicates, since a
	// game thinks in entities, not synchronizers. Resolving node -> sync means scanning children,
	// so the answer is memoized here on first use. Keyed by node ObjectID.
	HashMap<uint64_t, uint64_t> dirty_route;
	uint64_t last_send_us = 0;                   // server send deadline (microseconds — see _poll)
	uint32_t cached_min_interval_ms = 33;        // send cadence; refreshed on config add/remove, not per poll
	// The same cadence in microseconds. The gate runs off this rather than the millisecond value
	// because a millisecond cannot express the tick rates games actually use: 60 Hz is 16.667 ms,
	// and rounding that to 17 costs 1.2% of the rate before any jitter is counted.
	uint64_t cached_min_interval_us = 33000;
	int32_t snapshot_interval_override = 0;      // config: >0 overrides the synchronizer-derived send cadence (ms)
	uint64_t dbg_last_ms = 0;                    // throttle for the GOLDNET_DEBUG stats print
	uint64_t dbg_bytes = 0;                      // bytes sent since last stats print
	bool dbg = false;                            // GOLDNET_DEBUG=1 → periodic snapshot stats
	// GOLDNET_RPC_STATS=1 → every 5 s, the game's @rpc traffic per method next to the snapshot
	// stream, so a server's egress can be split by what sent it. Byte counts are estimates: the
	// args' Variant encoding plus a fixed header guess, times recipients. ENet's own socket
	// statistics are left alone — pop_statistic resets them, and the game reads them.
	struct RpcStat {
		uint64_t calls = 0;
		uint64_t sends = 0;   // calls times recipients
		uint64_t bytes = 0;   // estimated, summed over recipients
		bool unreliable = false;
		String owner;         // script file of the first caller seen
	};
	bool rpc_stats = false;
	uint64_t rpc_stats_last_ms = 0;
	uint64_t rpc_stats_snap_bytes = 0;
	uint64_t rpc_stats_snap_sends = 0;
	HashMap<StringName, RpcStat> rpc_stats_methods;
	void _rpc_stats_count(int32_t p_peer, Object *p_object, const StringName &p_method, const Array &p_args);
	void _rpc_stats_print(uint64_t p_now);
	int dbg_loss = SIM_LOSS_DEFAULT_PERCENT;     // GOLDNET_LOSS=<pct> → drop that % of snapshots
	uint32_t sim_seed = SIM_SEED_DEFAULT;        // GOLDNET_SIM_SEED=<n> → 0 = engine RNG (nondeterministic)
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
	// Defaults live here and nowhere else: set_sim_config uses these same constants as its
	// absent-key fallbacks, so `sim_config = {}` and a fresh instance agree by construction.
	static constexpr int   SIM_LATENCY_MIN_DEFAULT_MS = 0;
	static constexpr int   SIM_LATENCY_MAX_DEFAULT_MS = 0;
	static constexpr int   SIM_SPIKE_DEFAULT_MS       = 0;
	static constexpr float SIM_SPIKE_INTERVAL_DEFAULT_S = 10.0f;
	static constexpr float SIM_SPIKE_DURATION_DEFAULT_S = 0.2f;
	static constexpr int   SIM_LOSS_DEFAULT_PERCENT   = 0;
	static constexpr int   SIM_SEED_DEFAULT           = 0;

	int   latency_min_ms   = SIM_LATENCY_MIN_DEFAULT_MS;  // GOLDNET_LATENCY=min,max (or a single fixed value)
	int   latency_max_ms   = SIM_LATENCY_MAX_DEFAULT_MS;
	int   spike_ms         = SIM_SPIKE_DEFAULT_MS;        // GOLDNET_SPIKE=ms,interval,duration — one-way spike latency
	float spike_interval_s = SIM_SPIKE_INTERVAL_DEFAULT_S; // average seconds between spikes
	float spike_duration_s = SIM_SPIKE_DURATION_DEFAULT_S; // how long each spike lasts
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
	static void _read_quant(MultiplayerSynchronizer *p_sync, std::vector<uint8_t> &r_quant);
	static void _read_push(MultiplayerSynchronizer *p_sync, std::vector<uint8_t> &r_push);

public:
	/// Tell goldnet an entity's replicated state has changed, so the next snapshot reads it.
	/// Accepts the MultiplayerSynchronizer or the node it replicates. Cheap (one hash lookup
	/// after the first call) and safe to call off-server or with no session — it no-ops.
	void mark_dirty(Object *p_obj);

private:
	GoldNetLink *_ensure_link();                                 // create/find /root/__GoldNetLink
	void _server_tick();                                         // build + send delta snapshots
	uint32_t _min_interval_ms() const;
	uint64_t _min_interval_us() const;
	void _refresh_interval(); // recompute the cached send cadence and hand it to the core

	// Phase 3 spawn/despawn.
	bool spawners_scanned = false;
	void _wrap_spawner(class MultiplayerSpawner *p_spawner);  // capture its spawn_function
	void _on_node_added(Node *p_node);                        // SceneTree.node_added → wrap new spawners
	// Arm spawner capture: scan the tree once and watch for later additions. Idempotent, and no
	// longer public — it is called on our first poll and again when a peer is set, and a spawn that
	// still slips through both wraps its spawner on the spot (see _object_configuration_add), so
	// there is nothing left for a consumer to remember to do.
	void capture_spawners();
	Variant _spawn_trampoline(Variant p_data, int64_t p_spawner_objid); // wraps the game's spawn_function
	void _drain_pending_spawns();  // promote captured spawns (now in-tree) to core spawn records
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
	// Write-only on purpose, like set_peer_bandwidth_bps below: the per-peer value is consulted in
	// the send path, and nothing has ever needed to read it back, so the getters were dropped rather
	// than carried as surface. Both are trivial to restore if a consumer turns up wanting them.
	void set_peer_snapshot_interval_ms(int p_peer, int p_ms);
	// Entity-delta bandwidth budget in bytes/sec, converted to a per-packet budget via each peer's
	// snapshot interval. 0 (default) means no rate budget — only the flat MTU-safety ceiling
	// applies, today's behavior. A rate budget only ever tightens that ceiling, never relaxes it.
	// Unlike snapshot_interval_ms above (a server-wide send-cadence setting, unrelated to any
	// per-peer default), this one IS a genuine global default: set_bandwidth_bps applies to every
	// peer that hasn't been given its own via set_peer_bandwidth_bps, which wins when set.
	void set_bandwidth_bps(int p_bps);
	int get_bandwidth_bps() const;
	void set_peer_bandwidth_bps(int p_peer, int p_bps);
	// Network-condition simulation, as ONE replace-semantics property rather than seven knobs:
	// sim_config = {} is the off switch, and a read-back matches what was written. Unknown keys are
	// an error, so a typo cannot quietly reset the sim. The per-knob setters below stay as plain
	// C++ (the GOLDNET_* env parsing in the constructor uses them) but are deliberately NOT bound —
	// a test harness should not be a third of the public API.
	// Keys: loss_percent, seed, latency_min_ms, latency_max_ms, spike_ms, spike_interval_s,
	// spike_duration_s. Every setter is idempotent, so a read-modify-write of one knob disturbs
	// nothing else — see set_sim_seed.
	void set_sim_config(const Dictionary &p_cfg);
	Dictionary get_sim_config() const;

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
	// Opt into PVS render-relevance events: the server sends reliable-until-acked "leave" markers for
	// owned syncs that drop out of a peer's PVS, and the client emits `entity_relevance_lost` for them.
	void set_relevance_events(bool p_enabled);
	bool get_relevance_events() const;
	// Arm spawner capture now (wrap existing spawners + hook node_added). Call right after installing
	// GoldNet if the game spawns during _ready, before the first poll would otherwise do it.
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
