#pragma once
//
// The server half of the snapshot protocol: per-entity change tracking, per-peer delta against
// the last acked frame, reliable-until-acked spawns and despawns, derived PVS leaves, and the
// priority-ordered packet budget. Engine-agnostic: the host supplies the values, the visibility
// answers, the value encoding, and the send.
//
// V is the host's value type. It must be default-constructible, copyable and comparable with !=;
// the server never looks inside one, it only compares and stores them and hands them back to the
// host to encode. X is optional per-entity storage for the host (cached property lookups, say),
// carried on the entity so reaching it costs no second lookup.
//
// The host passed to tick() provides:
//   bool live(Entity &e)                        is the entity replicating this tick? (may refresh e.user)
//   void prepare(Entity &e)                     first live tick: set e.slots, e.quant, e.push, e.priority
//   void read(Entity &e, bool polled_only)      read the slot values, calling stamp() for each
//   bool visible(const Entity &e, int32_t peer) per-peer visibility of a live entity
//   bool spawn_visible(uint64_t key, int32_t peer)  same, for a spawn record's visibility keys
//   void encode(Writer &w, const V &v, uint8_t quant, uint32_t now_ms)  one tagged value
//   void send(int32_t peer, const Writer &packet)
//
// Spawns. An entity the client already has (one loaded with the map, at the same net_id on both
// ends) needs only add_entity. A runtime entity, which the client has no other way to know about,
// is two registrations under one net_id, and the host makes both:
//
//   add_spawn(net_id, record)   tells each peer to create it; record.data is the host's recipe
//   add_entity(key, net_id)     streams its slot state, like any other entity
//
// and when it's destroyed, despawn(net_id) and remove_entity(key). Calling both on the same tick
// is fine: a snapshot carries its spawns ahead of its entity deltas. A spawn reaches a peer the
// first time the entity is visible to it (see SpawnRecord::vis_keys), goes in that snapshot and
// the RELIABLE_BACKUP after it, then again whenever it is overdue, until the peer acks a snapshot
// carrying it; the despawn reaches only peers that got it, the same way. Until a spawn is proven
// delivered, the entities in its vis_keys don't count as held by the peer, so each snapshot sends
// them whole: a client that missed the spawn discards their deltas, and acking those must not
// leave the server believing it has them.
//
// On the client, Client::apply() hands each spawn to the host's spawn() before it reads the
// deltas, so by the time spawn() returns, registered() and begin_entity() must know the entity.
// A delta for an id the host doesn't know is discarded and the frame still acked, and an idle
// entity's state is then never sent again. A host that can't create it yet returns false from
// spawn(), and one that has it but can't take values yet sets begin_entity's r_defer; either way
// the client withholds the ack and the server keeps offering it (Client::apply has the details).
//
// Messages. Each peer's snapshots carry its end of a GoldSrc netchan (netchan.h): queue_reliable and
// queue_unreliable put the host's messages in the peer's next snapshots, ahead of the entities in the
// packet budget. The client's half of the acks arrives in its uplink (on_channel_ack), and what the
// server took from the client's uplink goes back in each header (set_uplink_ack).
//
#include "goldnet/buffer.h"
#include "goldnet/codec.h"
#include "goldnet/netchan.h"
#include "goldnet/protocol.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace goldnet {

struct NoUserData {};

template <class V, class X = NoUserData>
class Server {
public:
	struct Entity {
		uint64_t key = 0;    // the host's identity for it (stable for the entity's life)
		uint32_t net_id = 0; // its identity on the wire, identical on server and client
		int slots = 0;
		// Per-slot quantization tag (QUANT_AUTO = none). Empty means all-auto.
		std::vector<uint8_t> quant;
		// Per-slot push declaration: listing a slot is the host's promise to mark_dirty() every
		// write to it; every slot NOT listed is read every tick regardless of the mark. That
		// inversion is the point: an undeclared slot cannot go stale, so forgetting to mark one
		// costs a read instead of silently desyncing it for every peer. Empty means all polled.
		std::vector<uint8_t> push;
		// Peer-invariant importance weight for priority-ordered overflow (see PeerRing::stale_since
		// and SAFE_PACKET_BYTES). Set once, in prepare.
		float priority = 1.0f;
		// Push-based change notification. Starts true so an entity is read once when it registers —
		// that first read is what gives vals its contents, which every later full-state send (a new
		// peer, a PVS re-entry) is served from.
		bool dirty = true;
		// True when at least one slot is undeclared and so must be read on a tick where the entity
		// was not marked. Entities that declare every slot skip the poll.
		bool has_polled = true;
		bool prepared = false;
		// Change tracking, stamped ONCE per snapshot tick. The values an entity holds are the same
		// for every peer, so "did slot s change" is answered once here rather than re-derived per
		// peer. vals is the latest read; slot_ctr[s] is the snapshot counter at which slot s last
		// changed. A peer needs slot s iff slot_ctr[s] > its baseline's counter.
		std::vector<V> vals;
		std::vector<uint32_t> slot_ctr;
		X user;
	};

