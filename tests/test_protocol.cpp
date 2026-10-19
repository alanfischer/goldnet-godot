// The snapshot protocol end to end, out of engine: a real goldnet::Server feeding real
// goldnet::Clients over a fake link that can drop snapshots or acks. Values are plain ints.
//
// These are the behaviours the integration suite can only reach through a running Godot pair
// and wall-clock timing: delta against the acked baseline, self-heal after loss, ack aging,
// reliable spawns and despawns, derived PVS leaves, the packet budget and its priority order,
// per-peer cadence, the clock keepalive, and malformed input.

#include "test_support.h"

#include "goldnet/client.h"
#include "goldnet/server.h"

#include <map>
#include <set>
#include <string>

using namespace goldnet;

typedef int64_t Val;
typedef Server<Val> TestServer;

// --- the world the server replicates ---

struct WorldEnt {
	std::vector<Val> vals;
	std::vector<uint8_t> push;
	float priority = 1.0f;
	std::set<int32_t> visible_to; // empty = visible to everyone
	bool hidden = false;          // visible to no one
};

struct World {
	std::map<uint64_t, WorldEnt> ents;
	std::map<int32_t, std::vector<std::vector<uint8_t>>> outbox; // peer -> packets sent this tick
	int reads = 0;

	bool is_visible(uint64_t key, int32_t peer) const {
		auto it = ents.find(key);
		if (it == ents.end() || it->second.hidden) {
			return false;
		}
		return it->second.visible_to.empty() || it->second.visible_to.count(peer);
	}
};

struct TestServerHost {
	World *world;
	TestServer *server;

	bool live(TestServer::Entity &e) { return world->ents.count(e.key) > 0; }
	void prepare(TestServer::Entity &e) {
		const WorldEnt &w = world->ents[e.key];
		e.slots = (int)w.vals.size();
		e.push = w.push;
		e.priority = w.priority;
	}
	void read(TestServer::Entity &e, bool polled_only) {
		world->reads++;
		const WorldEnt &w = world->ents[e.key];
		for (int s = 0; s < e.slots; s++) {
			if (polled_only && TestServer::skip_on_poll(e, s)) {
				continue;
			}
			server->stamp(e, s, w.vals[s]);
		}
	}
	bool visible(const TestServer::Entity &e, int32_t peer) { return world->is_visible(e.key, peer); }
	bool spawn_visible(uint64_t key, int32_t peer) { return world->is_visible(key, peer); }
	void encode(Writer &w, const Val &v, uint8_t, uint32_t) {
		w.put_u8(TAG_INT);
		put_varint(&w, v);
	}
	void send(int32_t peer, const Writer &w) {
		world->outbox[peer].emplace_back(w.data(), w.data() + w.size());
	}
};

// --- one client: what it has applied, and what it acked ---

struct Peer {
	int32_t id;
	Client<Val> client;
	std::map<uint32_t, int> slot_count;       // registered entities (net_id -> slots)
	std::set<uint32_t> not_ready;             // registered but not in the tree yet
	std::map<uint32_t, std::vector<Val>> vals; // what has been applied
	std::map<uint32_t, int> synced;           // net_id -> times `synchronized` fired
	std::vector<uint32_t> spawned, despawned, lost;
	std::vector<uint16_t> acks;
	std::vector<uint32_t> times;
	std::vector<uint16_t> command_acks;
	std::vector<std::pair<uint32_t, uint32_t>> uplink_reports; // lag_ms, pps
	std::vector<uint32_t> update_rates;
	std::vector<std::pair<uint16_t, bool>> chan_acks;
	std::vector<std::pair<std::vector<uint8_t>, bool>> messages; // bytes, reliable
	std::vector<std::string> events; // spawns, messages and despawns, in the order the host saw them
	int mismatches = 0;
	int refuse_spawns = 0; // the host can't create the next N spawns yet
	std::map<uint32_t, std::pair<uint32_t, int>> creates; // spawn net_id -> the entity it registers
	uint32_t cur = 0;
};

