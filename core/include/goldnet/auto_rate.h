#pragma once
//
// Automatic rate and command rate: what a client asks the server to send it (GoldSrc `rate`) and how
// many uplink packets it sends a second (GoldSrc `cl_cmdrate`), each chosen from how far behind its
// best its stream arrives. A queue on a slow link shows as that lag before it shows as loss.
//
// The downlink is measured on the client, from each snapshot's server send time. The uplink is
// measured on the server, from each uplink packet's client send time, and reported back in the
// snapshot header. Both sides react the way TCP finds a link's capacity: a sharp cut while it queues,
// a slow climb while it doesn't.
//
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace goldnet {

// How far behind its best a stream is arriving, in ms: time spent queued on a slow link. Arrival
// time minus send time is the two clocks' offset plus the trip; its floor over the last WINDOW_MS
// or two is the uncongested trip, and anything above that waited in a queue. Looking back only that
// far lets the floor follow drift between the clocks. A pause in the stream starts the floor over
// rather than reading as seconds of lag.
class QueueMeter {
public:
	static constexpr uint32_t WINDOW_MS = 4000;
	static constexpr uint32_t PAUSE_MS = 500;

	// Something sent at p_sent_ms (on the sender's clock) arrived at p_now. Returns the current lag.
	uint32_t note(uint32_t p_now, int64_t p_sent_ms) {
		if (!any_ || p_now - last_arrival_ > PAUSE_MS) {
			cur_min_ = INF;
			prev_min_ = INF;
			window_end_ = p_now + WINDOW_MS;
		}
		any_ = true;
		last_arrival_ = p_now;
		if ((int32_t)(p_now - window_end_) >= 0) {
			prev_min_ = cur_min_;
			cur_min_ = INF;
			window_end_ = p_now + WINDOW_MS;
		}
		const double offset = (double)p_now - (double)p_sent_ms;
		cur_min_ = std::min(cur_min_, offset);
		const double lag = std::round(offset - std::min(cur_min_, prev_min_));
		return lag > 0.0 ? (uint32_t)lag : 0;
	}

	void reset() { *this = QueueMeter(); }

private:
	static constexpr double INF = std::numeric_limits<double>::infinity();
	double cur_min_ = INF;
	double prev_min_ = INF;
	uint32_t window_end_ = 0;
	uint32_t last_arrival_ = 0;
	bool any_ = false;
};

// How many packets a second a stream is arriving in. While a slow link is queueing it delivers at
// its capacity, so this is how much the link carries. A pause starts the count over.
class ArrivalMeter {
public:
	static constexpr uint32_t WINDOW_MS = 500;
	static constexpr uint32_t PAUSE_MS = 500;

	// A packet arrived. Returns the rate over the last full window; 0 until one closes.
	uint32_t note(uint32_t p_now) {
		const bool paused = !any_ || p_now - last_ms_ > PAUSE_MS;
		any_ = true;
		last_ms_ = p_now;
		if (paused) {
			window_start_ = p_now;
			count_ = 0;
			return pps_;
		}
		count_++;
		if (p_now - window_start_ >= WINDOW_MS) {
			pps_ = (uint32_t)std::lround(count_ * 1000.0 / (double)(p_now - window_start_));
			window_start_ = p_now;
			count_ = 0;
		}
		return pps_;
	}

	uint32_t pps() const { return pps_; }

private:
	uint32_t pps_ = 0;
	uint32_t window_start_ = 0;
	uint32_t count_ = 0;
	uint32_t last_ms_ = 0;
	bool any_ = false;
};

// The client's `rate`, from its downlink lag. No limit until the downlink queues. Then the first limit
// is just under what is actually arriving, since a full link's delivery rate is its capacity; from
// there, a sharp cut while it still queues and a slow climb while it doesn't, up to MAX_RATE.
//
// Lag alone can't see a queue that was full from the very first snapshot (the join's burst can do
// that): with no uncongested moment to measure from, the full queue IS the floor. A full buffer drops
// what it can't hold, though, so while there's no limit, snapshots going missing counts as queueing
// too. Once limited, lag is the signal: missing snapshots may be the limit's own chokes.
class RateAuto {
public:
	// Wider than the uplink's band: a snapshot's trip carries the link's whole jitter, and that alone
	// can hold the smoothed lag at a few tens of ms with no queue at all.
	static constexpr double HIGH_MS = 80.0;     // smoothed lag over this: the downlink is queueing
	static constexpr double LOW_MS = 40.0;      // under this for CALM_MS: room for more
	static constexpr uint32_t WARMUP_MS = 5000; // the join's burst queues on any link; judge only after it
	static constexpr uint32_t CALM_MS = 3000;
	static constexpr uint32_t HOLD_MS = 2000;   // after a cut, let the queue drain before judging again
	static constexpr double START = 0.9;        // first limit, as a fraction of what's arriving
	static constexpr double CUT = 0.8;
	static constexpr double CLIMB = 1.1;
	static constexpr int MIN_RATE = 1000;
	static constexpr int MAX_RATE = 1000000;
	static constexpr double LOSS_HIGH = 0.25;   // share of snapshots lost (while unlimited) that means overflow
	static constexpr double SMOOTHING = 0.1;

	// One report of the downlink lag, with what is arriving (bytes/sec on the wire) and the share of
	// snapshots lost over the last sample. Returns the rate in bytes/sec; 0 = no limit.
	int note(uint32_t p_now, double p_lag_ms, double p_delivered_bps, double p_loss) {
		smoothed_ += (p_lag_ms - smoothed_) * SMOOTHING;
		if (!started_) {
			started_ = true;
			started_ms_ = p_now;
		}
		if (p_now - started_ms_ < WARMUP_MS) {
			calm_since_ = p_now;
			return rate_;
		}
		if ((int32_t)(p_now - hold_until_) < 0) {
			return rate_;
		}
		if (smoothed_ > HIGH_MS || (rate_ == 0 && p_loss > LOSS_HIGH)) {
			const int cut = rate_ == 0 ? (int)(p_delivered_bps * START) : (int)(rate_ * CUT);
			rate_ = std::max(MIN_RATE, cut);
			hold_until_ = p_now + HOLD_MS;
			calm_since_ = p_now;
		} else if (smoothed_ < LOW_MS) {
			if (rate_ > 0 && p_now - calm_since_ >= CALM_MS) {
				rate_ = std::min(MAX_RATE, (int)(rate_ * CLIMB));
				calm_since_ = p_now;
			}
		} else {
			calm_since_ = p_now;
		}
		return rate_;
	}

	int rate() const { return rate_; }

private:
	int rate_ = 0;
	double smoothed_ = 0.0;
	uint32_t hold_until_ = 0;
	uint32_t calm_since_ = 0;
	uint32_t started_ms_ = 0;
	bool started_ = false;
};

// The client's command rate, from the server's report of its uplink lag. Fewer packets cost a little
// input latency but save the per-packet overhead that fills a slow uplink. It starts low and climbs
// quickly until the first sign of queueing (TCP's slow start): starting at the ceiling floods a
// modem's uplink before any report can say so. A fast link never queues, so it reaches the ceiling
// within seconds.
//
// A queueing link delivers at its capacity, so the server's count of our packets arriving says where
// the link is. A cut goes under that by enough to drain the backlog within the hold, rather than a
// fixed fraction of a rate that may be far above the link.
class CmdRateAuto {
public:
	static constexpr double HIGH_MS = 60.0;        // smoothed lag over this: the uplink is queueing
	static constexpr double LOW_MS = 25.0;         // under this for CALM_MS: room to send more
	static constexpr uint32_t CALM_MS = 3000;
	static constexpr uint32_t HOLD_MS = 2000;      // after a cut, let the queue drain before judging again
	static constexpr double CUT = 0.7;             // when the arrival rate isn't known yet
	static constexpr double DRAIN_MIN = 0.5;       // a cut's share of the arrival rate: never under this...
	static constexpr double DRAIN_MAX = 0.9;       // ...nor over this
	static constexpr double STEP = 5.0;
	static constexpr double START_RATE = 20.0;     // a modem's uplink takes about this much
	static constexpr uint32_t SLOW_START_MS = 1000; // the calm a step takes until the first cut
	static constexpr double MIN_RATE = 10.0;
	static constexpr double SMOOTHING = 0.1;       // per report

	// One report of the uplink lag and of our packets arriving per second (0 = not known yet);
	// p_max_rate is the ceiling (the rate commands are made). Returns the rate in packets/sec.
	double note(uint32_t p_now, double p_lag_ms, double p_arriving, double p_max_rate) {
		rate_ = std::min(rate_, p_max_rate);
		smoothed_ += (p_lag_ms - smoothed_) * SMOOTHING;
		if (!calm_set_) {
			calm_set_ = true;
			calm_since_ = p_now;
		}
		if ((int32_t)(p_now - hold_until_) < 0) {
			return rate_;
		}
		if (smoothed_ > HIGH_MS) {
			double cut = rate_ * CUT;
			if (p_arriving > 0.0) {
				// Sending at f of capacity drains (1 - f) ms of backlog per ms.
				cut = std::min(rate_, p_arriving) * std::clamp(1.0 - p_lag_ms / HOLD_MS, DRAIN_MIN, DRAIN_MAX);
			}
			rate_ = std::max(MIN_RATE, cut);
			cut_ = true;
			hold_until_ = p_now + HOLD_MS;
			calm_since_ = p_now;
		} else if (smoothed_ < LOW_MS) {
			if (p_now - calm_since_ >= (cut_ ? CALM_MS : SLOW_START_MS)) {
				rate_ = std::min(p_max_rate, rate_ + STEP);
				calm_since_ = p_now;
			}
		} else {
			calm_since_ = p_now;
		}
		return rate_;
	}

	double rate() const { return rate_; }

private:
	double rate_ = START_RATE;
	bool cut_ = false; // the uplink has queued: climb slowly from now on
	double smoothed_ = 0.0;
	uint32_t hold_until_ = 0;
	uint32_t calm_since_ = 0;
	bool calm_set_ = false;
};

// The client's downlink: how far behind each snapshot arrives, what arrives on the wire, and the
// share of snapshots lost, sampled each SAMPLE_MS, feeding RateAuto.
class DownlinkMeter {
public:
	static constexpr uint32_t SAMPLE_MS = 1000;

	// Running totals of what arrived (bytes on the wire, snapshots received and lost), read once a
	// SAMPLE_MS. Totals that went down belong to a new session.
	void sample(uint32_t p_now, uint64_t p_bytes, uint64_t p_received, uint64_t p_lost) {
		if (sampled_ && p_bytes >= bytes_ && p_now != sample_ms_) {
			delivered_bps_ = (double)(p_bytes - bytes_) * 1000.0 / (double)(p_now - sample_ms_);
		}
		if (sampled_ && p_received >= received_ && p_lost >= lost_) {
			const uint64_t got = p_received - received_;
			const uint64_t missed = p_lost - lost_;
			if (got + missed > 0) {
				loss_ = (double)missed / (double)(got + missed);
			}
		}
		sampled_ = true;
		sample_ms_ = p_now;
		bytes_ = p_bytes;
		received_ = p_received;
		lost_ = p_lost;
	}

	bool sample_due(uint32_t p_now) const { return !sampled_ || p_now - sample_ms_ >= SAMPLE_MS; }

	// One snapshot arrived, stamped with the server's send time. Returns the rate to ask for.
	int note(uint32_t p_now, uint32_t p_server_time_ms) {
		lag_ = queue_.note(p_now, (int64_t)p_server_time_ms);
		return rate_auto_.note(p_now, (double)lag_, delivered_bps_, loss_);
	}

	uint32_t lag_ms() const { return lag_; }
	double delivered_bps() const { return delivered_bps_; }
	int rate() const { return rate_auto_.rate(); }

private:
	QueueMeter queue_;
	RateAuto rate_auto_;
	uint32_t lag_ = 0;
	double delivered_bps_ = 0.0;
	double loss_ = 0.0;
	bool sampled_ = false;
	uint32_t sample_ms_ = 0;
	uint64_t bytes_ = 0;
	uint64_t received_ = 0;
	uint64_t lost_ = 0;
};

} // namespace goldnet
