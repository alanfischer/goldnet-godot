// The uplink end to end: a CommandSender feeding a CommandReceiver over a link that can lose
// packets either way. The contract under test is a reliable RPC's: every command reaches the host
// once, in order, unless the client gave up on it.

#include "test_support.h"

#include "goldnet/uplink.h"

#include <set>

using namespace goldnet;

struct Delivered {
	uint32_t stream;
	std::vector<uint8_t> bytes;
};

struct RecvHost {
	std::vector<Delivered> got;
	std::vector<uint16_t> snapshot_acks;
	int mismatches = 0;
	void protocol_mismatch(int32_t) { mismatches++; }
	void snapshot_ack(int32_t, uint16_t seq) { snapshot_acks.push_back(seq); }
	void channel_ack(int32_t, const ChannelAck &) {}
	void message(int32_t, const uint8_t *, size_t, bool) {}
	void command(int32_t, uint32_t stream, const uint8_t *data, size_t size) {
		got.push_back({ stream, std::vector<uint8_t>(data, data + size) });
	}
};

// A command whose bytes say which one it is, so order and identity are checkable.
static std::vector<uint8_t> cmd_bytes(uint32_t n) {
	return { (uint8_t)n, (uint8_t)(n >> 8), (uint8_t)(n >> 16), 7, 7, 7, 7, 7 };
}

static uint32_t cmd_id(const std::vector<uint8_t> &b) { return b[0] | (b[1] << 8) | (b[2] << 16); }

struct Link {
	CommandSender sender;
	CommandReceiver receiver;
	RecvHost host;
	uint32_t now = 1000;
	uint32_t rng = 12345;
	int up_loss = 0;   // percent of uplink packets lost
	int down_loss = 0; // percent of acks lost
	uint32_t pushed = 0;
	size_t bytes_up = 0;
	int packets_up = 0;
	std::set<int> drop_next; // packet numbers to lose regardless of loss

	bool roll(int pct) { return pct > 0 && (int)(xorshift32(rng) % 100) < pct; }

	// One 16 ms tick: make a command, send if due, deliver, ack back.
	void tick(bool make = true, uint32_t stream = 1) {
		now += 16;
		if (make) {
			std::vector<uint8_t> b = cmd_bytes(pushed++);
			sender.push(stream, b.data(), b.size(), now);
		}
		if (!sender.due(now)) {
			return;
		}
		Writer w;
		sender.write(now, 0, ChannelAckSource(), w);
		const int n = packets_up++;
		bytes_up += w.size();
		if (roll(up_loss) || drop_next.count(n)) {
			return;
		}
		CHECK(receiver.receive(2, w.data(), w.size(), host, 0));
		if (!roll(down_loss)) {
			sender.on_ack(receiver.ack(2));
		}
	}

	void check_in_order(uint32_t from = 0) {
		for (size_t i = 0; i < host.got.size(); i++) {
			CHECK(cmd_id(host.got[i].bytes) == from + i);
		}
	}
};

static void test_lossless_delivers_each_once_in_order() {
	Link l;
	for (int i = 0; i < 200; i++) {
		l.tick();
	}
	CHECK(l.host.got.size() == 200);
	l.check_in_order();
	CHECK(l.sender.pending() <= 1); // the last one's ack is the only thing outstanding
	printf("  lossless: each command once, in order: ok\n");
}

static void test_loss_both_ways_still_delivers_everything() {
	for (uint32_t seed = 1; seed <= 5; seed++) {
		Link l;
		l.rng = seed * 7919;
		l.up_loss = 30;
		l.down_loss = 30;
		for (int i = 0; i < 1000; i++) {
			l.tick();
		}
		for (int i = 0; i < 100; i++) {
			l.tick(false); // drain: no new commands, overdue ones keep going
		}
		CHECK(l.host.got.size() == 1000);
		l.check_in_order();
		CHECK(l.receiver.skipped(2) == 0);
		CHECK(l.sender.dropped() == 0);
	}
	printf("  30%% loss each way: every command still once, in order: ok\n");
}