struct TestClientHost {
	Peer *p;
	void protocol_mismatch() { p->mismatches++; }
	void server_time(uint32_t ms) { p->times.push_back(ms); }
	void command_ack(uint16_t seq) { p->command_acks.push_back(seq); }
	void uplink_report(uint32_t lag_ms, uint32_t pps) { p->uplink_reports.push_back({ lag_ms, pps }); }
	void update_rate(uint32_t hz) { p->update_rates.push_back(hz); }
	void send_ack(uint16_t seq) { p->acks.push_back(seq); }
	void channel_ack(const ChannelAck &a) { p->chan_acks.push_back({ a.received, a.bit }); }
	void message(const uint8_t *data, size_t size, bool reliable) {
		p->messages.push_back({ std::vector<uint8_t>(data, data + size), reliable });
		p->events.push_back((reliable ? "r" : "u") + std::to_string(data[0]));
	}
	bool spawn(uint32_t net_id, uint32_t, const uint8_t *, size_t) {
		if (p->refuse_spawns > 0) {
			p->refuse_spawns--;
			return false;
		}
		p->spawned.push_back(net_id);
		p->events.push_back("s" + std::to_string(net_id));
		auto it = p->creates.find(net_id);
		if (it != p->creates.end()) {
			p->slot_count[it->second.first] = it->second.second;
		}
		return true;
	}
	void despawn(uint32_t net_id) {
		p->despawned.push_back(net_id);
		p->events.push_back("d" + std::to_string(net_id));
	}
	void relevance_lost(uint32_t net_id) { p->lost.push_back(net_id); }
	bool registered(uint32_t net_id) { return p->slot_count.count(net_id) > 0; }
	int begin_entity(uint32_t net_id, uint32_t mask, bool &r_defer) {
		p->cur = net_id;
		if (!p->slot_count.count(net_id)) {
			return -1;
		}
		if (p->not_ready.count(net_id)) {
			r_defer = mask != 0;
			return -1;
		}
		std::vector<Val> &v = p->vals[net_id];
		v.resize(p->slot_count[net_id]);
		return p->slot_count[net_id];
	}
	bool set_slot(int s, const Val &v) {
		p->vals[p->cur][s] = v;
		return true;
	}
	void end_entity(bool applied) {
		if (applied) {
			p->synced[p->cur]++;
		}
	}
	Val decode(Reader &r, uint32_t) {
		r.get_u8();
		return get_varint(&r);
	}
};

// --- the harness: one server, N peers, a lossy link ---

struct Net {
	World world;
	TestServer server;
	std::map<int32_t, Peer> peers;
	uint32_t now = 1000;
	std::set<int32_t> drop_snapshots; // peers whose next snapshots are lost
	std::set<int32_t> drop_acks;      // peers whose acks are lost

	Peer &add_peer(int32_t id) {
		Peer &p = peers[id];
		p.id = id;
		return p;
	}

	void add_entity(uint64_t key, uint32_t net_id, std::vector<Val> vals) {
		world.ents[key].vals = vals;
		server.add_entity(key, net_id);
		for (auto &kv : peers) {
			kv.second.slot_count[net_id] = (int)vals.size();
		}
	}

	// One server tick, then deliver every packet and every ack the link lets through.
	void tick(uint32_t dt = 16) {
		now += dt;
		std::vector<int32_t> ids;
		for (auto &kv : peers) {
			ids.push_back(kv.first);
		}
		world.outbox.clear();
		TestServerHost sh{ &world, &server };
		server.tick(now, ids, sh);
		for (auto &kv : world.outbox) {
			Peer &p = peers[kv.first];
			for (auto &pkt : kv.second) {
				if (drop_snapshots.count(p.id)) {
					continue;
				}
				size_t acks_before = p.acks.size();
				TestClientHost ch{ &p };
				p.client.apply(pkt.data(), pkt.size(), ch, now);
				if (!drop_acks.count(p.id)) {
					for (size_t i = acks_before; i < p.acks.size(); i++) {
						server.on_ack(p.id, p.acks[i], now);
					}
					server.on_channel_ack(p.id, p.client.channel_ack().at(now));
				}
			}
		}
	}

	size_t sent_to(int32_t peer) { return world.outbox.count(peer) ? world.outbox[peer].size() : 0; }
	const std::vector<uint8_t> &last_packet(int32_t peer) { return world.outbox[peer].back(); }
};

static uint16_t base_seq_of(const std::vector<uint8_t> &pkt) { return (uint16_t)(pkt[3] | (pkt[4] << 8)); }
// With an empty channel part and no uplink report, the spawn count follows the 11-byte header, the
// report's two 1-byte zeros and the channel part's 5 bytes.
static uint16_t spawn_ct_of(const std::vector<uint8_t> &pkt) { return (uint16_t)(pkt[18] | (pkt[19] << 8)); }

// --- tests ---

static void test_baseline_then_delta() {
	Net n;
	Peer &p = n.add_peer(2);
	n.add_entity(10, 100, { 1, 2, 3 });
	n.tick();
	CHECK(p.vals[100] == (std::vector<Val>{ 1, 2, 3 }));
	CHECK(base_seq_of(n.last_packet(2)) == 0); // first send is a full frame
	size_t full = n.last_packet(2).size();

	n.world.ents[10].vals[1] = 20;
	n.tick();
	CHECK(p.vals[100] == (std::vector<Val>{ 1, 20, 3 }));
	CHECK(base_seq_of(n.last_packet(2)) != 0); // a delta against the acked frame
	CHECK(n.last_packet(2).size() < full);     // carrying one slot, not three
	printf("  baseline then delta: ok\n");
}

