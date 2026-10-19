// GoldSrc's netchan (netchan.h) between two ends over a link that can lose and reorder packets: the
// reliable stream arrives once, in order, and is resent only on an ack that proves it lost; a big
// message moves in a window of fragments, one per packet; the datagram is all-or-nothing and never
// plays late.

#include "test_support.h"

#include "goldnet/netchan.h"
#include "goldnet/uplink.h"

#include <deque>
#include <string>

using namespace goldnet;

typedef std::vector<std::vector<uint8_t>> Msgs;

static std::vector<uint8_t> msg(uint32_t n, size_t size = 6) {
	std::vector<uint8_t> m(size, 9);
	m[0] = (uint8_t)n;
	m[1] = (uint8_t)(n >> 8);
	m[2] = (uint8_t)(n >> 16);
	return m;
}

static uint32_t msg_id(const std::vector<uint8_t> &m) { return m[0] | (m[1] << 8) | (m[2] << 16); }

// One end of the channel, writing and reading bare channel packets: a seq, then the channel part.
struct End {
	ReliableSender rel;
	ChannelReceiver in;
	DatagramSender dg;
	uint16_t seq = 1;
	uint32_t now = 1000;
	Msgs got_rel, got_unrel;
	int blocks_sent = 0;
	int fragments_sent = 0;

	std::vector<uint8_t> packet() {
		const uint16_t s = seq;
		seq = command_seq_next(seq);
		now += 16;
		std::vector<uint8_t> part;
		const uint8_t flags = rel.write(s, part);
		blocks_sent += (flags & CHAN_RELIABLE) ? 1 : 0;
		fragments_sent += (flags & CHAN_FRAGMENT) ? 1 : 0;
		std::vector<uint8_t> datagram;
		dg.take(SAFE_PACKET_BYTES, datagram);
		Writer w;
		w.put_u16(s);
		put_channel(w, in.ack_source().at(now), flags, part, datagram);
		return std::vector<uint8_t>(w.data(), w.data() + w.size());
	}

	void receive(const std::vector<uint8_t> &p) {
		Reader r(p.data(), p.size());
		const uint16_t s = r.get_u16();
		ChannelPart c;
		c.read(r);
		CHECK(!r.failed());
		if (!in.accept(s)) {
			return;
		}
		rel.on_ack(c.ack);
		CHECK(c.take(in, now, got_rel, got_unrel));
	}
};

struct Link {
	End a, b;
	uint32_t rng = 777;
	int loss = 0; // percent, each way
	bool roll() { return loss > 0 && (int)(xorshift32(rng) % 100) < loss; }

	// One exchange: a packet each way, either of which may be lost.
	void step() {
		std::vector<uint8_t> ab = a.packet();
		std::vector<uint8_t> ba = b.packet();
		if (!roll()) {
			b.receive(ab);
		}
		if (!roll()) {
			a.receive(ba);
		}
	}
};

static void check_in_order(const Msgs &p_got, uint32_t p_count) {
	CHECK(p_got.size() == p_count);
	for (uint32_t i = 0; i < p_got.size(); i++) {
		CHECK(msg_id(p_got[i]) == i);
	}
}

static void test_reliable_in_order_once_under_loss() {
	Link l;
	l.loss = 30;
	uint32_t queued = 0;
	for (int t = 0; t < 2000; t++) {
		if (t < 1500 && t % 3 == 0) {
			std::vector<uint8_t> m = msg(queued, 5 + (queued % 40));
			queued++;
			l.a.rel.queue(m.data(), m.size());
		}
		l.step();
	}
	check_in_order(l.b.got_rel, queued);
	CHECK(l.a.rel.idle() && l.a.rel.delivered() == queued);
	printf("  reliable messages arrive once, in order, under 30%% loss: ok (%u)\n", queued);
}

static void test_one_block_in_flight() {
	Link l;
	for (uint32_t i = 0; i < 300; i++) {
		std::vector<uint8_t> m = msg(i, 100);
		l.a.rel.queue(m.data(), m.size());
	}
	// Each block waits for the last one's ack, so a block goes every other exchange at best: the
	// packet carrying it, then the one that comes back acking it.
	std::vector<uint8_t> first = l.a.packet();
	std::vector<uint8_t> second = l.a.packet();
	CHECK(l.a.blocks_sent == 1);
	l.b.receive(first);
	l.b.receive(second);
	l.a.receive(l.b.packet());
	l.a.packet();
	CHECK(l.a.blocks_sent == 2);
	CHECK(l.b.got_rel.size() == RELIABLE_BLOCK_BYTES / 101); // 100 bytes and a 1-byte length each
	printf("  one block in flight, as many messages as fit: ok\n");
}

