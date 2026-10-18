// The send-side network-condition sim: config clamping, seeded reproducibility, the latency and
// loss draws, the in-order delay pipe, and the spike state machine — all with explicit clocks, so
// nothing here depends on wall time.

#include "test_support.h"

#include "goldnet/netsim.h"

#include <string>
#include <vector>

using namespace goldnet;

typedef NetSim<int> Sim;

static std::vector<int> draws(Sim &s, int n) {
	std::vector<int> out;
	for (int i = 0; i < n; i++) {
		out.push_back(s.delay_ms());
		out.push_back(s.loss_roll() ? 1 : 0);
	}
	return out;
}

static void test_defaults_are_off() {
	Sim s;
	CHECK(s.delay_ms() == 0);
	CHECK(!s.loss_roll());
	CHECK(!s.spike_active());
	printf("  defaults: no delay, no loss, no spike: ok\n");
}

static void test_clamps() {
	Sim s;
	s.set_loss_percent(-5);
	CHECK(s.loss_percent() == 0);
	s.set_loss_percent(150);
	CHECK(s.loss_percent() == 100);
	s.set_latency_min_ms(-1);
	s.set_latency_max_ms(-1);
	CHECK(s.latency_min_ms() == 0 && s.latency_max_ms() == 0);
	s.set_spike_ms(-3);
	CHECK(s.spike_ms() == 0);
	s.set_spike_interval_s(-1.0f);
	s.set_spike_duration_s(-1.0f);
	CHECK(s.spike_interval_s() == 0.0f && s.spike_duration_s() == 0.0f);
	s.set_seed(-7);
	CHECK(s.seed() == 0);
	printf("  setters clamp: ok\n");
}

static void test_full_loss_drops_everything() {
	Sim s;
	s.set_loss_percent(100);
	for (int i = 0; i < 100; i++) {
		CHECK(s.loss_roll());
	}
	printf("  100%% loss drops every packet: ok\n");
}

static void test_seed_reproduces_session() {
	Sim a, b, c;
	for (Sim *s : { &a, &b, &c }) {
		s->set_loss_percent(30);
		s->set_latency_min_ms(20);
		s->set_latency_max_ms(80);
	}
	a.set_seed(42);
	b.set_seed(42);
	c.set_seed(43);
	std::vector<int> da = draws(a, 200);
	CHECK(da == draws(b, 200));
	CHECK(da != draws(c, 200));
	printf("  one seed replays loss and latency together: ok\n");
}

static void test_reseeding_same_value_keeps_sequence() {
	Sim a, b;
	for (Sim *s : { &a, &b }) {
		s->set_latency_max_ms(100);
		s->set_seed(9);
	}
	draws(a, 10);
	draws(b, 10);
	b.set_seed(9); // a read-modify-write of another knob writes the seed back unchanged
	CHECK(draws(a, 10) == draws(b, 10));
	b.set_seed(10);
	b.set_seed(9); // a genuine change restarts it
	Sim fresh;
	fresh.set_latency_max_ms(100);
	fresh.set_seed(9);
	CHECK(draws(b, 10) == draws(fresh, 10));
	printf("  re-writing the same seed doesn't rewind; a new one restarts: ok\n");
}

static void test_latency_range() {
	Sim s;
	s.set_seed(1);
	s.set_latency_min_ms(30);
	s.set_latency_max_ms(60);
	bool saw_min = false, saw_max = false;
	for (int i = 0; i < 2000; i++) {
		int d = s.delay_ms();
		CHECK(d >= 30 && d <= 60);
		saw_min |= d == 30;
		saw_max |= d == 60;
	}
	CHECK(saw_min && saw_max); // both ends inclusive
	s.set_latency_min_ms(45);
	s.set_latency_max_ms(45);
	CHECK(s.delay_ms() == 45);
	s.set_latency_min_ms(70); // min above max: the min wins
	CHECK(s.delay_ms() == 70);
	printf("  latency draws span [min, max] inclusive: ok\n");
}

static void test_loss_rate() {
	Sim s;
	s.set_seed(5);
	s.set_loss_percent(30);
	int lost = 0;
	for (int i = 0; i < 10000; i++) {
		lost += s.loss_roll() ? 1 : 0;
	}
	CHECK(lost > 2700 && lost < 3300);
	s.set_loss_percent(1); // tight enough to tell 1% from 2%
	lost = 0;
	for (int i = 0; i < 100000; i++) {
		lost += s.loss_roll() ? 1 : 0;
	}
	CHECK(lost > 800 && lost < 1200);
	printf("  loss rate tracks the percent: ok\n");
}

static void test_pipe_keeps_order() {
	Sim s;
	std::vector<int> fired;
	auto fire = [&](int &p) { fired.push_back(p); };
	s.enqueue(1000, 0, 100, 1); // due 1100
	s.enqueue(1000, 0, 10, 2);  // due 1010, but may not overtake 1
	s.enqueue(1050, 0, 80, 3);  // due 1130
	s.pump(1050, fire);
	CHECK(fired.empty());
	s.pump(1100, fire);
	CHECK(fired == (std::vector<int>{ 1, 2 }));
	s.pump(1129, fire);
	CHECK(fired.size() == 2);
	s.pump(1130, fire);
	CHECK(fired == (std::vector<int>{ 1, 2, 3 }));
	CHECK(s.pending() == 0);
	printf("  the delay pipe never reorders: ok\n");
}

