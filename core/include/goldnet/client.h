#pragma once
//
// The client half of the snapshot protocol: reconstruct each delta against the baseline frame the
// server diffed it against, apply spawns / despawns / leaves / changed slots through the host, and
// ack. Engine-agnostic, like Server.
//
// The host passed to apply() provides:
//   void protocol_mismatch()                       a different build is talking to us (once per session)
//   void server_time(uint32_t ms)                  every in-order snapshot's header clock
//   void send_ack(uint16_t seq)
//   void spawn(uint32_t net_id, uint32_t spawner_net_id, const uint8_t *data, size_t size)
//   void despawn(uint32_t net_id)
//   void relevance_lost(uint32_t net_id)
//   bool registered(uint32_t net_id)               does an entity with this id exist here?
//   int  begin_entity(uint32_t net_id, uint32_t mask, bool &r_defer)
//        slot count of a live target, or -1 if none; r_defer = known but not ready (warmup)
//   bool set_slot(int slot, const V &v)            apply to the current target; true if applied
//   void end_entity(bool applied)                  the current target is done
//   V    decode(Reader &r, uint32_t ref_time_ms)   one tagged value
//
#include "goldnet/buffer.h"
#include "goldnet/codec.h"
#include "goldnet/protocol.h"

#include <unordered_map>
#include <vector>

namespace goldnet {

template <class V>
class Client {
public:
	// What the last apply() saw, for a host's debug print.
	struct ApplyInfo {
		uint16_t seq = 0;
		uint16_t base_seq = 0;
		int spawns = 0;
		int despawns = 0;
		int changed = 0;
		bool committed = false;
	};

	// A fresh session: old baselines are meaningless.
	void reset() {
		for (int i = 0; i < RING; i++) {
			frames_[i].clear();
			frame_seq_[i] = 0;
		}
		last_seq_ = 0;
		has_ = false;
		defer_streak_ = 0;
	}

	template <class Host>
	ApplyInfo apply(const uint8_t *p_data, size_t p_size, Host &p_host);

private:
	// A frame is { net_id -> its slot values, in slot order }.
	typedef std::unordered_map<uint32_t, std::vector<V>> Frame;