	// A runtime entity's creation record, replayed to each peer the first time the entity is
	// visible to it. spawner_net_id names what creates it on the client (0 = none) and goes on the
	// wire with data, which is opaque to the protocol. node_key is the host's own; the server never
	// reads it. vis_keys are the keys to ask spawn_visible() about.
	struct SpawnRecord {
		uint32_t spawner_net_id = 0;
		uint64_t node_key = 0;
		std::vector<uint8_t> data;
		// Empty ⇒ no visibility info ⇒ send to everyone (fail open).
		std::vector<uint64_t> vis_keys;
	};

	// --- Entities ---

	Entity &add_entity(uint64_t p_key, uint32_t p_net_id) {
		auto it = index_.find(p_key);
		if (it != index_.end()) {
			Entity &e = ents_[it->second];
			e = Entity();
			e.key = p_key;
			e.net_id = p_net_id;
			return e;
		}
		index_[p_key] = ents_.size();
		ents_.emplace_back();
		Entity &e = ents_.back();
		e.key = p_key;
		e.net_id = p_net_id;
		return e;
	}

	// Order-preserving, so the packet order of everything else is unaffected.
	bool remove_entity(uint64_t p_key) {
		auto it = index_.find(p_key);
		if (it == index_.end()) {
			return false;
		}
		const size_t idx = it->second;
		index_.erase(it);
		ents_.erase(ents_.begin() + idx);
		for (auto &kv : index_) {
			if (kv.second > idx) {
				kv.second--;
			}
		}
		return true;
	}

	Entity *find(uint64_t p_key) {
		auto it = index_.find(p_key);
		return it == index_.end() ? nullptr : &ents_[it->second];
	}

	std::vector<Entity> &entities() { return ents_; }
	const std::vector<Entity> &entities() const { return ents_; }

	// The next tick reads this entity's declared (pushed) slots too. False if it isn't registered.
	bool mark_dirty(uint64_t p_key) {
		Entity *e = find(p_key);
		if (!e) {
			return false;
		}
		e->dirty = true;
		return true;
	}

	// Called by the host's read(): record slot s's current value, stamping it if it moved.
	void stamp(Entity &p_e, int p_slot, const V &p_value) {
		if (p_value != p_e.vals[p_slot]) {
			p_e.vals[p_slot] = p_value;
			p_e.slot_ctr[p_slot] = ctr_;
		}
	}

	// Whether a polled-only read should skip slot s (the host promised to mark it and didn't).
	static bool skip_on_poll(const Entity &p_e, int p_slot) {
		return (size_t)p_slot < p_e.push.size() && p_e.push[p_slot];
	}

	// --- Spawns ---

	void add_spawn(uint32_t p_net_id, SpawnRecord p_rec) {
		spawns_[p_net_id] = std::move(p_rec);
		despawn_pending_.erase(p_net_id); // a reused net_id is a fresh spawn, not a despawn
	}

	const std::unordered_map<uint32_t, SpawnRecord> &spawns() const { return spawns_; }

	// A spawned entity was destroyed: it becomes a despawn owed to every peer that RECEIVED the
	// spawn. With lazy spawning, peers that never saw the entity never got the spawn, so they're
	// owed nothing — scope the despawn to peers holding it (delivered) or mid-receiving it.
	void despawn(uint32_t p_net_id) {
		if (spawns_.erase(p_net_id) == 0) {
			return;
		}
		std::unordered_set<int32_t> needers;
		for (auto &kv : rings_) {
			PeerRing &pr = kv.second;
			if (pr.spawn_acked.count(p_net_id) || pr.spawn_wait.count(p_net_id)) {
				needers.insert(kv.first);
			}
			pr.spawn_wait.erase(p_net_id);  // stop resending the (now void) spawn
			pr.spawn_acked.erase(p_net_id); // and forget delivery, so a reused id re-spawns cleanly
			pr.stale_since.erase(p_net_id); // and stop tracking overflow-priority for a dead entity
		}
		if (!needers.empty()) {
			despawn_pending_[p_net_id] = std::move(needers);
		}
	}

	// --- Peers ---

	// GoldSrc cl_updaterate: the snapshots/sec this peer asks for (CommandReceiver::update_rate); 0 =
	// as many as the server sends. It is granted that, under the snapshot rate and under what its rate
	// can carry in idle snapshots, and at least MIN_UPDATE_RATE, without slowing anyone else down.
	// Dropped with the peer.
	void set_peer_update_rate(int32_t p_peer, uint32_t p_hz) { rings_[p_peer].update_rate_asked = p_hz; }
	// What p_peer was granted at its last tick, snapshots/sec, as its headers tell it; 0 = every tick,
	// at a rate the server doesn't know.
	uint32_t update_rate(int32_t p_peer) const {
		auto it = rings_.find(p_peer);
		return it == rings_.end() ? 0 : it->second.update_rate;
	}
	// The rate tick() is called at, snapshots/sec: the ceiling on every grant, and the tick a slower
	// peer's cadence is counted in. 0 = unknown.
	void set_snapshot_rate(double p_hz) { snapshot_rate_ = p_hz > 0.0 ? p_hz : 0.0; }
	double snapshot_rate() const { return snapshot_rate_; }