static void test_backups_cover_a_lost_packet_at_once() {
	Link l;
	l.tick();
	l.drop_next = { 1 };
	l.tick(); // lost
	size_t before = l.host.got.size();
	l.tick(); // carries the lost one as a backup
	CHECK(l.host.got.size() == before + 2);
	l.check_in_order();
	printf("  a lost packet's command rides the next one as a backup: ok\n");
}

static void test_burst_past_the_backups_resends_when_overdue() {
	Link l;
	l.tick();
	l.drop_next = { 1, 2, 3, 4 }; // more than COMMAND_BACKUP_DEFAULT can cover
	for (int i = 0; i < 4; i++) {
		l.tick();
	}
	l.tick(); // backups reach 2 back, so the first lost one is still missing: everything waits
	CHECK(l.host.got.size() == 1);
	for (int i = 0; i < 10; i++) {
		l.tick(); // overdue after ~66 ms
	}
	CHECK(l.host.got.size() == 16);
	l.check_in_order();
	printf("  a burst longer than the backups resends once overdue, holding order: ok\n");
}

static void test_given_up_commands_are_skipped() {
	Link l;
	l.tick();
	const int lost = COMMAND_RING + 10;
	for (int i = 0; i < lost; i++) {
		l.drop_next.insert(l.packets_up + i);
	}
	for (int i = 0; i < lost; i++) {
		l.tick();
	}
	CHECK(l.sender.dropped() == 10);
	for (int i = 0; i < 5; i++) {
		l.tick(false); // the link is back: the next sweep catches the server up
	}
	CHECK(l.receiver.skipped(2) == 10);
	CHECK(l.host.got.size() == 1 + lost - 10);
	for (int i = 0; i < 5; i++) {
		l.tick();
	}
	CHECK(l.receiver.skipped(2) == 10);
	CHECK(cmd_id(l.host.got[0].bytes) == 0);
	CHECK(cmd_id(l.host.got[1].bytes) == 11); // 1..10 fell out of the ring
	for (size_t i = 1; i < l.host.got.size(); i++) {
		CHECK(cmd_id(l.host.got[i].bytes) == 10 + i);
	}
	printf("  commands the client gave up on are skipped, not waited for: ok\n");
}

static void test_streams_share_one_order() {
	Link l;
	for (int i = 0; i < 60; i++) {
		l.tick(true, 100 + (i % 3));
	}
	CHECK(l.host.got.size() == 60);
	l.check_in_order();
	for (size_t i = 0; i < l.host.got.size(); i++) {
		CHECK(l.host.got[i].stream == 100 + (i % 3));
	}
	printf("  several streams keep one order and their own ids: ok\n");
}

static void test_seq_wraps() {
	Link l;
	for (int i = 0; i < 70000; i++) {
		l.tick();
	}
	CHECK(l.host.got.size() == 70000);
	l.check_in_order();
	printf("  the 16-bit seq wraps cleanly: ok\n");
}

static void test_repeated_bytes_cost_their_changes() {
	CommandSender s;
	const std::vector<uint8_t> a(32, 9);
	std::vector<uint8_t> b = a;
	b[5] = 1;
	s.push(1, a.data(), a.size(), 10);
	s.push(1, a.data(), a.size(), 10);
	s.push(1, b.data(), b.size(), 10);
	Writer w;
	s.write(10, 0, ChannelAckSource(), w);
	// Header 24 (incl. the send time, zero rates, an empty channel part, one stream and first_seq),
	// then the first command against zeros: len 1, mask 4, 32 bytes. Then an identical one: gap 1,
	// len 1, mask 4. Then one changed byte: gap, len, mask, 1.
	CHECK(w.size() == 24 + 37 + 6 + 7);
	CommandReceiver r;
	RecvHost h;
	CHECK(r.receive(2, w.data(), w.size(), h, 0));
	CHECK(h.got.size() == 3 && h.got[0].bytes == a && h.got[1].bytes == a && h.got[2].bytes == b);
	printf("  a command repeating the last costs its mask and changed bytes: ok\n");
}