static void test_resent_only_on_proof_of_loss() {
	End a, b;
	std::vector<uint8_t> m = msg(1);
	a.rel.queue(m.data(), m.size());
	a.packet(); // carries the block, and is lost
	for (int i = 0; i < 20; i++) {
		a.packet(); // no ack has come back: no timer resends it
	}
	CHECK(a.blocks_sent == 1);
	a.receive(b.packet()); // b received nothing from a: an ack of nothing proves nothing
	a.packet();
	CHECK(a.blocks_sent == 1);
	b.receive(a.packet()); // b receives a later packet without the block
	a.receive(b.packet()); // and acks it with the bit unflipped: the block was lost
	std::vector<uint8_t> resend = a.packet();
	CHECK(a.blocks_sent == 2);
	b.receive(resend);
	CHECK(b.got_rel.size() == 1 && msg_id(b.got_rel[0]) == 1);
	printf("  a block goes again only when an ack proves it lost: ok\n");
}

static void test_reordered_packets_never_deliver_twice_or_late() {
	End a, b;
	for (uint32_t i = 0; i < 3; i++) {
		std::vector<uint8_t> m = msg(i);
		a.rel.queue(m.data(), m.size());
	}
	std::vector<uint8_t> u0 = msg(100);
	a.dg.queue(u0.data(), u0.size());
	std::vector<uint8_t> p1 = a.packet(); // the block, and datagram 100
	std::vector<uint8_t> u1 = msg(101);
	a.dg.queue(u1.data(), u1.size());
	std::vector<uint8_t> p2 = a.packet(); // datagram 101
	b.receive(p2);
	b.receive(p1); // late: older than p2, so neither its block nor its datagram counts
	b.receive(p1); // and a duplicate changes nothing
	CHECK(b.got_rel.empty());
	CHECK(b.got_unrel.size() == 1 && msg_id(b.got_unrel[0]) == 101);
	a.receive(b.packet()); // acks p2 without the bit: the block was lost
	b.receive(a.packet());
	check_in_order(b.got_rel, 3);
	printf("  a late packet's messages never play, and nothing plays twice: ok\n");
}

// A link whose packets take p_delay exchanges to arrive, so several are in flight at once.
struct SlowLink {
	End a, b;
	int delay = 6;
	uint32_t rng = 99;
	int loss = 0;
	std::deque<std::pair<int, std::vector<uint8_t>>> ab, ba; // arrival step, packet
	int t = 0;
	void step() {
		t++;
		std::vector<uint8_t> p = a.packet();
		if (!(loss > 0 && (int)(xorshift32(rng) % 100) < loss)) {
			ab.push_back({ t + delay, std::move(p) });
		}
		std::vector<uint8_t> q = b.packet();
		if (!(loss > 0 && (int)(xorshift32(rng) % 100) < loss)) {
			ba.push_back({ t + delay, std::move(q) });
		}
		while (!ab.empty() && ab.front().first <= t) {
			b.receive(ab.front().second);
			ab.pop_front();
		}
		while (!ba.empty() && ba.front().first <= t) {
			a.receive(ba.front().second);
			ba.pop_front();
		}
	}
};

static std::vector<uint8_t> big_message() {
	std::vector<uint8_t> big(20000);
	for (size_t i = 0; i < big.size(); i++) {
		big[i] = (uint8_t)(i * 7);
	}
	return big;
}

static void test_fragments_fill_the_window() {
	SlowLink l;
	const std::vector<uint8_t> big = big_message();
	std::vector<uint8_t> before = msg(4), after = msg(5);
	l.a.rel.queue(before.data(), before.size());
	l.a.rel.queue(big.data(), big.size());
	l.a.rel.queue(after.data(), after.size());
	CHECK(!l.a.rel.overflowed()); // a fragmented message doesn't count toward the overflow
	int steps = 0;
	while (l.b.got_rel.size() < 3 && steps < 1000) {
		l.step();
		steps++;
	}
	CHECK(l.b.got_rel.size() == 3 && msg_id(l.b.got_rel[0]) == 4 && l.b.got_rel[1] == big && msg_id(l.b.got_rel[2]) == 5);
	const int fragments = (20000 + 3 + RELIABLE_BLOCK_BYTES - 1) / RELIABLE_BLOCK_BYTES; // its 3-byte length too
	CHECK(l.a.fragments_sent == fragments); // none twice on a lossless link
	// One fragment per packet, not one per round trip (which would be fragments * 2 * delay): the
	// fragments, plus a round trip each for the block before, the last fragment and the block after.
	CHECK(steps <= fragments + 6 * l.delay + 3);
	printf("  a big message fills the window, one fragment per packet, in order: ok (%d fragments, %d steps)\n",
			fragments, steps);
}

