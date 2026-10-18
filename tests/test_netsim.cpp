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
	s.enqueue(1000, 100, 1); // due 1100
	s.enqueue(1000, 10, 2);  // due 1010, but may not overtake 1
	s.enqueue(1050, 80, 3);  // due 1130
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
	printf("%d checks passed\n", checks);
	return 0;
}