static void test_spike_cycle() {
	Sim s;
	s.set_spike_ms(300);
	s.set_spike_interval_s(1.0f);
	s.set_spike_duration_s(0.2f);
	auto none = [](int &) {};
	s.pump(1000, none);
	s.pump(1500, none);
	CHECK(!s.spike_active());
	CHECK(s.delay_ms() == 0);
	s.pump(2000, none);
	CHECK(s.spike_active());
	CHECK(s.delay_ms() == 300); // a spike overrides the (zero) latency range
	s.pump(2100, none);
	CHECK(s.spike_active());
	s.pump(2200, none);
	CHECK(!s.spike_active());
	s.pump(3200, none);
	CHECK(s.spike_active());
	s.set_spike_ms(0); // disabling clears a spike in flight
	CHECK(!s.spike_active());
	CHECK(s.delay_ms() == 0);
	printf("  spikes come on every interval, last their duration, and clear when disabled: ok\n");
}

// --- the bandwidth cap: a slow link queues, then drops ---

static Sim link_sim(int bps, int queue_ms) {
	Sim s;
	s.set_bandwidth_bps(bps);
	s.set_queue_ms(queue_ms);
	s.set_packet_overhead(0);
	return s;
}

static void test_uncapped_link_is_just_latency() {
	Sim s;
	s.set_latency_min_ms(30);
	s.set_latency_max_ms(30);
	CHECK(s.schedule(1000, 2, 5000, true) == 30);
	CHECK(s.schedule(1000, 2, 5000, true) == 30); // no backlog builds without a cap
	printf("  no cap: just the latency draw: ok\n");
}

static void test_capped_link_serializes() {
	Sim s = link_sim(1000, 100000); // 1 byte per ms
	for (int i = 1; i <= 10; i++) {
		CHECK(s.schedule(1000, 2, 100, true) == 100 * i); // each waits for the ones ahead of it
	}
	CHECK(s.backlog_ms(1000, 2) == 1000.0);
	CHECK(s.backlog_ms(1600, 2) == 400.0); // drains in real time
	CHECK(s.backlog_ms(2500, 2) == 0.0);
	CHECK(s.schedule(2500, 2, 100, true) == 100); // an idle link starts from now
	printf("  a capped link serializes sends and drains in real time: ok\n");
}

static void test_overhead_and_latency_add() {
	Sim s = link_sim(1000, 100000);
	s.set_packet_overhead(40);
	s.set_latency_min_ms(50);
	s.set_latency_max_ms(50);
	CHECK(s.schedule(0, 2, 60, true) == 100 + 50); // (60 + 40) bytes, then the leg's latency
	printf("  per-send overhead and latency stack on transmit time: ok\n");
}

static void test_links_are_independent() {
	Sim s = link_sim(1000, 100000);
	s.schedule(0, 2, 5000, true);
	CHECK(s.schedule(0, 3, 100, true) == 100); // peer 3's link isn't behind peer 2's
	printf("  one client's slow link doesn't delay another's: ok\n");
}

static void test_full_buffer_drops_only_droppable() {
	Sim s = link_sim(1000, 250);
	int dropped = 0, reliable_queued = 0;
	for (int i = 0; i < 10; i++) {
		if (s.schedule(0, 2, 100, true) < 0) {
			dropped++;
		}
	}
	CHECK(dropped == 7); // 0, 100, 200 ms of backlog are within the buffer; past 250 drops
	for (int i = 0; i < 3; i++) {
		if (s.schedule(0, 2, 100, false) > 0) {
			reliable_queued++;
		}
	}
	CHECK(reliable_queued == 3); // reliable sends always queue (the transport would retransmit)
	CHECK(s.backlog_ms(0, 2) == 600.0);
	printf("  a full buffer drops unreliable sends and queues reliable ones: ok\n");
}

static void test_link_queues_release_independently() {
	Sim s;
	std::vector<int> fired;
	auto fire = [&](int &p) { fired.push_back(p); };
	s.enqueue(0, 2, 500, 1); // a backed-up link
	s.enqueue(0, 3, 20, 2);  // a clear one, queued after it
	s.pump(20, fire);
	CHECK(fired == std::vector<int>{ 2 });
	s.forget_link(2); // its peer left: what it had queued goes with it
	s.pump(600, fire);
	CHECK(fired == std::vector<int>{ 2 });
	CHECK(s.pending() == 0);
	printf("  links release independently; a forgotten link's queue is dropped: ok\n");
}

int main() {
	printf("netsim:\n");
	test_defaults_are_off();
	test_clamps();
	test_full_loss_drops_everything();
	test_seed_reproduces_session();
	test_reseeding_same_value_keeps_sequence();
	test_latency_range();
	test_loss_rate();
	test_pipe_keeps_order();
	test_spike_cycle();
	test_uncapped_link_is_just_latency();
	test_capped_link_serializes();
	test_overhead_and_latency_add();
	test_links_are_independent();
	test_full_buffer_drops_only_droppable();
	test_link_queues_release_independently();
	printf("%d checks passed\n", checks);
	return 0;
}