	// GoldSrc rate / choke: the most bytes per second this peer's link takes (0 = unlimited). Every
	// send to the peer — snapshots here, and whatever else the host reports through charge() —
	// pushes its clear time out by bytes / rate. While that's in the future the peer is choked: it
	// gets no snapshot this tick, and the next one it does get deltas against its acked baseline,
	// so it carries the latest state rather than a backlog. A slow link then gets fewer snapshots
	// instead of a queue that delays every one of them.
	void set_peer_rate(int32_t p_peer, uint32_t p_bps) { rings_[p_peer].rate_bps = p_bps; }
	uint32_t peer_rate(int32_t p_peer) const {
		auto it = rings_.find(p_peer);
		return it == rings_.end() ? 0 : it->second.rate_bps;
	}
	// Wire bytes a snapshot costs beyond its payload (headers below the protocol), charged per packet.
	void set_packet_overhead(int p_bytes) { packet_overhead_ = p_bytes > 0 ? p_bytes : 0; }
	// Count p_bytes sent to p_peer against its rate. No-op for an unrated peer.
	void charge(int32_t p_peer, uint32_t p_now, size_t p_bytes) {
		auto it = rings_.find(p_peer);
		if (it == rings_.end() || it->second.rate_bps == 0) {
			return;
		}
		PeerRing &pr = it->second;
		const double start = pr.clear_at_ms > (double)p_now ? pr.clear_at_ms : (double)p_now;
		pr.clear_at_ms = start + (double)p_bytes * 1000.0 / (double)pr.rate_bps;
	}
	// The newest command this peer sent that the host has delivered with none missing before it
	// (CommandReceiver::ack), carried in the peer's next snapshot header. 0 = none. p_news
	// (CommandReceiver::take_news) earns a snapshot even when the ack hasn't moved.
	void set_command_ack(int32_t p_peer, uint16_t p_seq, bool p_news = false) {
		PeerRing &pr = rings_[p_peer];
		pr.cmd_ack = p_seq;
		pr.cmd_news = pr.cmd_news || p_news;
	}

	// How far behind its best this peer's uplink arrives, ms, and its packets arriving per second
	// (CommandReceiver::uplink_lag_ms / uplink_pps), carried in the peer's next snapshot header for its
	// CmdRateAuto.
	void set_uplink_report(int32_t p_peer, uint32_t p_lag_ms, uint32_t p_pps) {
		PeerRing &pr = rings_[p_peer];
		pr.uplink_lag_ms = p_lag_ms;
		pr.uplink_pps = p_pps;
	}

	// --- Messages (netchan.h) ---

	void queue_reliable(int32_t p_peer, const uint8_t *p_data, size_t p_size) { rings_[p_peer].rel.queue(p_data, p_size); }
	// False if the peer's datagram is full and the message was dropped.
	bool queue_unreliable(int32_t p_peer, const uint8_t *p_data, size_t p_size) {
		return rings_[p_peer].dgram.queue(p_data, p_size);
	}
	// The peer isn't taking its reliable data: GoldSrc dropped the client.
	bool overflowed(int32_t p_peer) const {
		auto it = rings_.find(p_peer);
		return it != rings_.end() && it->second.rel.overflowed();
	}
	// Reliable messages queued to the peer, and how many of those it has. Both in queue order.
	uint64_t reliable_queued(int32_t p_peer) const {
		auto it = rings_.find(p_peer);
		return it == rings_.end() ? 0 : it->second.rel.queued();
	}
	uint64_t reliable_delivered(int32_t p_peer) const {
		auto it = rings_.find(p_peer);
		return it == rings_.end() ? 0 : it->second.rel.delivered();
	}
	// Datagrams to the peer dropped for not fitting.
	uint32_t datagram_dropped(int32_t p_peer) const {
		auto it = rings_.find(p_peer);
		return it == rings_.end() ? 0 : it->second.dgram.dropped();
	}
	// From the peer's uplink: what it has received of the server's channel.
	void on_channel_ack(int32_t p_peer, const ChannelAck &p_ack) {
		auto it = rings_.find(p_peer);
		if (it != rings_.end()) {
			it->second.rel.on_ack(p_ack);
		}
	}
	// What the server has received of the peer's channel (CommandReceiver::channel_ack), for its next
	// header. A new block bit or fragment earns a snapshot of its own: it is what lets the peer send on.
	void set_uplink_ack(int32_t p_peer, const ChannelAckSource &p_src) { rings_[p_peer].uplink = p_src; }

	// Snapshots withheld from this peer by its rate so far (GoldSrc's choke count).
	uint32_t chokes(int32_t p_peer) const {
		auto it = rings_.find(p_peer);
		return it == rings_.end() ? 0 : it->second.chokes;
	}