static void test_snapshot_ack_rides_along() {
	CommandSender s;
	Writer w;
	s.write(10, 321, ChannelAckSource(), w); // nothing to send but the ack
	CommandReceiver r;
	RecvHost h;
	CHECK(r.receive(2, w.data(), w.size(), h, 0));
	CHECK(h.snapshot_acks == std::vector<uint16_t>{ 321 });
	CHECK(h.got.empty() && r.ack(2) == 0);
	printf("  the snapshot ack rides an uplink packet: ok\n");
}

static void test_malformed_input() {
	CommandSender s;
	std::vector<uint8_t> b = cmd_bytes(1);
	s.push(1, b.data(), b.size(), 10);
	s.push(1, b.data(), b.size(), 10);
	Writer w;
	s.write(10, 5, ChannelAckSource(), w);
	CommandReceiver r;
	RecvHost h;
	for (size_t cut = 0; cut < w.size(); cut++) {
		CHECK(!r.receive(2, w.data(), cut, h, 0)); // every truncation is refused
	}
	CHECK(h.got.empty() && h.snapshot_acks.empty());
	std::vector<uint8_t> bad(w.data(), w.data() + w.size());
	bad[0] = SNAPSHOT_VERSION + 1;
	CHECK(!r.receive(3, bad.data(), bad.size(), h, 0));
	CHECK(!r.receive(3, bad.data(), bad.size(), h, 0));
	CHECK(h.mismatches == 1);
	CHECK(r.receive(2, w.data(), w.size(), h, 0) && h.got.size() == 2);
	printf("  truncated packets and other builds are refused: ok\n");
}

// The round trip is timed from each command's first send, backups and all; an overdue resend
// isn't timed, since its ack can't say which send arrived.
static void test_rtt_is_measured_through_backups() {
	CommandSender s;
	CommandReceiver r;
	RecvHost h;
	uint32_t now = 1000;
	std::vector<std::vector<uint8_t>> in_flight; // packets arriving 5 ticks (80 ms) after they're sent
	for (int i = 0; i < 100; i++) {
		now += 16;
		std::vector<uint8_t> b = cmd_bytes(i);
		s.push(1, b.data(), b.size(), now);
		Writer w;
		s.write(now, 0, ChannelAckSource(), w);
		in_flight.emplace_back(w.data(), w.data() + w.size());
		if (in_flight.size() > 5) {
			CHECK(r.receive(2, in_flight.front().data(), in_flight.front().size(), h, 0));
			in_flight.erase(in_flight.begin());
			s.on_ack(r.ack(2)); // the ack comes straight back: an 80 ms round trip
			s.on_channel_ack(r.channel_ack(2).at(now), now);
		}
	}
	CHECK(s.rtt_ms() == 80);
	printf("  the round trip is timed through the backups: ok\n");
}

// A round trip longer than the first overdue guess resends every command before its ack arrives,
// so no ack can be timed the usual way. The streak of them still measures it, and the resends stop.
static void test_rtt_is_learned_past_the_first_overdue() {
	CommandSender s;
	CommandReceiver r;
	RecvHost h;
	uint32_t now = 1000;
	std::vector<std::vector<uint8_t>> in_flight; // packets arriving 10 ticks (160 ms) after they're sent
	int late_cmds = 0;
	for (int i = 0; i < 200; i++) {
		now += 16;
		std::vector<uint8_t> b = cmd_bytes(i);
		s.push(1, b.data(), b.size(), now);
		Writer w;
		s.write(now, 0, ChannelAckSource(), w);
		if (i >= 150) {
			late_cmds += w.data()[21]; // cmd_ct, after the send time, zero rates, an empty channel part and one stream id
		}
		in_flight.emplace_back(w.data(), w.data() + w.size());
		if (in_flight.size() > 10) {
			CHECK(r.receive(2, in_flight.front().data(), in_flight.front().size(), h, 0));
			in_flight.erase(in_flight.begin());
			s.on_ack(r.ack(2));
			s.on_channel_ack(r.channel_ack(2).at(now), now);
		}
	}
	CHECK(s.rtt_ms() == 160);
	CHECK(late_cmds == 50 * (1 + COMMAND_BACKUP_DEFAULT)); // each new command and its backups, no resends
	CHECK(h.got.size() == 190);
	printf("  a round trip past the first overdue guess is still learned: ok\n");
}

