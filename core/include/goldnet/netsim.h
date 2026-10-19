#pragma once
//
// Network-condition simulation: loss, latency, periodic latency spikes, and a bandwidth cap. A
// host runs it at its send funnels, where latency is the full per-leg delay; the Godot binding
// also runs a second one on what a client receives, so a client alone can simulate its downlink.
//
// P is whatever the host needs to replay a deferred send. The sim decides; the host sends:
//
//   if (sim.loss_percent() > 0 && droppable && sim.loss_roll()) return;  // simulated drop
//   int d = sim.schedule(now_ms, link, bytes, droppable);
//   if (d < 0) return;                                                 // link queue full
//   if (d == 0) send_now(...); else sim.enqueue(now_ms, link, d, payload);
//   ...
//   sim.pump(now_ms, [&](P &payload) { send_now(payload); });          // every poll
//
// The bandwidth cap models a slow link (a modem, a BLE connection): sending over capacity doesn't
// lose packets, it queues them, so delay grows until the link's buffer fills and only then do
// packets drop. Each link is independent — on a server, one per client, since each client's
// downlink is its own; on a client, its one uplink.
//
// Every random draw goes through one generator, so a non-zero seed replays a whole session —
// loss rolls and latency draws alike — which is what a regression test needs. Seed 0 is
// nondeterministic: a different pattern every run.
//
#include "goldnet/codec.h"

#include <cmath>
#include <cstdint>
#include <deque>
#include <random>
#include <unordered_map>

namespace goldnet {

template <class P>
class NetSim {
public:
	// Defaults live here and nowhere else, so a host's "reset to defaults" and a fresh instance
	// agree by construction.
	static constexpr int LATENCY_MIN_DEFAULT_MS = 0;
	static constexpr int LATENCY_MAX_DEFAULT_MS = 0;
	static constexpr int SPIKE_DEFAULT_MS = 0;
	static constexpr float SPIKE_INTERVAL_DEFAULT_S = 10.0f;
	static constexpr float SPIKE_DURATION_DEFAULT_S = 0.2f;
	static constexpr int LOSS_DEFAULT_PERCENT = 0;
	static constexpr int SEED_DEFAULT = 0;
	static constexpr int BANDWIDTH_DEFAULT_BPS = 0; // 0 = unlimited
	static constexpr int QUEUE_DEFAULT_MS = 1000;
	// What one send costs on the wire beyond its payload: IP + UDP headers (28) and the transport's
	// own framing. A host that knows better passes its own.
	static constexpr int PACKET_OVERHEAD_DEFAULT = 40;

	NetSim() { rng_ = fresh_state(0); }

	// --- config: every setter clamps, and is idempotent, so a read-modify-write of one knob
	// disturbs nothing else ---

	void set_loss_percent(int p_pct) { loss_ = p_pct < 0 ? 0 : (p_pct > 100 ? 100 : p_pct); }
	int loss_percent() const { return loss_; }

	// Re-writing the same seed must NOT restart the sequence: a read-modify-write of an unrelated
	// knob on a seeded run writes the seed back unchanged, and rewinding there would destroy the
	// reproducibility the seed exists to provide. Setting a new one restarts it, so seed before
	// the session starts rather than mid-run.
	void set_seed(int p_seed) {
		const uint32_t next = (uint32_t)(p_seed < 0 ? 0 : p_seed);
		if (next == seed_) {
			return;
		}
		seed_ = next;
		rng_ = fresh_state(seed_);
	}
	int seed() const { return (int)seed_; }

	void set_latency_min_ms(int p_ms) { latency_min_ = p_ms < 0 ? 0 : p_ms; }
	int latency_min_ms() const { return latency_min_; }
	void set_latency_max_ms(int p_ms) { latency_max_ = p_ms < 0 ? 0 : p_ms; }
	int latency_max_ms() const { return latency_max_; }