static void test_idle_sends_nothing_until_keepalive() {
	Net n;
	n.add_peer(2);
	n.add_entity(10, 100, { 1 });
	n.tick();
	n.tick();
	CHECK(n.sent_to(2) == 0); // nothing changed: no header-only packet
	int sent = 0;
	for (int i = 0; i < 20; i++) { // 320 ms
		n.tick();
		sent += (int)n.sent_to(2);
	}
	CHECK(sent == 1); // one keepalive for the clock feed, every CLOCK_KEEPALIVE_MS
	printf("  idle: nothing but the clock keepalive: ok\n");
}

static void test_lost_snapshot_self_heals() {
	Net n;
	Peer &p = n.add_peer(2);
	n.add_entity(10, 100, { 1 });
	n.tick();
	n.world.ents[10].vals[0] = 5;
	n.drop_snapshots.insert(2);
	n.tick();
	CHECK(p.vals[100][0] == 1);
	n.drop_snapshots.clear();
	n.tick(); // nothing new changed, but the peer never acked the change, so it goes again
	CHECK(p.vals[100][0] == 5);
	printf("  lost snapshot self-heals: ok\n");
}

static void test_lost_ack_resends_against_old_baseline() {
	Net n;
	Peer &p = n.add_peer(2);
	n.add_entity(10, 100, { 1, 1 });
	n.tick();
	n.drop_acks.insert(2);
	n.world.ents[10].vals[0] = 7;
	n.tick();
	n.tick();
	CHECK(n.sent_to(2) == 1); // the change is re-carried while the ack is missing
	CHECK(p.vals[100][0] == 7);
	n.drop_acks.clear();
	n.tick();
	n.tick();
	CHECK(n.sent_to(2) == 0); // acked: settled
	printf("  lost ack resends against the old baseline: ok\n");
}

static void test_aged_out_ack_falls_back_to_full_frame() {
	Net n;
	Peer &p = n.add_peer(2);
	n.add_entity(10, 100, { 1 });
	n.add_entity(11, 101, { 9 });
	n.tick();
	n.drop_acks.insert(2);
	for (int i = 0; i < RING + 2; i++) { // the acked frame ages out of the ring
		n.world.ents[10].vals[0] = 100 + i;
		n.tick();
	}
	CHECK(base_seq_of(n.last_packet(2)) == 0);
	n.drop_acks.clear();
	n.world.ents[10].vals[0] = 1;
	n.tick();
	CHECK(p.vals[100][0] == 1);
	CHECK(p.vals[101][0] == 9);
	printf("  aged-out ack falls back to a full frame: ok\n");
}

static void test_unknown_baseline_is_dropped() {
	Net n;
	Peer &p = n.add_peer(2);
	n.add_entity(10, 100, { 1 });
	n.tick();
	// A client that lost its history receives a delta against a frame it never had.
	p.client.reset();
	n.world.ents[10].vals[0] = 2;
	n.drop_acks.insert(2);
	n.tick();
	CHECK(p.vals[100][0] == 1); // dropped, not misapplied
	printf("  delta against an unknown baseline is dropped: ok\n");
}

static void test_reordered_snapshot_is_dropped() {
	Net n;
	Peer &p = n.add_peer(2);
	n.add_entity(10, 100, { 1 });
	n.tick();
	std::vector<uint8_t> old_pkt = n.last_packet(2);
	n.world.ents[10].vals[0] = 2;
	n.tick();
	TestClientHost ch{ &p };
	p.client.apply(old_pkt.data(), old_pkt.size(), ch, n.now);
	CHECK(p.vals[100][0] == 2);
	printf("  reordered snapshot is dropped: ok\n");
}

static void test_truncated_packet_not_committed() {
	Net n;
	Peer &p = n.add_peer(2);
	n.add_entity(10, 100, { 1, 2 });
	n.tick();
	n.world.ents[10].vals[0] = 3;
	n.drop_snapshots.insert(2);
	n.tick();
	std::vector<uint8_t> pkt = n.last_packet(2);
	size_t acks = p.acks.size();
	TestClientHost ch{ &p };
	Client<Val>::ApplyInfo info = p.client.apply(pkt.data(), pkt.size() - 1, ch, n.now);
	CHECK(!info.committed);
	CHECK(p.acks.size() == acks);
	printf("  truncated packet is not committed or acked: ok\n");
}

static void test_protocol_mismatch_warns_once() {
	Peer p;
	uint8_t bad[] = { (uint8_t)(SNAPSHOT_VERSION + 1), 0, 0, 0, 0 };
	TestClientHost ch{ &p };
	p.client.apply(bad, sizeof(bad), ch, 0);
	p.client.apply(bad, sizeof(bad), ch, 0);
	CHECK(p.mismatches == 1);
	CHECK(p.acks.empty());
	printf("  protocol mismatch warns once: ok\n");
}