static void test_fragments_survive_loss_and_reorder() {
	SlowLink l;
	l.loss = 25;
	const std::vector<uint8_t> big = big_message();
	for (int i = 0; i < 3; i++) {
		l.a.rel.queue(big.data(), big.size());
		std::vector<uint8_t> m = msg(i);
		l.a.rel.queue(m.data(), m.size());
	}
	int steps = 0;
	while ((l.b.got_rel.size() < 6 || !l.a.rel.idle()) && steps < 5000) {
		l.step();
		steps++;
	}
	CHECK(l.b.got_rel.size() == 6);
	for (int i = 0; i < 3; i++) {
		CHECK(l.b.got_rel[2 * i] == big && msg_id(l.b.got_rel[2 * i + 1]) == (uint32_t)i);
	}
	CHECK(l.a.rel.idle() && l.a.rel.delivered() == 6);
	printf("  fragments survive 25%% loss, each message whole and in order: ok (%d steps)\n", steps);
}

static void test_fragment_ack_goes_quiet() {
	ChannelReceiver in;
	std::vector<std::vector<uint8_t>> got;
	const uint8_t d[] = { 1, 7 };
	CHECK(!in.ack_source().at(100).fragments); // never had one: says nothing
	CHECK(in.take_fragment(0, 0, d, 2, 100, got) && got.size() == 1);
	CHECK(in.ack_source().at(100 + FRAGMENT_ACK_MS - 1).fragments);
	CHECK(in.ack_source().at(100 + FRAGMENT_ACK_MS - 1).next_fragment == 1);
	CHECK(!in.ack_source().at(100 + FRAGMENT_ACK_MS).fragments);
	printf("  the fragment ack goes quiet a while after the last fragment: ok\n");
}

static void test_overflow() {
	ReliableSender s;
	std::vector<uint8_t> m = msg(1, 500);
	for (int i = 0; i < 140; i++) {
		s.queue(m.data(), m.size());
	}
	CHECK(s.overflowed());
	printf("  pending reliable data past the cap is an overflow: ok\n");
}

static void test_datagram_all_or_nothing() {
	DatagramSender d;
	std::vector<uint8_t> m = msg(1, 300);
	CHECK(d.queue(m.data(), m.size()));
	CHECK(d.queue(m.data(), m.size()));
	CHECK(d.queue(m.data(), m.size()));
	CHECK(!d.queue(m.data(), m.size())); // past a packet: dropped
	std::vector<uint8_t> out;
	d.take(500, out); // doesn't fit: none of it goes
	CHECK(out.empty() && d.empty() && d.dropped() == 2);
	CHECK(d.queue(m.data(), m.size()));
	d.take(500, out);
	Msgs got;
	CHECK(split_messages(out.data(), out.size(), got) && got.size() == 1 && got[0] == m);
	printf("  the datagram goes whole or not at all: ok\n");
}

// The uplink's channel: reliable messages before the commands, the datagram after them.
struct UpHost {
	std::vector<std::string> log;
	void protocol_mismatch(int32_t) {}
	void snapshot_ack(int32_t, uint16_t) {}
	void channel_ack(int32_t, const ChannelAck &) {}
	void command(int32_t, uint32_t, const uint8_t *data, size_t) { log.push_back("c" + std::to_string(data[0])); }
	void message(int32_t, const uint8_t *data, size_t, bool reliable) {
		log.push_back((reliable ? "r" : "u") + std::to_string(data[0]));
	}
};

static void test_uplink_order() {
	CommandSender s;
	std::vector<uint8_t> c = { 1 }, r = { 2 }, u = { 3 };
	s.queue_unreliable(u.data(), u.size());
	s.push(1, c.data(), c.size(), 10);
	s.queue_reliable(r.data(), r.size());
	CHECK(s.due(10));
	Writer w;
	s.write(10, 0, ChannelAckSource(), w);
	CommandReceiver recv;
	UpHost h;
	CHECK(recv.receive(2, w.data(), w.size(), h, 10));
	CHECK((h.log == std::vector<std::string>{ "r2", "c1", "u3" }));
	CHECK(recv.channel_ack(2).bit); // took the block
	CHECK(recv.channel_ack(2).received == 1);
	printf("  uplink: the reliable block, then the commands, then the datagram: ok\n");
}

int main() {
	printf("test_netchan\n");
	test_reliable_in_order_once_under_loss();
	test_one_block_in_flight();
	test_resent_only_on_proof_of_loss();
	test_reordered_packets_never_deliver_twice_or_late();
	test_fragments_fill_the_window();
	test_fragments_survive_loss_and_reorder();
	test_fragment_ack_goes_quiet();
	test_overflow();
	test_datagram_all_or_nothing();
	test_uplink_order();
	printf("all %d checks passed\n", checks);
	return 0;
}
