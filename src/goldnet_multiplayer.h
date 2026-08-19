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
		// Peer-invariant importance weight for priority-ordered overflow (see PeerRing::stale_since
		// and SAFE_PACKET_BYTES), from the "gn_priority" meta (a float; default 1.0 if unset).
		// Read and cached the same lazy way as quant — by the first tick, the game has had the
		// chance to set it.
		//
		// Read ONCE, like quant: changing the meta after the entity's first tick has no effect.
		// That is a sharper limit here than it is for quant, because importance is the more
		// plausible thing to want to vary at runtime ("this corpse stopped mattering"). Games
		// needing that today should express it through visibility (set_visibility_for) instead,
		// which is consulted every tick. Re-reading per tick is a get_meta across the GDExtension
		// boundary per entity, which is the cost the cached read plan exists to avoid.
		float priority = 1.0f;
		bool priority_read = false;
		// Change tracking, stamped ONCE per snapshot tick (see _server_tick). The values an entity
		// holds are the same for every peer, so "did slot s change" must be answered once here
		// rather than re-derived per peer — that comparison was O(peers x entities x slots) and
		// ~94% of it ran on entities that had not changed at all.
		//
		// last_vals is the previous tick's values; slot_ctr[s] is the monotonic snapshot counter
		// at which slot s last changed. A peer needs slot s iff slot_ctr[s] > its baseline's
		// counter, which is a plain integer compare no matter how many peers there are.
		Vector<Variant> last_vals;
		Vector<uint32_t> slot_ctr;
		// Resolved read plan, built once (see _cache_slots). Reading a slot used to re-parse the
		// replication config, allocate a TypedArray, and walk each property path back to its node
		// — building Strings and NodePaths — every tick, for every entity, forever. None of that
		// varies: the config and the node layout are fixed once the entity is in the tree, so all
		// that has to happen per tick is the get_indexed itself.
		//
		// Cached lazily on first read, matching how `quant` is already handled: by then the game
		// has finished setting the entity up. A config swapped out at runtime would not be picked
		// up, which is the same assumption `quant_read` already makes.
		Vector<uint64_t> slot_target_id; // object id owning each slot's property
		Vector<NodePath> slot_prop;      // property path to read on that object
		bool slots_cached = false;
		// Push-based change notification (see mark_dirty). Reading an entity's slots is the only
		// way to discover it changed, so with ~390 entities and ~25 of them actually moving, the
		// read loop spent almost all of its time confirming that nothing happened. A game that
		// already knows when it wrote state can say so instead. Starts true so an entity is read
		// once when it registers — that first read is what gives last_vals its contents, which
		// every later full-state send (a new peer, a PVS re-entry) is served from.
		bool dirty = true;
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
		// What the peer HAD at each frame, as net_ids only. This used to hold every visible
		// entity's values (FrameData), one full copy per peer per frame — 32 copies live per
		// peer. The values were identical across peers and only ever used to answer "did this
		// change since the peer's baseline", which SyncEntry::slot_ctr now answers directly.
		// All this has to remember is WHICH entities the peer had, so a re-entering entity
		// still gets a full state rather than a delta against something it never received.
		HashSet<uint32_t> frames[RING];
		uint16_t frame_seq[RING] = {}; // 0 = empty
		// The monotonic snapshot counter each frame was sent at. The wire seq stays per-peer
		// (16-bit, its own space, unchanged), while change tracking needs a counter that never
		// aliases: a slot untouched for 32768 ticks would otherwise compare as newly changed.
		uint32_t frame_ctr[RING] = {};
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
		// Per-peer PVS relevance for owned_syncs, GoldSrc-style: removals are DERIVED from the
		// delta baseline rather than queued. `held` is what this peer's last acked frame says it
		// holds; anything in there that the peer can no longer see gets a remove marker in the next
		// snapshot. A lost snapshot isn't acked, so `held` doesn't move and the same diff recomputes
		// the same markers next tick — self-correcting, with no retry queue to leak or starve.
		// (Enter needs no event — an entering entity re-appears in the changed set with a full
		// baseline, which fires the synchronizer's `synchronized` signal the game already listens to.)
		// Empty until the first ack, which is also why a joining peer gets no leave burst: it holds
		// nothing yet, so there is nothing to remove.
		HashSet<uint32_t> held;
		// Removals written since this peer's last ack. The peer has been told to drop these, but
		// `held` won't reflect it until the frame carrying them is acked — so a re-entry inside that
		// window must ship a full baseline, not a slot delta against a frame the peer no longer
		// matches. Soft state: losing it only costs a redundant full baseline.
		HashSet<uint32_t> left_unacked;
		// Per-peer snapshot cadence (GoldSrc cl_updaterate). 0 = serve this peer every server
		// tick; otherwise serve it at most once per interval_ms. Throttling costs nothing on the
		// wire: the next frame this peer does get simply deltas against its older acked baseline,
		// which the ring already supports (it is the same path a lost snapshot takes).
		uint32_t interval_ms = 0;
		uint32_t last_sent_ms = 0;
		// When a snapshot was last actually EMITTED to this peer. Distinct from last_sent_ms, which
		// stamps the send-cadence check and advances even on a tick that decides to send nothing:
		// this one gates the clock keepalive (see GN_CLOCK_KEEPALIVE_MS), which has to know how long
		// the peer has really been without a header, not how long since it was last considered.
		uint32_t last_packet_ms = 0;
		// Priority-ordered overflow (see MAX_ENTITY_BODY_BYTES in _server_tick). net_id -> the
		// snapshot_ctr this entity FIRST had an unsent pending change for THIS peer; cleared once
		// it's actually written. The gap between that and the current snapshot_ctr is how many
		// ticks it's been waiting, multiplied by SyncEntry::priority to score entities when more
		// changed this tick than fit in the budget — the most-overdue, highest-weighted entity
		// goes first. Mirrors spawn_wait/despawn_wait's "first-seq" bookkeeping for the same
		// reason: only a successful write resets it, not merely a later tick touching it again.
		HashMap<uint32_t, uint32_t> stale_since;
		// Per-peer bandwidth budget in bytes/sec for the entity-delta body (0 = unset — use the
		// flat MAX_ENTITY_BODY_BYTES ceiling only). Converted to a per-packet budget via
		// interval_ms in _server_tick; never relaxes the hard MTU-safety ceiling, only tightens it.
		uint32_t bandwidth_bps = 0;
	};
	HashMap<int32_t, PeerRing> peer_rings;       // server: peer_id -> ring
	// Monotonic snapshot counter, bumped once per _server_tick that sends anything. Purely
	// internal: it stamps SyncEntry::slot_ctr and PeerRing::frame_ctr so change tests are a
	// single integer compare. Unlike the 16-bit wire seq it never wraps in any realistic
	// session (2^32 ticks at 60 Hz is ~2 years), so a long-static entity can't alias as changed.
	uint32_t snapshot_ctr = 0;

	// mark_dirty() accepts either a synchronizer or the node whose state it replicates, since a
	// game thinks in entities, not synchronizers. Resolving node -> sync means scanning children,
	// so the answer is memoized here on first use. Keyed by node ObjectID.
	HashMap<uint64_t, uint64_t> dirty_route;
	// Push mode. OFF by default: goldnet polls every entity every tick, which is correct without
	// any cooperation from the game. A game that marks its writes (mark_dirty) opts in and pays
	// only for entities that actually changed. Defaulting this on would silently stale every
	// consumer that has not been taught to mark.
	bool push_dirty = false;
	// Audit mode: read every entity as if it were dirty and report any that changed WITHOUT being
	// marked. A missed mark_dirty is otherwise invisible — the entity just silently stops updating
	// for everyone — so this exists to turn that into a loud, testable failure. Off by default;
	// intended for dev builds and the integration suite, not production.
	bool dirty_audit = false;

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
	uint32_t bandwidth_bps_default = 0;          // config: >0 sets the default per-peer bandwidth budget (bytes/sec)
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
	static bool _read_and_stamp(SyncEntry &p_entry, uint32_t p_ctr);