static void test_spawn_is_lazy_and_reliable() {
	Net n;
	Peer &a = n.add_peer(2);
	Peer &b = n.add_peer(3);
	n.add_entity(10, 100, { 1 });
	n.world.ents[10].visible_to = { 2 };
	TestServer::SpawnRecord rec;
	rec.spawner_net_id = 7;
	rec.vis_keys = { 10 };
	n.server.add_spawn(500, rec);

	n.drop_snapshots.insert(2);
	n.tick();
	n.tick();
	n.drop_snapshots.clear();
	n.tick();
	CHECK(a.spawned == std::vector<uint32_t>{ 500 }); // resent until it got through
	CHECK(b.spawned.empty());                         // never visible to peer 3
	n.tick();
	CHECK(a.spawned.size() == 1); // acked: not sent again

	n.server.despawn(500);
	n.tick();
	CHECK(a.despawned == std::vector<uint32_t>{ 500 });
	CHECK(b.despawned.empty()); // never had it, so isn't owed a despawn
	printf("  spawns are lazy and reliable; despawns go to holders only: ok\n");
}

// With acks lost the server resends the spawn and the despawn every tick; the host must still see
// each once. A spawn the host refuses isn't counted as held, so a resend gets another try.
static void test_resent_spawn_reaches_host_once() {
	Net n;
	Peer &a = n.add_peer(2);
	n.add_entity(10, 100, { 1 });
	n.server.add_spawn(500, TestServer::SpawnRecord());
	a.refuse_spawns = 2;
	n.drop_acks.insert(2);
	n.tick();
	n.tick();
	CHECK(a.spawned.empty()); // refused twice
	n.tick();
	n.tick();
	n.tick();
	CHECK(a.spawned == std::vector<uint32_t>{ 500 }); // accepted on the third, resends ignored

	n.server.despawn(500);
	n.tick();
	n.tick();
	n.tick();
	CHECK(a.despawned == std::vector<uint32_t>{ 500 });

	n.server.add_spawn(502, TestServer::SpawnRecord());
	a.refuse_spawns = 100;
	n.tick();
	n.server.despawn(502);
	n.tick();
	n.tick();
	CHECK(a.despawned == std::vector<uint32_t>{ 500 }); // never held 502, so no despawn for it

	a.client.reset();
	a.refuse_spawns = 0;
	n.drop_acks.clear();
	n.server.add_spawn(500, TestServer::SpawnRecord());
	n.tick();
	CHECK(a.spawned == (std::vector<uint32_t>{ 500, 500 })); // a reused id spawns again
	printf("  resent spawns and despawns reach the host once: ok\n");
}

// A refused spawn holds the ack, even on the very first frame: acking would retire the spawn as
// delivered, and the next copy comes only once the last is overdue.
static void test_refused_spawn_holds_ack() {
	Net n;
	Peer &a = n.add_peer(2);
	n.add_entity(10, 100, { 1 });
	n.server.add_spawn(500, TestServer::SpawnRecord());
	a.refuse_spawns = RELIABLE_BACKUP + 1; // every copy of the first burst
	for (int i = 0; i < 20 && a.spawned.empty(); i++) {
		n.tick();
	}
	CHECK(a.spawned == std::vector<uint32_t>{ 500 });
	n.tick();
	n.tick();
	CHECK(a.spawned.size() == 1);
	CHECK(n.sent_to(2) == 0 || base_seq_of(n.last_packet(2)) != 0); // acked and back on deltas
	printf("  a refused spawn holds the ack until it's accepted: ok\n");
}

// GoldSrc's reliable channel resends on loss, not in every packet. With no ack coming back the
// spawn goes in a burst of 1 + RELIABLE_BACKUP, then once more per overdue interval — not every tick.
static void test_unacked_spawn_resends_only_when_overdue() {
	Net n;
	n.add_peer(2);
	n.add_entity(10, 100, { 1 });
	n.server.add_spawn(500, TestServer::SpawnRecord());
	n.drop_acks.insert(2);
	int carried = 0;
	const int ticks = 30;
	for (int i = 0; i < ticks; i++) {
		n.tick();
		if (n.sent_to(2) && spawn_ct_of(n.last_packet(2)) > 0) {
			carried++;
		}
	}
	// 480 ms with no round trip timed: overdue is 66 ms, so a burst about every 5 ticks.
	CHECK(carried >= RELIABLE_BACKUP + 1);
	CHECK(carried < ticks / 2);
	printf("  an unacked spawn resends only when overdue (%d of %d snapshots): ok\n", carried, ticks);
}