// A round trip longer than the ring: every command falls out before its ack comes back, so no
// command ack can time it. Packet acks still do, and the commands then go once each with their
// backups rather than the whole ring in every packet (which kept a slow link full for good).
static void test_rtt_longer_than_the_ring_is_learned() {
	CommandSender s;
	CommandReceiver r;
	RecvHost h;
	uint32_t now = 1000;
	const size_t trip = COMMAND_RING + 16; // packets arriving 80 ticks (1280 ms) after they're sent
	std::vector<std::vector<uint8_t>> in_flight;
	int late_cmds = 0;
	for (int i = 0; i < 400; i++) {
		now += 16;
		std::vector<uint8_t> b = cmd_bytes(i);
		s.push(1, b.data(), b.size(), now);
		Writer w;
		s.write(now, 0, ChannelAckSource(), w);
		if (i >= 350) {
			late_cmds += w.data()[21]; // cmd_ct, after the send time, zero rates, an empty channel part and one stream id
		}
		in_flight.emplace_back(w.data(), w.data() + w.size());
		if (in_flight.size() > trip) {
			CHECK(r.receive(2, in_flight.front().data(), in_flight.front().size(), h, 0));
			in_flight.erase(in_flight.begin());
			s.on_ack(r.ack(2));
			s.on_channel_ack(r.channel_ack(2).at(now), now);
		}
	}
	CHECK(s.rtt_ms() == trip * 16);
	CHECK(late_cmds == 50 * (1 + COMMAND_BACKUP_DEFAULT));
	CHECK(r.skipped(2) == 0);
	printf("  a round trip longer than the ring is learned, and stops the resends: ok\n");
}

static void test_interval_paces_packets() {
	Link l;
	l.sender.set_interval_ms(50);
	for (int i = 0; i < 60; i++) {
		l.tick();
	}
	CHECK(l.packets_up == 15); // 16 ms ticks: the first one at or past 50 ms is every fourth
	for (int i = 0; i < 10; i++) {
		l.tick(false);
	}
	CHECK(l.host.got.size() == 60);
	l.check_in_order();
	printf("  an interval paces packets, each carrying what was made since: ok\n");
}

// A command the server's channel ack proves lost (a later packet arrived, its ack hasn't) goes again in
// the next packet, not when its overdue time runs out; the server says so though its ack is stuck.
static void test_proven_lost_command_goes_at_once() {
	CommandSender s;
	CommandReceiver r;
	RecvHost h;
	s.set_backup(0);
	uint32_t now = 1000;
	auto send = [&](bool deliver) {
		Writer w;
		s.write(now, 0, ChannelAckSource(), w);
		if (deliver) {
			CHECK(r.receive(2, w.data(), w.size(), h, now));
		}
	};
	std::vector<uint8_t> a = cmd_bytes(0), b = cmd_bytes(1);
	s.push(1, a.data(), a.size(), now);
	send(false); // command 0 is lost
	now += 16;
	s.push(1, b.data(), b.size(), now);
	send(true); // command 1 arrives and is held behind it
	CHECK(r.ack(2) == 0 && h.got.empty());
	CHECK(r.take_news(2)); // the server has something to say though its ack hasn't moved
	CHECK(!r.take_news(2));
	ChannelAck ack = r.channel_ack(2).at(now);
	now += 16;
	s.on_channel_ack(ack, now); // it received packet 2, not packet 1
	CHECK(s.due(now));     // long before command 0 is overdue
	send(true);
	CHECK(h.got.size() == 2 && cmd_id(h.got[0].bytes) == 0 && cmd_id(h.got[1].bytes) == 1);
	printf("  a command the server's ack proves lost goes again at once: ok\n");
}

