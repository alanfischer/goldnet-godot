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
	int mismatches = 0;
	uint32_t cur = 0;
};

struct TestClientHost {
	Peer *p;
	void protocol_mismatch() { p->mismatches++; }
	void server_time(uint32_t ms) { p->times.push_back(ms); }
	void send_ack(uint16_t seq) { p->acks.push_back(seq); }
	void spawn(uint32_t net_id, uint32_t, const uint8_t *, size_t) { p->spawned.push_back(net_id); }
	void despawn(uint32_t net_id) { p->despawned.push_back(net_id); }
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
				p.client.apply(pkt.data(), pkt.size(), ch);
				if (!drop_acks.count(p.id)) {
					for (size_t i = acks_before; i < p.acks.size(); i++) {
						server.on_ack(p.id, p.acks[i]);
					}
				}
			}
		}
	}

	size_t sent_to(int32_t peer) { return world.outbox.count(peer) ? world.outbox[peer].size() : 0; }
	const std::vector<uint8_t> &last_packet(int32_t peer) { return world.outbox[peer].back(); }
};

static uint16_t base_seq_of(const std::vector<uint8_t> &pkt) { return (uint16_t)(pkt[3] | (pkt[4] << 8)); }

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
	p.client.apply(old_pkt.data(), old_pkt.size(), ch);
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
	Client<Val>::ApplyInfo info = p.client.apply(pkt.data(), pkt.size() - 1, ch);
	CHECK(!info.committed);
	CHECK(p.acks.size() == acks);
	printf("  truncated packet is not committed or acked: ok\n");
}

static void test_protocol_mismatch_warns_once() {
	Peer p;
	uint8_t bad[] = { (uint8_t)(SNAPSHOT_VERSION + 1), 0, 0, 0, 0 };
	TestClientHost ch{ &p };
	p.client.apply(bad, sizeof(bad), ch);
	p.client.apply(bad, sizeof(bad), ch);
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
	n.server.set_tick_interval_ms(16);
	for (int i = 0; i < 20; i++) {
		n.add_entity(10 + i, 100 + i, { 0 });
	}
	n.world.ents[29].priority = 50.0f;
	n.tick();
	n.server.set_bandwidth_bps(600); // 9 bytes a packet: two 7-byte entities a tick
	for (int i = 0; i < 20; i++) {
		n.world.ents[10 + i].vals[0] = 1;
	}
	n.tick();
	CHECK(p.vals[119][0] == 1); // the heavy one goes first, though it registered last
	CHECK(p.vals[118][0] == 0);
	for (int i = 0; i < 40; i++) {
		n.tick();
	}
	for (int i = 0; i < 20; i++) {
		CHECK(p.vals[100 + i][0] == 1); // nothing starves
	}
	printf("  budget overflow drains, highest priority first: ok\n");
}

static void test_peer_interval_throttles_one_peer() {
	Net n;
	n.add_peer(2);
	Peer &slow = n.add_peer(3);
	n.add_entity(10, 100, { 0 });
	n.server.set_peer_interval_ms(3, 50);
	int fast_sent = 0, slow_sent = 0;
	for (int i = 0; i < 30; i++) {
		n.world.ents[10].vals[0] = i;
		n.tick();
		fast_sent += (int)n.sent_to(2);
		slow_sent += (int)n.sent_to(3);
	}
	CHECK(fast_sent == 30);
	CHECK(slow_sent == 8); // 16 ms ticks: the first one at or past 50 ms is every 4th
	CHECK(slow.vals[100][0] >= 26);           // and it still converges
	printf("  per-peer interval throttles only that peer: ok\n");
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
	n.server.set_peer_rate(2, 1000);
	n.tick();                          // the first snapshot goes out
	n.server.charge(2, n.now, 250);    // then the host sends two RPC bursts, 500 bytes in all:
	n.server.charge(2, n.now, 250);    // half a second's worth, so they queue behind each other
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
	test_leave_and_reentry();
	test_leave_lost_is_rederived();
	test_reentry_before_leave_acked();
	test_budget_overflow_drains_by_priority();
	test_peer_interval_throttles_one_peer();
	test_push_declared_slots_skip_reads();
	test_warmup_defer_then_accept();
	test_removed_entity_leaves_client_frame();
	test_rate_chokes_to_the_budget();
	test_charged_traffic_delays_the_next_snapshot();
	printf("%d checks passed\n", checks);
	return 0;
}