// An ack of a snapshot that did NOT carry the spawn proves nothing: the client may have lost every
// carrier and still committed a later frame. The spawn must come again.
static void test_ack_of_a_later_frame_does_not_retire_a_spawn() {
	Net n;
	Peer &a = n.add_peer(2);
	n.add_entity(10, 100, { 1 });
	n.server.add_spawn(500, TestServer::SpawnRecord());
	n.drop_snapshots.insert(2);
	for (int i = 0; i <= RELIABLE_BACKUP; i++) {
		n.tick();
	}
	n.drop_snapshots.clear();
	n.world.ents[10].vals[0] = 2; // gives the next frames something to carry, and acks to send
	n.tick();
	CHECK(a.spawned.empty());
	CHECK(!a.acks.empty());
	for (int i = 0; i < 20 && a.spawned.empty(); i++) {
		n.tick();
	}
	CHECK(a.spawned == std::vector<uint32_t>{ 500 });
	printf("  an ack of a later frame does not retire a spawn: ok\n");
}

// A client that missed every carrier of a spawn discards its entity's deltas and acks them anyway.
// Once the spawn lands, the entity must arrive whole, not as a delta against values it never had.
static void test_entity_sent_whole_until_its_spawn_is_proven() {
	Net n;
	Peer &a = n.add_peer(2);
	n.add_entity(10, 100, { 1, 2 });
	a.slot_count.erase(100);
	a.creates[500] = { 100, 2 };
	TestServer::SpawnRecord rec;
	rec.vis_keys = { 10 };
	n.server.add_spawn(500, rec);
	n.drop_snapshots.insert(2);
	for (int i = 0; i <= RELIABLE_BACKUP; i++) {
		n.tick();
	}
	n.drop_snapshots.clear();
	n.tick(); // the entity, to a client that doesn't know it: discarded, and acked
	CHECK(a.spawned.empty());
	n.world.ents[10].vals[0] = 5; // only slot 0 changes from here on
	for (int i = 0; i < 20 && a.spawned.empty(); i++) {
		n.tick();
	}
	CHECK(a.spawned == std::vector<uint32_t>{ 500 });
	CHECK(a.vals[100] == (std::vector<Val>{ 5, 2 })); // slot 1 too, though it never changed
	printf("  an entity goes whole until its spawn is proven: ok\n");
}

// A despawn follows the same rule: a burst, then only when overdue, until an ack proves it.
static void test_lost_despawn_is_resent() {
	Net n;
	Peer &a = n.add_peer(2);
	n.add_entity(10, 100, { 1 });
	n.server.add_spawn(500, TestServer::SpawnRecord());
	n.tick();
	n.tick();
	CHECK(a.spawned.size() == 1);
	n.server.despawn(500);
	n.drop_snapshots.insert(2);
	for (int i = 0; i <= RELIABLE_BACKUP; i++) {
		n.tick();
	}
	n.drop_snapshots.clear();
	for (int i = 0; i < 20 && a.despawned.empty(); i++) {
		n.world.ents[10].vals[0]++; // keep snapshots going
		n.tick();
	}
	CHECK(a.despawned == std::vector<uint32_t>{ 500 });
	printf("  a lost despawn is resent once overdue: ok\n");
}

static void test_leave_and_reentry() {
	Net n;
	n.server.set_relevance_events(true);
	Peer &p = n.add_peer(2);
	n.add_entity(10, 100, { 4 });
	n.tick();
	n.tick();
	n.world.ents[10].hidden = true;
	n.tick();
	CHECK(p.lost == std::vector<uint32_t>{ 100 });
	n.tick();
	CHECK(p.lost.size() == 1); // acked: not re-sent

	// Re-entry with no change still ships a full baseline, so the client has something to show.
	int before = p.synced[100];
	n.world.ents[10].hidden = false;
	n.tick();
	CHECK(p.synced[100] == before + 1);
	printf("  leave once, re-enter with a full baseline: ok\n");
}

static void test_leave_lost_is_rederived() {
	Net n;
	n.server.set_relevance_events(true);
	Peer &p = n.add_peer(2);
	n.add_entity(10, 100, { 4 });
	n.tick();
	n.world.ents[10].hidden = true;
	n.drop_snapshots.insert(2);
	n.tick();
	CHECK(p.lost.empty());
	n.drop_snapshots.clear();
	n.tick();
	CHECK(p.lost == std::vector<uint32_t>{ 100 });
	printf("  a lost leave is re-derived next tick: ok\n");
}

// Back in view before the leave was acked: the server's acked baseline still lists the entity,
// but the client has already dropped it, so a slot delta (empty for an idle entity) would leave
// it hidden for good. It has to come back as a full baseline.
static void test_reentry_before_leave_acked() {
	Net n;
	n.server.set_relevance_events(true);
	Peer &p = n.add_peer(2);
	n.add_entity(10, 100, { 4 });
	n.tick();
	n.world.ents[10].hidden = true;
	n.drop_acks.insert(2);
	n.tick();
	CHECK(p.lost == std::vector<uint32_t>{ 100 });
	int before = p.synced[100];
	n.world.ents[10].hidden = false;
	n.tick();
	CHECK(p.synced[100] == before + 1);
	printf("  re-entry before the leave is acked ships a full baseline: ok\n");
}

