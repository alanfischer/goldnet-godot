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
// addons/goldnet/command_stream.gd): a client's calls to the server are carried by its uplink
// (goldnet::Client::push_command) instead of the inner, and the server calls the method once per call, in
// order, the way a reliable RPC would.
//
// Every other RPC between the server and a client rides the same packets too, as a message on
// GoldSrc's netchan (goldnet/netchan.h): a reliable one in the reliable stream, an unreliable one in
// the datagram. See _send_message.

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

	// How each of a synchronizer's slots goes on the wire, the same on both ends: its kind, from the
	// property's declared type and its gn_quant / gn_grid hint (see _make_slot_coding).
	struct SlotCoding {
		std::vector<uint8_t> kinds;
		std::vector<double> grids;
		std::vector<uint8_t> quant; // a tagged slot's hint
		uint8_t check = 0;          // a hash of the kinds and grids, for Entity::kinds_check
	};

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
		SlotCoding coding;
	};
	typedef goldnet::Server<Variant, SyncData> CoreServer;
	CoreServer server;
	goldnet::Client<Variant> client;
	struct ServerHost;
	struct ClientHost;
	HashMap<uint32_t, uint64_t> netid_to_objid;  // net_id -> synchronizer ObjectID (client apply lookup)
	HashMap<uint64_t, SlotCoding> client_slot_codings; // synchronizer ObjectID -> its slot coding, made on first apply
	HashSet<uint32_t> warned_kinds;                // net_ids whose slot kinds differ from the server's

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
	// Send cadence; refreshed on config add/remove, not per poll. Microseconds, because a millisecond
	// cannot express the tick rates games actually use: 60 Hz is 16.667 ms.
	uint64_t cached_min_interval_us = 33000;
	double snapshot_rate_setting = 0.0;          // config: >0 overrides the synchronizer-derived send cadence (Hz)
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
	// --- Network-condition simulation (send-side) ---
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
	void _forget_peer(int32_t p_id);  // a peer's state, here and in the core
	void _end_server_session();       // every peer's, when a hosted session ends

	// --- Command streams (uplink) ---
	// Client: the update rate the server last granted, as update_rate_granted told the game; -1 = none
	// yet this session. The rates themselves are goldnet's Client's (set_rate & co).
	int update_rate_granted = -1;
	// A command stream's signature: each argument is one field, typed by its declaration, or each
	// entry of a lone Dictionary argument, named and typed by CommandStream.mark's table (see
	// command_signature in the .cpp). id 0 = an ordinary RPC.
	struct CommandSignature {
		uint32_t id = 0;
		goldnet::Schema kinds;
		std::vector<uint8_t> forms; // how each field maps back to the argument's Variant
		std::vector<StringName> names;
		bool table = false; // the fields are one Dictionary argument's entries (CommandStream.mark)
	};
	// Client: object -> method -> its signature. Resolved on the first call.
	HashMap<uint64_t, HashMap<StringName, CommandSignature>> command_stream_cache;
	// Server: stream id -> the method it calls, found by scanning the tree for gn_commands metas.
	struct CommandTarget {
		uint64_t object_id = 0;
		StringName method;
		CommandSignature sig;
	};
	HashMap<uint32_t, CommandTarget> command_targets;
	uint64_t command_scan_ms = 0;
	// The peer whose command or message is being delivered, for get_remote_sender_id().
	int32_t command_sender = 0;
	const CommandSignature *_command_stream(Object *p_object, const StringName &p_method);
	void _scan_command_targets(Node *p_node);
	const CommandTarget *_command_target(uint32_t p_stream);
	void _deliver_command(int32_t p_peer, uint32_t p_stream, const goldnet::Command &p_cmd);
	void _send_uplink();
	struct UplinkHost;

	// --- Messages: RPCs on the netchan ---
	// A message names its target (a node path and a method) by a small id, which the sending end
	// registers with the receiving one reliably, the first time it uses it with that peer: GoldSrc's
	// svc_newusermsg, done lazily. The ids are this end's own; each peer keeps the ones the other named.
	HashMap<String, uint32_t> msg_ids; // "path::method" -> id
	uint32_t msg_next_id = 1;          // 0 is the registration message itself
	// Peer -> id -> the index of its registration in that peer's reliable stream. Until the peer has
	// that, an unreliable call goes reliable, so it can't arrive before its target is known.
	HashMap<int32_t, HashMap<uint32_t, uint64_t>> msg_registered;
	struct MsgTarget {
		NodePath path;
		StringName method;
		uint64_t object_id = 0; // resolved on first delivery
		bool warned = false;
	};
	HashMap<int32_t, HashMap<uint32_t, MsgTarget>> msg_targets; // peer -> id -> what it named
	bool _send_message(int32_t p_peer, Node *p_node, const StringName &p_method, const Array &p_args, bool p_reliable);
	void _deliver_message(int32_t p_from, const uint8_t *p_data, size_t p_size);
	bool _netchan_rpc(int32_t p_peer, Object *p_object, const StringName &p_method, const Array &p_args);
	void _check_overflow(int32_t p_peer);

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
	static void _make_slot_coding(MultiplayerSynchronizer *p_sync, const Vector<uint64_t> &p_target_ids,
			const Vector<NodePath> &p_props, SlotCoding &r_coding);
	static void _read_push(MultiplayerSynchronizer *p_sync, std::vector<uint8_t> &r_push);