	// Opt-in relevance events: the server sends leave markers for entities that drop out of a
	// peer's PVS. Off by default so the protocol stays pure state replication.
	void set_relevance_events(bool p_enabled) { relevance_events_ = p_enabled; }
	bool relevance_events() const { return relevance_events_; }

	void remove_peer(int32_t p_peer) {
		rings_.erase(p_peer);
		// Drop the peer from any despawn needer-set so those despawns can still retire.
		for (auto it = despawn_pending_.begin(); it != despawn_pending_.end();) {
			it->second.erase(p_peer);
			if (it->second.empty()) {
				it = despawn_pending_.erase(it);
			} else {
				++it;
			}
		}
	}

	// A client acked the newest frame it received. Advances that peer's delta baseline.
	void on_ack(int32_t p_peer, uint16_t p_seq, uint32_t p_now) {
		auto rit = rings_.find(p_peer);
		if (rit == rings_.end()) {
			return;
		}
		PeerRing &pr = rit->second;
		if (!pr.has_ack || seq_newer(p_seq, pr.last_acked)) {
			pr.last_acked = p_seq;
			pr.has_ack = true;
			// This frame is now what the peer holds — the set every PVS removal is derived against.
			// Copied rather than read from the ring at send time so it survives the frame aging out
			// under heavy loss: a stale-but-real `held` re-sends a removal the peer may already have
			// (harmless, the client just hides an entity twice), while an empty one would lose it.
			const int hslot = p_seq & (RING - 1);
			if (pr.frame_seq[hslot] == p_seq && p_seq != 0) {
				pr.held = pr.frames[hslot];
				pr.left_unacked.clear(); // the acked frame already excludes them
				// Only the first ack of a frame is timed: the client repeats it until a newer frame.
				const uint32_t sample = p_now - pr.frame_ms[hslot];
				pr.srtt_ms = pr.srtt_ms == 0 ? sample : (pr.srtt_ms * 7 + sample) / 8;
			}
		}
		// Retire spawn/despawn records this ack proves delivered.
		std::vector<uint32_t> retired;
		retire_proven(pr.spawn_wait, p_seq, retired);
		for (uint32_t nid : retired) {
			pr.spawn_acked.insert(nid); // durably remember delivery (spawn source record persists)
		}
		retired.clear();
		retire_proven(pr.despawn_wait, p_seq, retired);
		for (uint32_t nid : retired) {
			auto dit = despawn_pending_.find(nid);
			if (dit != despawn_pending_.end()) {
				dit->second.erase(p_peer);
				if (dit->second.empty()) {
					despawn_pending_.erase(dit); // all peers have it — done
				}
			}
		}
	}

	// Build and send this tick's snapshot to every peer that is due one.
	template <class Host>
	void tick(uint32_t p_now, const std::vector<int32_t> &p_peers, Host &p_host);

private:
	// What the peer HAD at each frame, as net_ids only — values live once, on the entity, and
	// slot_ctr answers "did this change since the peer's baseline" directly. All a frame has to
	// remember is WHICH entities the peer had, so a re-entering entity still gets a full state
	// rather than a delta against something it never received.
	typedef std::unordered_set<uint32_t> IdSet;