	// One-way latency while a spike is active, modelling a power-save stall that releases a burst
	// of packets at once. 0 disables spikes and clears any in flight.
	void set_spike_ms(int p_ms) {
		const int next = p_ms < 0 ? 0 : p_ms;
		if (next == spike_ms_) {
			return;
		}
		spike_ms_ = next;
		if (spike_ms_ == 0) {
			spike_active_ = false;
			spike_timer_ = 0.0f;
			spike_elapsed_ = 0.0f;
		}
	}
	int spike_ms() const { return spike_ms_; }
	void set_spike_interval_s(float p_s) { spike_interval_s_ = p_s < 0.0f ? 0.0f : p_s; } // between spikes
	float spike_interval_s() const { return spike_interval_s_; }
	void set_spike_duration_s(float p_s) { spike_duration_s_ = p_s < 0.0f ? 0.0f : p_s; } // each spike's length
	float spike_duration_s() const { return spike_duration_s_; }

	// Link capacity in bytes per second; 0 = unlimited.
	void set_bandwidth_bps(int p_bps) { bandwidth_bps_ = p_bps < 0 ? 0 : p_bps; }
	int bandwidth_bps() const { return bandwidth_bps_; }
	// How much backlog, in time, a link buffers before it drops droppable sends.
	void set_queue_ms(int p_ms) { queue_ms_ = p_ms < 0 ? 0 : p_ms; }
	int queue_ms() const { return queue_ms_; }
	void set_packet_overhead(int p_bytes) { packet_overhead_ = p_bytes < 0 ? 0 : p_bytes; }
	int packet_overhead() const { return packet_overhead_; }

	// --- decisions ---

	// One loss roll: true = drop this packet. Free (no RNG draw) while loss is off.
	bool loss_roll() {
		if (loss_ <= 0) {
			return false;
		}
		return (int)(rand() % 100) < loss_;
	}

	// This send's delay: the full spike latency while a spike is active, otherwise a draw from
	// [latency_min_ms, latency_max_ms]. 0 = send immediately.
	int delay_ms() {
		if (spike_active_ && spike_ms_ > 0) {
			return spike_ms_ > 1 ? spike_ms_ : 1;
		}
		if (latency_max_ <= 0) {
			return 0;
		}
		return rand_range(latency_min_, latency_max_);
	}

	bool spike_active() const { return spike_active_; }

	// The delay before a send of p_bytes on link p_link should fire: its wait behind earlier sends
	// on that link, its own transmit time, then the latency draw. -1 = drop it: the link's backlog
	// is past queue_ms and the send is droppable. A send that can't be dropped (reliable) always
	// queues, since the real transport would keep retransmitting it. Without a bandwidth cap this
	// is just delay_ms().
	int schedule(uint64_t p_now_ms, int32_t p_link, size_t p_bytes, bool p_droppable) {
		if (bandwidth_bps_ <= 0) {
			return delay_ms();
		}
		double &free_at = link_free_at_[p_link];
		const double now = (double)p_now_ms;
		const double start = free_at > now ? free_at : now;
		if (p_droppable && start - now > (double)queue_ms_) {
			return -1;
		}
		free_at = start + (double)(p_bytes + (size_t)packet_overhead_) * 1000.0 / (double)bandwidth_bps_;
		const int wait = (int)std::ceil(free_at - now);
		return wait + delay_ms();
	}

	// How far behind link p_link is, in ms: what a send made now would wait before transmitting.
	double backlog_ms(uint64_t p_now_ms, int32_t p_link) const {
		auto it = link_free_at_.find(p_link);
		if (it == link_free_at_.end() || it->second <= (double)p_now_ms) {
			return 0.0;
		}
		return it->second - (double)p_now_ms;
	}

	// A link that's gone (a disconnected peer): drop its backlog and anything still queued on it.
	void forget_link(int32_t p_link) {
		link_free_at_.erase(p_link);
		queues_.erase(p_link);
	}