public:
	/// Tell goldnet an entity's replicated state has changed, so the next snapshot reads it.
	/// Accepts the MultiplayerSynchronizer or the node it replicates. Cheap (one hash lookup
	/// after the first call) and safe to call off-server or with no session — it no-ops.
	void mark_dirty(Object *p_obj);

private:
	GoldNetLink *_ensure_link();                                 // create/find /root/__GoldNetLink
	void _server_tick();                                         // build + send delta snapshots
	uint64_t _min_interval_us() const;
	void _refresh_interval(); // recompute the cached send cadence

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
	// Server: snapshots per second, the ceiling on every client's update_rate. 0 (default) derives it
	// from the synchronizers' replication_interval (min across owned syncs). With manual_send, set it
	// to the rate send_snapshots() is called at; otherwise it also sets goldnet's own send timer.
	void set_snapshot_rate(double p_hz);
	double get_snapshot_rate() const;
	// Client: the snapshots per second we ask to be served at (GoldSrc cl_updaterate; 0 = as many as
	// the server sends, the default). The server grants it under its snapshot_rate and under what our
	// rate can carry, and at least 10, and says what it granted: update_rate_granted.
	void set_update_rate(int p_hz);
	int get_update_rate() const;
	// Client: the most bytes per second the server may send us (GoldSrc `rate`; 0 = no limit, the
	// default). Snapshots and game RPCs both count; while we're over it the server skips our
	// snapshots (a choke), and the next one we get carries the latest state. Ignored while
	// auto_rate is on, which picks it from how far behind the downlink is arriving.
	void set_rate(int p_bytes_per_sec);
	int get_rate() const;
	void set_auto_rate(bool p_enabled);
	bool get_auto_rate() const;
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
	// Running totals for the link to p_peer (a client passes 1); a caller wanting a rate diffs two
	// reads. Every key is present on both ends, 0 where it doesn't apply:
	//   chokes                   server: snapshots withheld from the peer by its rate
	//   snapshots_received/lost  client: lost counts only seqs the server spent, so real losses
	//   messages_queued/delivered  reliable netchan messages sent to the peer, and how many it has
	//   commands_pending/dropped client: unacked commands, and ones given up on
	//   command_rtt_ms           client: the command round trip (current, not a total)
	//   recv_sim_bytes/packets   client: what the receive sim let through, which is what a downlink
	//                            meter should count while it is on, not the transport's counters
	// and these, which are current values rather than totals:
	//   rate                     the rate in force for the link, bytes/sec; 0 = no limit
	//   update_rate              snapshots/sec the server grants the client; 0 = every tick, at a
	//                            rate the server doesn't know
	//   command_rate             client: command packets/sec in force; 0 = one per command
	//   downlink_lag_ms          client: how far behind its best the downlink is arriving
	//   uplink_lag_ms, uplink_pps  the same for the uplink, and its packets arriving per second
	//                            (measured on the server, reported to the client)
	//   delivered_bps            client: bytes/sec arriving on the wire
	Dictionary get_peer_stats(int p_peer) const;

	// Opt into PVS render-relevance events: the server sends reliable-until-acked "leave" markers for
	// owned syncs that drop out of a peer's PVS, and the client emits `entity_relevance_lost` for them.
	void set_relevance_events(bool p_enabled);
	bool get_relevance_events() const;

	// Client: command packets per second (GoldSrc cl_cmdrate). 0 (the default) sends whenever there
	// is a command to send, which a game calling its command RPC once per tick makes once per tick.
	// Ignored while auto_command_rate is on, which picks it from the server's report of how far
	// behind our uplink is arriving, up to the physics tick rate.
	void set_command_rate(double p_hz);
	double get_command_rate() const;
	void set_auto_command_rate(bool p_enabled);
	bool get_auto_command_rate() const;
	// WIRE_VERSION: two ends that differ can't talk, so a game's own join handshake can compare them.
	int get_protocol_version() const;
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