	// Per-peer send history. Each new frame deltas against the frame the peer last acked; the
	// ring lets a promoted ack become the next baseline and lets a lost ack self-heal (we keep
	// diffing against the same acked frame). frame_seq[] guards against a stale (aged-out) slot.
	struct PeerRing {
		IdSet frames[RING];
		uint16_t frame_seq[RING] = {}; // 0 = empty
		// The monotonic snapshot counter each frame was sent at. The wire seq stays per-peer
		// (16-bit, its own space), while change tracking needs a counter that never aliases: a slot
		// untouched for 32768 ticks would otherwise compare as newly changed.
		uint32_t frame_ctr[RING] = {};
		uint32_t frame_ms[RING] = {}; // when each frame went, to time its ack
		uint32_t srtt_ms = 0;         // smoothed snapshot-to-ack round trip; 0 = none timed yet
		uint16_t next_seq = 1; // 0 is reserved for "no baseline / full state"
		uint16_t last_acked = 0;
		bool has_ack = false;
		// Reliable-until-acked spawn/despawn delivery: net_id -> the snapshots that carried it,
		// until an ack of one of them proves it arrived (see owe_include / retire_proven).
		std::unordered_map<uint32_t, Owed> spawn_wait;
		std::unordered_map<uint32_t, Owed> despawn_wait;
		// Spawns already acked by this peer. Unlike despawns (one-shot — the record is dropped once
		// delivered), a spawn's source record lives as long as the entity, so "absent from
		// spawn_wait" can't mean "delivered" — it would re-arm the spawn every frame. This set is the
		// durable "peer already has this node" marker; a net_id here is never re-sent until the
		// entity despawns (which clears it).
		IdSet spawn_acked;
		// Per-peer PVS relevance, GoldSrc-style: removals are DERIVED from the delta baseline rather
		// than queued. `held` is what this peer's last acked frame says it holds; anything in there
		// that the peer can no longer see gets a remove marker in the next snapshot. A lost snapshot
		// isn't acked, so `held` doesn't move and the same diff recomputes the same markers next
		// tick — self-correcting, with no retry queue to leak or starve. Empty until the first ack,
		// which is also why a joining peer gets no leave burst: it holds nothing yet.
		IdSet held;
		// Removals written since this peer's last ack. The peer has been told to drop these, but
		// `held` won't reflect it until the frame carrying them is acked — so a re-entry inside that
		// window must ship a full baseline, not a slot delta against a frame the peer no longer
		// matches. Soft state: losing it only costs a redundant full baseline.
		IdSet left_unacked;
		uint32_t update_rate_asked = 0; // see set_peer_update_rate
		uint32_t update_rate = 0;       // granted, rounded, as the header carries it
		// When this peer was last considered for a send, for the cadence check. Advances even on a
		// tick that decides to send nothing.
		uint32_t last_sent_ms = 0;
		// When a snapshot was last actually EMITTED to this peer. Gates the clock keepalive, which
		// has to know how long the peer has really been without a header.
		uint32_t last_packet_ms = 0;
		// Priority-ordered overflow. net_id -> the snapshot counter this entity FIRST had an unsent
		// pending change for THIS peer; cleared once it's actually written. The wait, weighted by
		// Entity::priority, scores entities when more changed this tick than fit in the budget — the
		// most-overdue, highest-weighted entity goes first. Only a successful write resets it, not
		// merely a later tick touching it again.
		std::unordered_map<uint32_t, uint32_t> stale_since;
		uint16_t cmd_ack = 0;      // see set_command_ack
		uint16_t cmd_ack_sent = 0; // what the last snapshot told the peer, so a new one earns a send
		bool cmd_news = false;     // see set_command_ack
		uint32_t uplink_lag_ms = 0; // see set_uplink_report
		uint32_t uplink_pps = 0;
		uint32_t rate_bps = 0;     // see set_peer_rate; 0 = unlimited
		double clear_at_ms = 0.0;  // when what's been sent to this peer has cleared its rate
		uint32_t chokes = 0;
		ReliableSender rel;
		DatagramSender dgram;
		ChannelAckSource uplink; // see set_uplink_ack
		ChannelAck uplink_sent;  // what the last snapshot told the peer
	};

	struct Candidate {
		size_t idx;
		uint32_t net_id;
		uint32_t mask;
		float score;
		bool holdable; // false while the entity's spawn isn't proven delivered to this peer
	};

	std::vector<Entity> ents_;
	std::unordered_map<uint64_t, size_t> index_;
	std::unordered_map<uint32_t, SpawnRecord> spawns_;
	// Despawns awaiting delivery: the peers that still need each one (reliable-until-acked); erased
	// once every such peer acks a frame carrying it.
	std::unordered_map<uint32_t, std::unordered_set<int32_t>> despawn_pending_;
	std::unordered_map<int32_t, PeerRing> rings_;
	// Monotonic snapshot counter, bumped once per tick that sends anything. It stamps
	// Entity::slot_ctr and PeerRing::frame_ctr so change tests are a single integer compare. Unlike
	// the 16-bit wire seq it never wraps in any realistic session (2^32 ticks at 60 Hz is ~2 years).
	uint32_t ctr_ = 0;
	int packet_overhead_ = 0;
	double snapshot_rate_ = 0.0;
	bool relevance_events_ = false;
	std::vector<size_t> live_; // scratch: entities live this tick
	std::unordered_set<uint64_t> unproven_; // scratch: entity keys whose spawn this peer may lack

	// See set_peer_update_rate. 0 = every tick.
	double grant(const PeerRing &p_pr) const {
		double hz = (double)p_pr.update_rate_asked;
		if (p_pr.rate_bps > 0) {
			const double cap = p_pr.rate_bps * UPDATE_RATE_RATE_SHARE / (double)(IDLE_SNAPSHOT_BYTES + packet_overhead_);
			hz = hz > 0.0 ? std::min(hz, cap) : cap;
		}
		if (snapshot_rate_ > 0.0 && (hz <= 0.0 || hz > snapshot_rate_)) {
			hz = snapshot_rate_;
		}
		if (hz <= 0.0) {
			return 0.0;
		}
		hz = std::max(hz, (double)MIN_UPDATE_RATE);
		if (snapshot_rate_ > 0.0) {
			// Served every nth tick, so that is what it gets.
			double n = std::ceil(snapshot_rate_ / hz - 1e-9);
			n = std::max(1.0, std::min(n, std::floor(snapshot_rate_ / (double)MIN_UPDATE_RATE)));
			hz = snapshot_rate_ / n;
		}
		return hz;
	}
};