static void test_budget_overflow_drains_by_priority() {
	Net n;
	Peer &p = n.add_peer(2);
	const int count = 400; // 7 bytes each, so a packet carries well under half of them
	for (int i = 0; i < count; i++) {
		n.add_entity(10 + i, 100 + i, { 0 });
	}
	n.world.ents[10 + count - 1].priority = 50.0f;
	for (int i = 0; i < 10; i++) {
		n.tick(); // the first full baseline overflows too; let it drain
	}
	for (int i = 0; i < count; i++) {
		n.world.ents[10 + i].vals[0] = 1;
	}
	n.tick();
	CHECK(p.vals[100 + count - 1][0] == 1); // the heavy one goes first, though it registered last
	CHECK(p.vals[100 + count - 2][0] == 0);
	for (int i = 0; i < 40; i++) {
		n.tick();
	}
	for (int i = 0; i < count; i++) {
		CHECK(p.vals[100 + i][0] == 1); // nothing starves
	}
	printf("  budget overflow drains, highest priority first: ok\n");
}

static void test_update_rate_throttles_one_peer() {
	Net n;
	Peer &fast = n.add_peer(2);
	Peer &slow = n.add_peer(3);
	n.add_entity(10, 100, { 0 });
	n.server.set_snapshot_rate(60.0);
	n.server.set_peer_update_rate(3, 20);
	int fast_sent = 0, slow_sent = 0;
	for (int i = 0; i < 30; i++) {
		n.world.ents[10].vals[0] = i;
		n.tick(i % 3 == 2 ? 17 : 16); // about 60 Hz, as a jittery millisecond clock sees it
		fast_sent += (int)n.sent_to(2);
		slow_sent += (int)n.sent_to(3);
	}
	CHECK(fast_sent == 30);
	CHECK(slow_sent == 10);             // every 3rd tick, though the gaps measure 49 ms
	CHECK(slow.vals[100][0] >= 27);     // and it still converges
	CHECK(fast.update_rates.back() == 60 && slow.update_rates.back() == 20);
	printf("  an update rate throttles only that peer, every nth tick: ok\n");
}

// A grant is what the peer asks for, held to a whole number of ticks, the snapshot rate, its rate's
// share and MIN_UPDATE_RATE.
static void test_update_rate_grant() {
	Net n;
	n.add_peer(2);
	n.add_entity(10, 100, { 0 });
	auto granted = [&n](uint32_t asked, uint32_t rate) {
		n.server.set_peer_update_rate(2, asked);
		n.server.set_peer_rate(2, rate);
		n.tick(1000);
		return n.server.update_rate(2);
	};
	CHECK(granted(0, 0) == 0); // nothing asked, snapshot rate unknown: every tick
	CHECK(granted(25, 0) == 25);
	CHECK(granted(1, 0) == 10);
	n.server.set_snapshot_rate(60.0);
	CHECK(granted(0, 0) == 60);
	CHECK(granted(100, 0) == 60);
	CHECK(granted(45, 0) == 30); // 60 Hz can't serve 45: every 2nd tick
	CHECK(granted(20, 0) == 20);
	CHECK(granted(1, 0) == 10);
	// 3000 B/s, 30% of it in idle snapshots of IDLE_SNAPSHOT_BYTES: 33 Hz, which is every 2nd tick
	CHECK(granted(0, 3000) == 30);
	CHECK(granted(0, 1000) == 10); // the floor holds even when the rate can't carry it
	n.server.set_packet_overhead(50);
	CHECK(granted(0, 4000) == 15); // 15.6 Hz: every 4th tick
	printf("  update rate grants: ok\n");
}

static void test_push_declared_slots_skip_reads() {
	Net n;
	Peer &p = n.add_peer(2);
	n.world.ents[10].push = { 1 };
	n.add_entity(10, 100, { 1 });
	n.tick();
	int reads = n.world.reads;
	n.world.ents[10].vals[0] = 2; // written without a mark
	n.tick();
	CHECK(n.world.reads == reads); // fully declared and unmarked: not read
	CHECK(p.vals[100][0] == 1);
	n.server.mark_dirty(10);
	n.tick();
	CHECK(p.vals[100][0] == 2);
	printf("  push-declared slots skip reads until marked: ok\n");
}

static void test_warmup_defer_then_accept() {
	Net n;
	Peer &p = n.add_peer(2);
	n.add_entity(10, 100, { 1 });
	n.add_entity(11, 101, { 2 });
	p.not_ready.insert(101);
	n.tick(); // first frame is accepted regardless
	n.world.ents[11].vals[0] = 3;
	n.tick(); // 101 still not ready: not acked past it
	CHECK(p.vals[101].empty());
	p.not_ready.clear();
	n.tick();
	CHECK(p.vals[101] == std::vector<Val>{ 3 });
	printf("  warmup defer, then accept once ready: ok\n");
}

