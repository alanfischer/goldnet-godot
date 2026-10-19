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
//
// Upstream, an @rpc a node lists in its "gn_commands" meta is a command stream (see
// addons/goldnet/command_stream.gd): a client's calls to the server are carried by
// goldnet::CommandSender instead of the inner, and the server's CommandReceiver calls the method
// once per call, in order, the way a reliable RPC would.

#include "goldnet/client.h"
#include "goldnet/netsim.h"
#include "goldnet/server.h"
#include "goldnet/uplink.h"
#include "goldnet_sim_peer.h"

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
	bool manual_send = false;                    // see set_manual_send
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
	// --- Network-condition simulation (send-side) — see docs/netsim_plan.md ---
	// Applied at the two send funnels goldnet owns: _rpc (every game @rpc) and the snapshot send in
	// ServerHost::send. Loss drops a packet; latency and spikes defer it. Configured through
	// sim_config, or at startup by GOLDNET_LOSS / GOLDNET_SIM_SEED / GOLDNET_LATENCY / GOLDNET_SPIKE /
	// GOLDNET_LINK.
	//
	// A deferred send replays inner->rpc(peer, <object>, method, args). The target is held by
	// ObjectID so a free during the (few-ms) delay is a safe no-op.
	struct PendingSend {
		int32_t peer = 0;
		uint64_t object_id = 0;
		StringName method;
		Array args;
	};
	goldnet::NetSim<PendingSend> sim;
	// The receive leg (recv_sim_config): its knobs, kept here so they outlive a session, and the
	// GoldNetSimPeer wrapping the current client peer that applies them.
	GoldNetSimPeer::Sim recv_sim;
	Ref<GoldNetSimPeer> recv_peer;
	// Shared tail of both send funnels: the simulated link, latency and spike, then send now, queue,
	// or drop. p_droppable: an unreliable send a full link buffer may drop. p_bytes: its wire size,
	// or -1 to estimate it (only done under a link cap). Returns the inner->rpc Error for the
	// immediate path (OK otherwise — the deferral can't fail here).
	Error _sim_send(int32_t p_peer, Object *p_object, const StringName &p_method, const Array &p_args,
			bool p_droppable, int p_bytes = -1);
	void _charge_rpc(int32_t p_peer, const Array &p_args); // count a game RPC against its recipients' rates
	// True iff (object, method)'s @rpc transfer mode is one of the unreliable modes — i.e. loss may
	// drop it without corrupting state (ENet retransmits reliable RPCs, so dropping one desyncs).
	// Resolved from the node's rpc config and cached per (object, method). Only consulted when loss
	// is armed (loss_percent > 0), so normal play pays nothing and the cache stays empty. Unknown/non-node
	// targets return false (treated reliable → never dropped), the safe default.
	bool _rpc_is_unreliable(Object *p_object, const StringName &p_method);
	HashMap<uint64_t, HashMap<StringName, bool>> _rpc_unreliable_cache; // object_id -> method -> is-unreliable

	void _reset_client_state();

	// --- Command streams (uplink) ---
	goldnet::CommandSender commands_out;  // client
	goldnet::CommandReceiver commands_in; // server
	// Client: the snapshot ack for the next uplink packet. While a command stream is active it waits
	// for the next command packet rather than going alone, as GoldSrc's acks ride clc_move.
	uint16_t snapshot_ack = 0;
	bool snapshot_ack_new = false;
	// Client: object -> method -> stream id, 0 = an ordinary RPC. Resolved on the first call.
	HashMap<uint64_t, HashMap<StringName, uint32_t>> command_stream_cache;
	// Server: stream id -> the method it calls, found by scanning the tree for gn_commands metas.
	struct CommandTarget {
		uint64_t object_id = 0;
		StringName method;
	};
	HashMap<uint32_t, CommandTarget> command_targets;
	uint64_t command_scan_ms = 0;
	// Server: the peer whose command is being delivered, for get_remote_sender_id().
	int32_t command_sender = 0;
	uint32_t _command_stream_id(Object *p_object, const StringName &p_method);
	void _scan_command_targets(Node *p_node);
	void _deliver_command(int32_t p_peer, uint32_t p_stream, const uint8_t *p_data, size_t p_size);
	void _send_uplink();
	struct UplinkHost;

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
	bool _apply_spawn(uint32_t p_net_id, uint32_t p_spawner_net_id, const Variant &p_data); // client
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
	// GoldSrc rate / choke: the most bytes per second to send this peer (0 = unlimited, the
	// default). Snapshots and game RPCs both count; while the peer is over, its snapshots are
	// skipped (choked), and the next one it gets carries the latest state. Unlike
	// set_peer_bandwidth_bps, which caps each snapshot's entity bytes, this spaces snapshots out.
	void set_peer_rate(int p_peer, int p_bytes_per_sec);
	// Snapshots withheld from this peer by its rate so far.
	int get_peer_chokes(int p_peer) const;
	// Who decides when snapshots go out. Off (the default): goldnet sends on its own clock, at the
	// synchronizers' replication_interval, from poll. On: the game calls send_snapshots() once per
	// server tick, after it has written that tick's state — GoldSrc's SV_SendClientMessages at the
	// end of SV_Frame. A timer that isn't locked to the game's tick drifts against it: some ticks
	// go out twice (the second with nothing new, so skipped) and some never go out on their own,
	// which a client sees as uneven gaps. Per-peer intervals and rates apply either way.
	void set_manual_send(bool p_manual);
	bool get_manual_send() const;
	void send_snapshots();
	// Network-condition simulation, as ONE replace-semantics property rather than seven knobs:
	// sim_config = {} is the off switch, and a read-back matches what was written. Unknown keys are
	// an error, so a typo cannot quietly reset the sim. Latency is the full per-leg delay applied at
	// THIS endpoint's send funnels; the opposite leg is configured on its own endpoint.
	// Keys: loss_percent, seed, latency_min_ms, latency_max_ms, spike_ms, spike_interval_s,
	// spike_duration_s, link_bps (a slow link's bytes/sec, 0 = none), link_queue_ms (its buffer),
	// link_overhead (per-send wire overhead in bytes). Every knob is idempotent, so a read-modify-write of one disturbs nothing
	// else. A non-zero seed replays the whole sim (loss rolls and latency draws); 0 is random.
	void set_sim_config(const Dictionary &p_cfg);
	Dictionary get_sim_config() const;
	// The same knobs for what this endpoint RECEIVES, so a client alone can play over a slow,
	// lossy downlink without the server being configured for it. Loss drops unreliable packets,
	// latency and spikes delay every packet, and link_bps queues them. Applies to client peers
	// only (a server's sends are its own sim_config), and live, at any point in a session.
	void set_recv_sim_config(const Dictionary &p_cfg);
	Dictionary get_recv_sim_config() const;
	// Client: {bytes, packets} the receive sim delivered since the last call. A downlink meter reads
	// this instead of the transport's counters while the sim is on, since those count what the sim
	// then holds back or drops. Zeros on a server, which has no receive sim.
	Dictionary pop_recv_sim_delivered();

	// Opt into PVS render-relevance events: the server sends reliable-until-acked "leave" markers for
	// owned syncs that drop out of a peer's PVS, and the client emits `entity_relevance_lost` for them.
	void set_relevance_events(bool p_enabled);
	bool get_relevance_events() const;

	// Client: command packets per second (GoldSrc cl_cmdrate). 0 (the default) sends whenever there
	// is a command to send, which a game calling its command RPC once per tick makes once per tick.
	void set_command_rate(double p_hz);
	double get_command_rate() const;
	// Client: {pending: unacked commands, dropped: given up on, rtt_ms: the command round trip}.
	Dictionary get_command_stats() const;
	~GoldNetMultiplayer();

	// Client-side snapshot apply (called by GoldNetLink::_gn_recv).
	void apply_snapshot(const PackedByteArray &p_bytes);

	// Server-side: one uplink packet from a client (called by GoldNetLink::_gn_up) — its snapshot
	// ack and its commands.
	void on_uplink(int32_t p_peer, const PackedByteArray &p_bytes);

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