	// Hold a send on link p_link for p_delay_ms. Each link is a pipe, not per-packet jitter:
	// pump() only releases the front of a link's queue, so a send never fires before one queued
	// earlier on the same link — a short draw behind a long one waits for it. Links don't wait on
	// each other.
	void enqueue(uint64_t p_now_ms, int32_t p_link, int p_delay_ms, P p_payload) {
		queues_[p_link].push_back(Pending{ p_now_ms + (uint64_t)(p_delay_ms > 0 ? p_delay_ms : 0), std::move(p_payload) });
	}

	size_t pending() const {
		size_t n = 0;
		for (const auto &kv : queues_) {
			n += kv.second.size();
		}
		return n;
	}

	// Call every poll: advances the spike machine by the real elapsed time, then hands every send
	// whose time has come to p_fire, in queue order per link.
	template <class F>
	void pump(uint64_t p_now_ms, F &&p_fire) {
		const float delta = last_pump_ms_ != 0 ? (float)(p_now_ms - last_pump_ms_) / 1000.0f : 0.0f;
		last_pump_ms_ = p_now_ms;
		update_spike(delta);
		for (auto &kv : queues_) {
			std::deque<Pending> &q = kv.second;
			while (!q.empty() && q.front().fire_at_ms <= p_now_ms) {
				Pending ps = std::move(q.front());
				q.pop_front();
				p_fire(ps.payload);
			}
		}
	}

private:
	struct Pending {
		uint64_t fire_at_ms;
		P payload;
	};

	static uint32_t fresh_state(uint32_t p_seed) {
		if (p_seed != 0) {
			return sim_rng_seed(p_seed);
		}
		std::random_device rd;
		return sim_rng_seed(rd()); // sim_rng_seed maps a 0 draw to a legal state
	}

	uint32_t rand() { return xorshift32(rng_); }

	int rand_range(int p_lo, int p_hi) {
		if (p_hi <= p_lo) {
			return p_lo;
		}
		return p_lo + (int)(rand() % (uint32_t)(p_hi - p_lo + 1));
	}

	void update_spike(float p_delta) {
		if (spike_ms_ <= 0) {
			return;
		}
		if (spike_active_) {
			spike_elapsed_ += p_delta;
			if (spike_elapsed_ >= spike_duration_s_) {
				spike_active_ = false;
				spike_elapsed_ = 0.0f;
				spike_timer_ = 0.0f;
			}
		} else {
			spike_timer_ += p_delta;
			if (spike_timer_ >= spike_interval_s_) {
				spike_active_ = true;
				spike_elapsed_ = 0.0f;
				spike_timer_ = 0.0f;
			}
		}
	}

	int loss_ = LOSS_DEFAULT_PERCENT;
	uint32_t seed_ = SEED_DEFAULT;
	uint32_t rng_ = 0;
	int latency_min_ = LATENCY_MIN_DEFAULT_MS;
	int latency_max_ = LATENCY_MAX_DEFAULT_MS;
	int spike_ms_ = SPIKE_DEFAULT_MS;
	float spike_interval_s_ = SPIKE_INTERVAL_DEFAULT_S;
	float spike_duration_s_ = SPIKE_DURATION_DEFAULT_S;
	bool spike_active_ = false;
	float spike_timer_ = 0.0f;   // counts up to spike_interval_s while idle
	float spike_elapsed_ = 0.0f; // counts up to spike_duration_s while active
	uint64_t last_pump_ms_ = 0;
	std::unordered_map<int32_t, std::deque<Pending>> queues_;
	int bandwidth_bps_ = BANDWIDTH_DEFAULT_BPS;
	int queue_ms_ = QUEUE_DEFAULT_MS;
	int packet_overhead_ = PACKET_OVERHEAD_DEFAULT;
	std::unordered_map<int32_t, double> link_free_at_; // when each link finishes what it has queued
};

} // namespace goldnet