public:
	/// Tell goldnet an entity's replicated state has changed, so the next snapshot reads it.
	/// Accepts the MultiplayerSynchronizer or the node it replicates. Cheap (one hash lookup
	/// after the first call) and safe to call off-server or with no session — it no-ops.
	void mark_dirty(Object *p_obj);
	void set_dirty_audit(bool p_enabled);
	bool get_dirty_audit() const;
	void set_push_dirty(bool p_enabled);
	bool get_push_dirty() const;

private:
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
	void set_peer_snapshot_interval_ms(int p_peer, int p_ms);
	int get_peer_snapshot_interval_ms(int p_peer) const;
	// Entity-delta bandwidth budget in bytes/sec, converted to a per-packet budget via each peer's
	// snapshot interval. 0 (default) means no rate budget — only the flat MTU-safety ceiling
	// applies, today's behavior. A rate budget only ever tightens that ceiling, never relaxes it.
	// Unlike snapshot_interval_ms above (a server-wide send-cadence setting, unrelated to any
	// per-peer default), this one IS a genuine global default: set_bandwidth_bps applies to every
	// peer that hasn't been given its own via set_peer_bandwidth_bps, which wins when set.
	void set_bandwidth_bps(int p_bps);
	int get_bandwidth_bps() const;
	void set_peer_bandwidth_bps(int p_peer, int p_bps);
	int get_peer_bandwidth_bps(int p_peer) const;
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