static void test_removed_entity_leaves_client_frame() {
	Net n;
	Peer &p = n.add_peer(2);
	n.add_entity(10, 100, { 1 });
	n.tick();
	n.server.remove_entity(10);
	n.world.ents.erase(10);
	p.slot_count.erase(100);
	n.add_entity(11, 101, { 2 });
	n.tick();
	CHECK(p.vals[101][0] == 2);
	printf("  removed entity is pruned and the stream continues: ok\n");
}

// GoldSrc rate / choke: a rated peer's snapshot bytes stay within its rate, it still converges, and
// a choke costs nothing an unrated peer would notice.
static void test_rate_chokes_to_the_budget() {
	Net n;
	n.add_peer(2);
	Peer &slow = n.add_peer(3);
	for (int i = 0; i < 10; i++) {
		n.add_entity(10 + i, 100 + i, { 0, 0 });
	}
	n.server.set_peer_rate(3, 2000); // bytes per second
	size_t fast_bytes = 0, slow_bytes = 0;
	int fast_packets = 0, slow_packets = 0;
	for (int t = 0; t < 125; t++) { // 2 s
		for (int i = 0; i < 10; i++) {
			n.world.ents[10 + i].vals[0] = t; // everything moves every tick
		}
		n.tick();
		for (auto &pkt : n.world.outbox[2]) {
			fast_bytes += pkt.size();
			fast_packets++;
		}
		for (auto &pkt : n.world.outbox[3]) {
			slow_bytes += pkt.size();
			slow_packets++;
		}
	}
	CHECK(fast_packets == 125);                  // unrated: every tick
	CHECK(slow_bytes <= 2000 * 2 + 200);         // rated: within its budget (plus the packet in hand)
	CHECK(slow_packets < fast_packets / 2);      // by sending less often, not by sending nothing
	CHECK(n.server.chokes(3) > 0);
	CHECK(n.server.chokes(2) == 0);
	CHECK(slow.vals[100][0] >= 120);             // and still current, not a backlog behind
	printf("  rate chokes a peer to its budget, and it stays current: ok\n");
}

static void test_charged_traffic_delays_the_next_snapshot() {
	Net n;
	n.add_peer(2);
	n.add_entity(10, 100, { 0 });
	n.server.set_peer_rate(2, 10000);  // fast enough that its update rate isn't what holds it back
	n.tick();                          // the first snapshot goes out
	n.server.charge(2, n.now, 2500);   // then the host sends two RPC bursts, 5000 bytes in all:
	n.server.charge(2, n.now, 2500);   // half a second's worth, so they queue behind each other
	int sent = 0;
	for (int t = 0; t < 25; t++) {     // 400 ms
		n.world.ents[10].vals[0] = t + 1;
		n.tick();
		sent += (int)n.sent_to(2);
	}
	CHECK(sent == 0);
	CHECK(n.server.chokes(2) == 25);
	printf("  other traffic charged to a peer delays its next snapshot: ok\n");
}

// The command ack rides the header, and a new one is worth a snapshot even when nothing else is.
static void test_command_ack_rides_the_header() {
	Net n;
	Peer &p = n.add_peer(2);
	n.add_entity(10, 100, { 1 });
	n.tick();
	n.tick();
	CHECK(n.sent_to(2) == 0); // idle
	n.server.set_command_ack(2, 77);
	n.tick();
	CHECK(n.sent_to(2) == 1 && p.command_acks.back() == 77);
	n.tick();
	CHECK(n.sent_to(2) == 0); // told once
	printf("  the command ack rides the header and earns a send when new: ok\n");
}

// The server's measure of a client's uplink rides the header of that client's snapshots.
static void test_uplink_report_rides_the_header() {
	Net n;
	Peer &p = n.add_peer(2);
	n.add_entity(10, 100, { 1 });
	n.server.set_uplink_report(2, 340, 44);
	n.tick();
	CHECK(!p.uplink_reports.empty() && p.uplink_reports.back() == std::make_pair(340u, 44u));
	printf("  the uplink report rides the snapshot header: ok\n");
}