// A client resending commands the server already delivered never heard the ack: that earns a snapshot.
static void test_a_repeat_earns_the_ack_again() {
	CommandSender s;
	CommandReceiver r;
	RecvHost h;
	std::vector<uint8_t> a = cmd_bytes(0);
	s.push(1, a.data(), a.size(), 10);
	Writer w;
	s.write(10, 0, ChannelAckSource(), w);
	CHECK(r.receive(2, w.data(), w.size(), h, 10));
	CHECK(!r.take_news(2)); // delivered, and the ack moved: the ack itself is the news
	CHECK(r.receive(2, w.data(), w.size(), h, 20)); // the same packet again
	CHECK(r.take_news(2));
	CHECK(h.got.size() == 1);
	std::vector<uint8_t> b = cmd_bytes(1);
	s.on_ack(r.ack(2));
	s.push(1, b.data(), b.size(), 30);
	Writer w2;
	s.write(30, 0, ChannelAckSource(), w2); // a new command, with the old one riding as a backup
	CHECK(r.receive(2, w2.data(), w2.size(), h, 30));
	CHECK(!r.take_news(2)); // routine backups are no reason to send
	CHECK(h.got.size() == 2);
	printf("  a repeated command earns the ack again: ok\n");
}

// The rate rides every packet, rounded up to 100 B/s; 0 is no limit.
static void test_rate_rides_along() {
	CommandSender s;
	CommandReceiver r;
	RecvHost h;
	Writer w;
	s.write(10, 0, ChannelAckSource(), w);
	CHECK(r.receive(2, w.data(), w.size(), h, 10));
	CHECK(r.rate(2) == 0);
	s.set_rate(2750);
	Writer w2;
	s.write(26, 0, ChannelAckSource(), w2);
	CHECK(r.receive(2, w2.data(), w2.size(), h, 26));
	CHECK(r.rate(2) == 2800);
	CHECK(r.update_rate(2) == 0);
	s.set_update_rate(20);
	Writer w3;
	s.write(42, 0, ChannelAckSource(), w3);
	CHECK(r.receive(2, w3.data(), w3.size(), h, 42));
	CHECK(r.update_rate(2) == 20);
	printf("  the rate and update rate ride every packet: ok\n");
}

// The server meters the uplink from each packet's 16-bit send time: a steady trip reads as no lag, a
// queue building on the way as its extra wait, across the stamp's wrap.
static void test_uplink_lag_is_metered_across_the_wrap() {
	CommandSender s;
	CommandReceiver r;
	RecvHost h;
	uint32_t now = 65536 - 500; // the client's clock wraps 16 bits half a second in
	for (int i = 0; i < 120; i++, now += 16) {
		Writer w;
		s.write(now, 0, ChannelAckSource(), w);
		CHECK(r.receive(2, w.data(), w.size(), h, now + 1000040)); // another clock, a 40 ms trip
	}
	CHECK(r.uplink_lag_ms(2) <= 1);
	CHECK(r.uplink_pps(2) >= 60 && r.uplink_pps(2) <= 64);
	for (int k = 0; k < 60; k++, now += 16) {
		Writer w;
		s.write(now, 0, ChannelAckSource(), w);
		CHECK(r.receive(2, w.data(), w.size(), h, now + 1000040 + 5 * k));
	}
	CHECK(r.uplink_lag_ms(2) >= 290 && r.uplink_lag_ms(2) <= 300);
	printf("  the uplink's queue is metered from send times, across the wrap (%u ms): ok\n", r.uplink_lag_ms(2));
}

int main() {
	printf("uplink:\n");
	test_lossless_delivers_each_once_in_order();
	test_loss_both_ways_still_delivers_everything();
	test_backups_cover_a_lost_packet_at_once();
	test_burst_past_the_backups_resends_when_overdue();
	test_given_up_commands_are_skipped();
	test_streams_share_one_order();
	test_seq_wraps();
	test_repeated_bytes_cost_their_changes();
	test_snapshot_ack_rides_along();
	test_malformed_input();
	test_rtt_is_measured_through_backups();
	test_rtt_is_learned_past_the_first_overdue();
	test_rtt_longer_than_the_ring_is_learned();
	test_interval_paces_packets();
	test_proven_lost_command_goes_at_once();
	test_a_repeat_earns_the_ack_again();
	test_rate_rides_along();
	test_uplink_lag_is_metered_across_the_wrap();
	printf("uplink: %d checks passed\n", checks);
	return 0;
}