template <class V, class X>
template <class Host>
void Server<V, X>::tick(uint32_t p_now, const std::vector<int32_t> &p_peers, Host &p_host) {
	// Resolve which peers are due BEFORE the per-entity state read below: on a tick where every
	// peer is throttled (all of them on a slower cadence than the server's) this costs nothing.
	// last_sent_ms == 0 always sends — a peer's first snapshot must not wait out an interval.
	// A slower peer is due half a tick early, so clock jitter can't push it a whole tick late.
	const double slack_ms = snapshot_rate_ > 0.0 ? 500.0 / snapshot_rate_ : 0.0;
	std::vector<int32_t> due;
	for (int32_t peer : p_peers) {
		PeerRing &pr = rings_[peer]; // default-constructs on first use
		const double hz = grant(pr);
		pr.update_rate = (uint32_t)std::lround(hz);
		const bool gated = hz > 0.0 && !(snapshot_rate_ > 0.0 && hz >= snapshot_rate_);
		if (gated && pr.last_sent_ms != 0 && (double)(p_now - pr.last_sent_ms) + slack_ms < 1000.0 / hz) {
			continue;
		}
		if (pr.rate_bps > 0 && pr.clear_at_ms > (double)p_now) {
			pr.chokes++;
			continue;
		}
		due.push_back(peer);
	}
	if (due.empty()) {
		return;
	}

	// Read every entity's current state ONCE per tick — the values are the server's authoritative
	// state, identical for all peers. Only the delta mask (vs. each peer's baseline) and the
	// visibility filter are peer-specific, so those stay in the per-peer loop.
	ctr_++;
	live_.clear();
	for (size_t i = 0; i < ents_.size(); i++) {
		Entity &e = ents_[i];
		if (!p_host.live(e)) {
			continue;
		}
		if (!e.prepared) {
			p_host.prepare(e);
			if (e.push.size() != (size_t)e.slots) {
				e.push.clear(); // a declaration that doesn't line up with the slots declares nothing
			}
			e.has_polled = e.push.empty() || std::find(e.push.begin(), e.push.end(), 0) != e.push.end();
			e.prepared = true;
			e.dirty = true; // first sight: vals has nothing in it yet
		}
		if (e.vals.size() != (size_t)e.slots) {
			e.vals.resize(e.slots);
			e.slot_ctr.assign(e.slots, ctr_);
		}
		// A marked entity is read whole. A clean one keeps the values and stamps it already has for
		// every slot it declared, which is all any peer — including one being sent a full baseline —
		// needs from it. Undeclared slots on an unmarked entity are read anyway: that is what keeps a
		// missed mark_dirty from being a silent, permanent desync.
		if (e.dirty) {
			p_host.read(e, false);
			e.dirty = false;
		} else if (e.has_polled) {
			p_host.read(e, true);
		}
		live_.push_back(i);
	}

	std::vector<Candidate> candidates;
	for (int32_t peer : due) {
		PeerRing &pr = rings_[peer];
		pr.last_sent_ms = p_now;

		// Baseline = the frame this peer last acked, if we still hold it. Otherwise send a full
		// frame (baseline seq 0) — first send, or an ack aged out of the ring under heavy loss.
		uint16_t base_seq = 0;
		const IdSet *base = nullptr;
		uint32_t base_ctr = 0;
		if (pr.has_ack) {
			const int bslot = pr.last_acked & (RING - 1);
			if (pr.frame_seq[bslot] == pr.last_acked && pr.last_acked != 0) {
				base_seq = pr.last_acked;
				base = &pr.frames[bslot];
				base_ctr = pr.frame_ctr[bslot];
			}
		}

		// Taken for good only once the snapshot goes (below), so a gap in the seqs a client
		// receives is a lost snapshot, never a skipped one.
		const uint16_t seq = pr.next_seq;

		// The reliable block first: it has the packet before anything else. One written here always
		// goes, since it makes the snapshot non-empty.
		std::vector<uint8_t> block;
		const uint8_t block_flags = pr.rel.write(seq, block);
		const ChannelAck up = pr.uplink.at(p_now);

		// Spawns owed to this peer, reliable-until-acked.
		Writer spawn_body;
		uint16_t spawn_ct = 0;
		const uint32_t overdue = overdue_ms(pr.srtt_ms);
		unproven_.clear();
		for (const auto &kv : spawns_) {
			if (pr.spawn_acked.count(kv.first)) {
				continue; // peer already has this node — don't re-arm the spawn every frame
			}
			unproven_.insert(kv.second.vis_keys.begin(), kv.second.vis_keys.end());
			// Lazy spawn (GoldSrc): defer sending the spawn to this peer until the entity is visible
			// to it — created client-side the first time it enters your PVS. Once delivered it
			// PERSISTS (spawn_acked keeps it; despawn only on server-free), so there's no
			// create/destroy churn as it moves in and out of view.
			if (!kv.second.vis_keys.empty()) {
				bool visible = false;
				for (uint64_t vk : kv.second.vis_keys) {
					if (p_host.spawn_visible(vk, peer)) {
						visible = true;
						break;
					}
				}
				if (!visible) {
					continue; // not in this peer's PVS yet — defer the spawn
				}
			}
			// on_ack is the sole populator of spawn_acked, the moment an ack proves a carrier arrived.
			if (!owe_include(pr.spawn_wait, kv.first, seq, p_now, overdue, RELIABLE_BACKUP)) {
				continue;
			}
			spawn_body.put_u32(kv.first);
			spawn_body.put_u32(kv.second.spawner_net_id);
			spawn_body.put_u32((uint32_t)kv.second.data.size());
			spawn_body.put_data(kv.second.data.data(), kv.second.data.size());
			spawn_ct++;
		}
		Writer despawn_body;
		uint16_t despawn_ct = 0;
		for (auto &kv : despawn_pending_) {
			if (!kv.second.count(peer)) {
				continue;
			}
			if (!owe_include(pr.despawn_wait, kv.first, seq, p_now, overdue, RELIABLE_BACKUP)) {
				continue; // not due again yet; on_ack drops the peer once it's proven delivered
			}
			despawn_body.put_u32(kv.first);
			despawn_ct++;
		}

		// Per-peer entity delta: filter by visibility, store the visible subset as this peer's next
		// baseline, and emit only the changed slots.
		//
		// Two sets, not one. `frame` is visibility this tick — every entity the peer can currently
		// see, used for PVS-leave detection, regardless of whether its bytes fit on the wire.
		// `included` is the peer's actual reconstructed state after this packet — only entities
		// whose current value the peer truly has (unchanged-so-nothing-to-send, or successfully
		// written this tick). It becomes the baseline future deltas compare against, so a changed
		// entity left out by the byte budget must NOT land in it: if it did, an ack of this seq
		// would push base_ctr past that entity's slot_ctr and the change would never be resent. Left
		// out of `included`, the same compare catches it again next tick — free retry.
		IdSet frame;
		IdSet included;
		Writer body;
		uint16_t changed = 0;

		// Pass 1: visibility + mask for every entity. Unchanged entities are settled immediately;
		// changed ones become overflow candidates, scored for pass 2.
		candidates.clear();
		// Set when any candidate's score departs from the flat 1.0 default. While it stays false
		// every score is identical and sorting is a no-op, so pass 2 skips it.
		bool needs_sort = false;
		for (size_t idx : live_) {
			const Entity &e = ents_[idx];
			if (!p_host.visible(e, peer)) {
				continue;
			}
			const uint32_t net_id = e.net_id;
			frame.insert(net_id);

			// Did this peer hold this entity at its baseline? If not (first sight, or it left and
			// re-entered the peer's PVS) it needs every slot. Otherwise a slot changed iff it was
			// stamped after the baseline's counter — no value comparison here. left_unacked: we've
			// already told this peer to drop the entity, but its acked baseline still lists it — a
			// slot delta against that baseline would leave the client hidden with nothing to re-show
			// it (an idle entity re-entering changes no slot at all).
			uint32_t mask = 0;
			const int n_vals = (int)e.vals.size();
			const bool holdable = !unproven_.count(e.key);
			if (!base || !base->count(net_id) || pr.left_unacked.count(net_id)) {
				mask = n_vals >= MAX_SLOTS ? 0xFFFFFFFFu : ((1u << n_vals) - 1u); // new → all slots
			} else {
				const int n_slots = std::min(n_vals, (int)e.slot_ctr.size());
				for (int s = 0; s < n_slots; s++) {
					if (e.slot_ctr[s] > base_ctr) {
						mask |= (1u << s);
					}
				}
			}
			if (mask == 0) {
				if (holdable) {
					included.insert(net_id); // peer's existing value is already correct
				}
				pr.stale_since.erase(net_id); // no longer owed anything — clear any old overflow mark
				continue;
			}
			// Score = how many ticks this entity has been waiting for THIS peer, weighted by its
			// priority. Only read here — pass 2 commits a wait-start or clears one.
			auto wit = pr.stale_since.find(net_id);
			const uint32_t wait_ticks = wit != pr.stale_since.end() ? ctr_ - wit->second : 0;
			Candidate c;
			c.idx = idx;
			c.net_id = net_id;
			c.mask = mask;
			c.score = (float)(wait_ticks + 1) * e.priority;
			c.holdable = holdable;
			if (c.score != 1.0f) {
				needs_sort = true;
			}
			candidates.push_back(c);
		}

		// Relevance leaves (opt-in), derived GoldSrc-style from the peer's acked baseline: every
		// entity that baseline holds and this frame can't see gets a remove marker. Nothing is
		// queued — a lost snapshot goes unacked, `held` stays put, and the next diff produces the
		// same markers. Enters need no event: an entering entity re-appears with a full baseline.
		// Built before pass 2 because the entity budget is whatever the packet has left once every
		// other section is built.
		Writer leave_body;
		uint16_t leave_ct = 0;
		if (relevance_events_) {
			std::vector<uint32_t> leaving;
			std::vector<uint32_t> carried;
			derive_leaves(pr.held, frame, MAX_LEAVES_PER_SNAPSHOT, leaving, carried);
			for (uint32_t id : leaving) {
				leave_body.put_u32(id);
				leave_ct++;
				// Told to drop it, so it leaves this frame's baseline too — which is what makes a
				// later re-entry a clean all-slots delta. Until the peer acks that, left_unacked
				// covers the window where its own baseline still disagrees.
				pr.left_unacked.insert(id);
			}
			// Held back by the cap: the peer still holds these, so they stay in the baseline and the
			// next diff picks them up again. Dropping them here would lose the removal entirely.
			for (uint32_t id : carried) {
				included.insert(id);
			}
		}

		// Pass 2: highest score first — the most-overdue, highest-weighted entity wins the budget.
		// Stable, so equal scores keep registration order.
		if (needs_sort) {
			std::stable_sort(candidates.begin(), candidates.end(),
					[](const Candidate &a, const Candidate &b) { return a.score > b.score; });
		}

		// What's left of a safe UDP payload after the header and every section already built —
		// measured, not estimated.
		int peer_budget = SAFE_PACKET_BYTES - SNAPSHOT_HEADER_BYTES - (int)spawn_body.size()
				- (int)despawn_body.size() - (int)leave_body.size();
		peer_budget -= (int)block.size() + (up.fragments ? 2 : 0);
		// Then the datagram, all of it or none, and the entities get the rest.
		std::vector<uint8_t> datagram;
		pr.dgram.take(peer_budget, datagram);
		peer_budget -= (int)datagram.size();

		for (size_t ci = 0; ci < candidates.size(); ci++) {
			const Candidate &c = candidates[ci];
			// Past budget, leave this entity out (see `included` above for why that's safe) and mark
			// it waiting so it outranks everything else next tick if it's still unlucky then.
			//
			// `ci > 0` makes the highest-scoring candidate unconditional, which guarantees forward
			// progress: a budget of zero or less (a spawn section that spent
			// the whole MTU) would otherwise starve replication completely and silently. One entity of
			// overrun beats a permanent stall, and it is self-limiting: the sections that crowded the
			// budget out are reliable-until-acked, so they retire and the budget reopens.
			if (ci > 0 && (int)body.size() >= peer_budget) {
				if (!pr.stale_since.count(c.net_id)) {
					pr.stale_since[c.net_id] = ctr_;
				}
				continue;
			}
			const Entity &e = ents_[c.idx];
			body.put_u32(c.net_id);
			put_uvarint(&body, c.mask); // most entities set only the low few bits — 1 byte, not 4
			for (size_t s = 0; s < e.vals.size(); s++) {
				if (c.mask & (1u << s)) {
					p_host.encode(body, e.vals[s], s < e.quant.size() ? e.quant[s] : QUANT_AUTO, p_now);
				}
			}
			if (c.holdable) {
				included.insert(c.net_id);
			}
			pr.stale_since.erase(c.net_id); // delivered (pending ack) — no longer owed
			changed++;
		}

		// Nothing to report: skip the header-only packet — except that the header carries
		// server_time, the client's only clock feed, so "nothing to report" only earns a skip while
		// that feed is still fresh. last_packet_ms == 0 is a peer never sent anything — its first
		// snapshot must go out regardless, or the clock never starts. Nothing above recorded `seq`
		// unless something went in the packet, so the next snapshot reuses it.
		// A command ack the peer hasn't been told is news too: it is what stops the peer resending.
		if (spawn_ct == 0 && despawn_ct == 0 && leave_ct == 0 && changed == 0 && pr.cmd_ack == pr.cmd_ack_sent && !pr.cmd_news
				&& block_flags == 0 && datagram.empty() && up.bit == pr.uplink_sent.bit
				&& (!up.fragments || up.next_fragment == pr.uplink_sent.next_fragment)
				&& pr.last_packet_ms != 0 && p_now - pr.last_packet_ms < CLOCK_KEEPALIVE_MS) {
			continue;
		}
		pr.last_packet_ms = p_now;
		pr.cmd_ack_sent = pr.cmd_ack;
		pr.cmd_news = false;
		pr.uplink_sent = up;
		pr.next_seq++;
		if (pr.next_seq == 0) {
			pr.next_seq = 1; // 0 is reserved for "no baseline"
		}

		const int slot = seq & (RING - 1);
		pr.frames[slot] = std::move(included);
		pr.frame_seq[slot] = seq;
		pr.frame_ctr[slot] = ctr_;
		pr.frame_ms[slot] = p_now;

		Writer packet;
		packet.put_u8(SNAPSHOT_VERSION);
		packet.put_u16(seq);
		packet.put_u16(base_seq);
		packet.put_u32(p_now);
		packet.put_u16(pr.cmd_ack);
		put_uvarint(&packet, pr.uplink_lag_ms);
		put_uvarint(&packet, pr.uplink_pps);
		put_uvarint(&packet, pr.update_rate);
		put_channel(packet, up, block_flags, block, datagram);
		packet.put_u16(spawn_ct);
		packet.put_data(spawn_body);
		packet.put_u16(despawn_ct);
		packet.put_data(despawn_body);
		packet.put_u16(leave_ct);
		packet.put_data(leave_body);
		packet.put_u16(changed);
		packet.put_data(body);
		p_host.send(peer, packet);
		charge(peer, p_now, packet.size() + (size_t)packet_overhead_);
	}
}

} // namespace goldnet