	// Receive history — a mirror of the server's ring, so a delta can be reconstructed against any
	// recent baseline the server might diff against.
	Frame frames_[RING];
	uint16_t frame_seq_[RING] = {};
	uint16_t last_seq_ = 0;
	bool has_ = false;
	bool warned_mismatch_ = false;
	// Consecutive frames deferred because a KNOWN entity wasn't ready yet. Capped so an entity that
	// never resolves can't stall the stream forever; after the cap we accept the frame and move on.
	uint32_t defer_streak_ = 0;
	static const uint32_t MAX_DEFER_STREAK = RING; // ~1s at 30 Hz before giving up on a warmup entity
};

template <class V>
template <class Host>
typename Client<V>::ApplyInfo Client<V>::apply(const uint8_t *p_data, size_t p_size, Host &p_host) {
	ApplyInfo info;
	Reader r(p_data, p_size);

	// Protocol guard: a mismatched build (different wire format) would misparse every field. Fail
	// loudly once and drop, rather than flooding the log with decode errors.
	if (r.get_u8() != SNAPSHOT_VERSION) {
		if (!warned_mismatch_) {
			warned_mismatch_ = true;
			p_host.protocol_mismatch();
		}
		return info;
	}

	const uint16_t seq = r.get_u16();
	const uint16_t base_seq = r.get_u16();
	const uint32_t server_time = r.get_u32();
	info.seq = seq;
	info.base_seq = base_seq;
	if (r.failed()) {
		return info;
	}

	// Drop stale/reordered snapshots (unreliable transport). We only advance forward.
	if (has_ && !seq_newer(seq, last_seq_)) {
		return info;
	}

	// Surface the header clock: every snapshot carries the server's send-time, an always-on feed a
	// client can drive a clock estimator from. Emitted after the stale-seq drop (a reordered
	// snapshot's time is <= one already fed) but before the baseline-resolution drop below, so a
	// forward snapshot we can't reconstruct for state still contributes its valid clock sample.
	p_host.server_time(server_time);

	// Resolve the baseline frame we reconstruct this delta against. base_seq 0 means full state. If
	// the server diffed against a frame we no longer hold, we can't reconstruct unchanged fields —
	// re-ack our last good frame so the server falls back to a baseline we have (eventually a full
	// frame), and drop this one.
	const Frame *base = nullptr;
	if (base_seq != 0) {
		const int bslot = base_seq & (RING - 1);
		if (frame_seq_[bslot] == base_seq) {
			base = &frames_[bslot];
		} else {
			if (has_) {
				p_host.send_ack(last_seq_);
			}
			return info;
		}
	}

	// Spawns first: create the entities so they register before the entity deltas below
	// reference them.
	const int spawn_ct = r.get_u16();
	for (int i = 0; i < spawn_ct && !r.failed(); i++) {
		const uint32_t nid = r.get_u32();
		const uint32_t spawner_nid = r.get_u32();
		const uint32_t len = r.get_u32();
		const uint8_t *data = r.get_data(len);
		if (data) {
			p_host.spawn(nid, spawner_nid, data, len);
		}
	}
	info.spawns = spawn_ct;
	// Despawns: collect now (wire order), free after the entity deltas are applied.
	const int despawn_ct = r.get_u16();
	std::vector<uint32_t> despawns;
	for (int i = 0; i < despawn_ct && !r.failed(); i++) {
		despawns.push_back(r.get_u32());
	}
	info.despawns = despawn_ct;
	// Relevance leaves: collect now (wire order), apply after the entity deltas.
	const int leave_ct = r.get_u16();
	std::vector<uint32_t> leaves;
	for (int i = 0; i < leave_ct && !r.failed(); i++) {
		leaves.push_back(r.get_u32());
	}

	const int count = r.get_u16();
	info.changed = count;

	// New frame = baseline carried forward, with this delta's changed fields applied.
	Frame frame;
	if (base) {
		frame = *base;
	}

	// Set if a KNOWN entity couldn't be applied because it isn't ready yet — the warmup window. We
	// must not ack this frame as delivered (see the defer block below), or the server folds the
	// entity into this peer's baseline and — for an idle entity whose state never changes again —
	// never resends it, stranding it hidden.
	bool defer_ack = false;

	for (int i = 0; i < count && !r.failed(); i++) {
		const uint32_t net_id = r.get_u32();
		const uint32_t mask = get_uvarint(&r);

		bool defer = false;
		const int n_slots = p_host.begin_entity(net_id, mask, defer);
		if (defer) {
			defer_ack = true;
		}

		// Start from this entity's baseline values, then overlay the changed slots. Values are read
		// for every set bit whether or not there is a target, so the stream stays aligned even for
		// unknown ids.
		auto cur = frame.find(net_id);
		std::vector<V> vals;
		if (cur != frame.end()) {
			vals = cur->second;
		}
		if (n_slots >= 0 && vals.size() != (size_t)n_slots) {
			vals.resize(n_slots);
		}
		bool applied = false;
		for (int s = 0; s < MAX_SLOTS; s++) {
			if (!(mask & (1u << s))) {
				continue;
			}
			V v = p_host.decode(r, server_time); // self-delimiting — always consume
			if ((size_t)s < vals.size()) {
				vals[s] = v;
			}
			if (n_slots >= 0 && s < n_slots && p_host.set_slot(s, v)) {
				applied = true;
			}
		}
		if (!vals.empty()) {
			frame[net_id] = std::move(vals);
		}
		if (n_slots >= 0) {
			p_host.end_entity(applied);
		}
	}

	// A truncated or corrupt packet: whatever it applied stays applied (later snapshots carry the
	// same state again), but it is not a frame we can baseline against, so don't commit or ack it.
	if (r.failed()) {
		return info;
	}

	// Now free despawned entities (after the deltas so nothing applies to one we then free).
	for (uint32_t nid : despawns) {
		p_host.despawn(nid);
	}

	// Drop entities no longer registered here (e.g. after a map change / despawn) so the client
	// baseline can't diverge from the server's, which only tracks live entities.
	for (auto it = frame.begin(); it != frame.end();) {
		if (!p_host.registered(it->first)) {
			it = frame.erase(it);
		} else {
			++it;
		}
	}

	// Relevance leaves: drop from the reconstructed frame (so it matches the server's baseline,
	// which also dropped these) and tell the host to hide the entity.
	for (uint32_t nid : leaves) {
		frame.erase(nid);
		p_host.relevance_lost(nid);
	}

	// Warmup defer: a known entity wasn't ready, so we couldn't apply (reveal) it. Committing +
	// acking this seq would advance the server's baseline past the entity; an idle one that never
	// changes again would then never be resent — stranded hidden. Instead re-ack our last good
	// frame so the server keeps resending its full baseline against a frame we hold, and drop this
	// one uncommitted. Capped, and needs a prior good frame to fall back to; on the very first
	// frame we just accept it.
	if (defer_ack && has_ && defer_streak_ < MAX_DEFER_STREAK) {
		defer_streak_++;
		p_host.send_ack(last_seq_);
		return info;
	}
	defer_streak_ = 0;

	// Commit this frame to the ring and ack it (newest wins, unreliable).
	const int slot = seq & (RING - 1);
	frames_[slot] = std::move(frame);
	frame_seq_[slot] = seq;
	last_seq_ = seq;
	has_ = true;
	p_host.send_ack(seq);
	info.committed = true;
	return info;
}

} // namespace goldnet