// A gap in the seqs a client receives is a lost snapshot. Idle ticks send nothing and take no
// seq, so they read as no loss at all, and a late snapshot is taken back off the running total.
static void test_arrival_stats_count_only_real_loss() {
	Net n;
	Peer &p = n.add_peer(2);
	n.add_entity(10, 100, { 1 });
	n.tick();
	for (int i = 0; i < 40; i++) { // 640 ms idle: two keepalives, ~38 skipped ticks
		n.tick();
	}
	auto s = p.client.arrival_stats();
	CHECK(s.received == 3);
	CHECK(s.lost == 0);

	n.drop_snapshots.insert(2);
	for (int v = 2; v < 5; v++) { // three changes, three snapshots, all lost
		n.world.ents[10].vals[0] = v;
		n.tick();
	}
	n.drop_snapshots.clear();
	n.world.ents[10].vals[0] = 9;
	n.tick();
	s = p.client.arrival_stats();
	CHECK(s.received == 4);
	CHECK(s.lost == 3);

	n.world.ents[10].vals[0] = 10;
	n.drop_snapshots.insert(2);
	n.tick();
	n.drop_snapshots.clear();
	std::vector<uint8_t> late = n.world.outbox[2].back();
	n.world.ents[10].vals[0] = 11;
	n.tick();
	TestClientHost ch{ &p };
	p.client.apply(late.data(), late.size(), ch, n.now);
	s = p.client.arrival_stats();
	CHECK(s.received == 6);
	CHECK(s.lost == 3); // the late one took its loss back off
	printf("  arrival stats count only real loss: ok\n");
}

// Messages ride the snapshot: the reliable ones before its spawns, the datagram's after the entity
// deltas and before its despawns, so an event can name an entity the same snapshot creates or removes.
static void test_messages_ride_the_snapshot_in_order() {
	Net n;
	Peer &a = n.add_peer(2);
	n.tick();
	const uint8_t r1[] = { 1 }, u2[] = { 2 }, r3[] = { 3 }, u4[] = { 4 };
	n.server.add_spawn(500, TestServer::SpawnRecord());
	n.server.queue_reliable(2, r1, 1);
	n.server.queue_unreliable(2, u2, 1);
	n.tick();
	CHECK((a.events == std::vector<std::string>{ "r1", "s500", "u2" }));
	n.server.despawn(500);
	n.server.queue_reliable(2, r3, 1);
	n.server.queue_unreliable(2, u4, 1);
	n.tick();
	CHECK((a.events == std::vector<std::string>{ "r1", "s500", "u2", "r3", "u4", "d500" }));
	CHECK(n.server.reliable_delivered(2) == 2);
	printf("  messages ride the snapshot, ordered around its spawns and despawns: ok\n");
}

// A snapshot that can't be baselined still delivers its messages, and a lost one's reliable block
// comes again once the client's ack shows it missing; its datagram doesn't.
static void test_messages_survive_a_lost_snapshot() {
	Net n;
	Peer &a = n.add_peer(2);
	n.add_entity(10, 100, { 1 });
	n.tick();
	const uint8_t r1[] = { 1 }, u2[] = { 2 };
	n.server.queue_reliable(2, r1, 1);
	n.server.queue_unreliable(2, u2, 1);
	n.drop_snapshots.insert(2);
	n.tick();
	n.drop_snapshots.clear();
	for (int i = 0; i < 4; i++) {
		n.world.ents[10].vals[0]++;
		n.tick();
	}
	CHECK((a.events == std::vector<std::string>{ "r1" }));
	CHECK(n.server.reliable_delivered(2) == 1);
	printf("  a lost snapshot's reliable block comes again, its datagram doesn't: ok\n");
}

int main() {
	printf("protocol:\n");
	test_baseline_then_delta();
	test_idle_sends_nothing_until_keepalive();
	test_lost_snapshot_self_heals();
	test_lost_ack_resends_against_old_baseline();
	test_aged_out_ack_falls_back_to_full_frame();
	test_unknown_baseline_is_dropped();
	test_reordered_snapshot_is_dropped();
	test_truncated_packet_not_committed();
	test_protocol_mismatch_warns_once();
	test_spawn_is_lazy_and_reliable();
	test_resent_spawn_reaches_host_once();
	test_command_ack_rides_the_header();
	test_refused_spawn_holds_ack();
	test_unacked_spawn_resends_only_when_overdue();
	test_ack_of_a_later_frame_does_not_retire_a_spawn();
	test_entity_sent_whole_until_its_spawn_is_proven();
	test_lost_despawn_is_resent();
	test_leave_and_reentry();
	test_leave_lost_is_rederived();
	test_reentry_before_leave_acked();
	test_budget_overflow_drains_by_priority();
	test_update_rate_throttles_one_peer();
	test_update_rate_grant();
	test_push_declared_slots_skip_reads();
	test_warmup_defer_then_accept();
	test_removed_entity_leaves_client_frame();
	test_rate_chokes_to_the_budget();
	test_charged_traffic_delays_the_next_snapshot();
	test_arrival_stats_count_only_real_loss();
	test_uplink_report_rides_the_header();
	test_messages_ride_the_snapshot_in_order();
	test_messages_survive_a_lost_snapshot();
	printf("%d checks passed\n", checks);
	return 0;
}
