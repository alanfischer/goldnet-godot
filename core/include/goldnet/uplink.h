#pragma once
//
// The uplink: a client's commands to the server, carried the Quake way. Each command reaches the
// server's host once, in the order it was made, with no reliable channel underneath. Every packet
// carries the commands not sent yet, the `backup` before them (GoldSrc cl_cmdbackup), and any
// unacked one out longer than a round trip, which is presumably lost. The server's ack (the newest
// command delivered with none missing before it) rides each snapshot header, and the client's
// snapshot ack rides each uplink packet, so neither direction needs a packet of its own for acks.
//
// A command is opaque bytes, tagged with a stream id the host picks (in Godot, the RPC it stands
// for). A client's streams share one sequence, so the order holds across them. Each command is
// delta-coded bytewise against the previous command of its stream in the same packet, the first
// against zeros (Quake's nullcmd), so one that repeats most of the last costs about its changed
// bytes.
//
// The one way this is weaker than a reliable channel: a command that falls out of the client's
// COMMAND_RING unacked is given up on. The packet's floor says so, and the server skips past it
// rather than wait forever. That takes about a second of total loss.
//
// The packet is also the client's end of a GoldSrc netchan (netchan.h): queue_reliable and
// queue_unreliable put the host's other messages in it, delivered to the server's host ahead of the
// commands (reliable) and after them (datagram), as GoldSrc ran a string command before a move.
//
// Each packet also carries its send time, the client's rate (GoldSrc `rate`) and update rate (GoldSrc
// `cl_updaterate`). The server meters
// the uplink from the send times (auto_rate.h) and reports it back in the snapshot header, where the
// client's CmdRateAuto reads it.
//
// Uplink packet:
//   u8   version          SNAPSHOT_VERSION
//   u16  seq              this packet's, counting up from 1 and skipping 0 like a snapshot's
//   u16  snapshot_ack     the newest snapshot the client committed; 0 = none
//   u16  send_time        the client's clock at send, ms, low 16 bits
//   uvarint rate          the rate the client asks for, in units of 100 bytes/sec; 0 = no limit
//   uvarint update_rate   the snapshots/sec it asks for; 0 = as many as the server sends
//   ...  channel part     (netchan.h) its ack is the newest snapshot received, committed or not
//   u16  floor            the oldest command the client still holds; its next seq if it holds none
//   u8   stream_ct        then stream_ct × u32 stream_id
//   u8   cmd_ct           then, if cmd_ct > 0, u16 first_seq and cmd_ct × command, seqs ascending
//   command:  uvarint (seq − previous seq − 1), absent for the first
//             u8 stream index, only when stream_ct > 1
//             uvarint len, then ceil(len / 8) mask bytes (bit i: byte i differs from the base),
//             then the differing bytes
//
// The host passed to CommandReceiver::receive() provides:
//   void protocol_mismatch(int32_t peer)            a different build is talking to us (once per peer)
//   void snapshot_ack(int32_t peer, uint16_t seq)   pass to Server::on_ack
//   void channel_ack(int32_t peer, const ChannelAck &ack)   pass to Server::on_channel_ack
//   void command(int32_t peer, uint32_t stream_id, const uint8_t *data, size_t size)
//   void message(int32_t peer, const uint8_t *data, size_t size, bool reliable)
//
#include "goldnet/auto_rate.h"
#include "goldnet/buffer.h"
#include "goldnet/codec.h"
#include "goldnet/netchan.h"
#include "goldnet/protocol.h"

#include <algorithm>
#include <deque>
#include <unordered_map>
#include <vector>

namespace goldnet {

inline uint16_t command_seq_next(uint16_t p_seq) { return p_seq == 0xFFFFu ? 1 : (uint16_t)(p_seq + 1); }
inline uint16_t command_seq_prev(uint16_t p_seq) { return p_seq <= 1 ? 0xFFFFu : (uint16_t)(p_seq - 1); }

// One command's bytes against its base (null = zeros).
inline void put_byte_delta(Writer &w, const std::vector<uint8_t> *p_base, const std::vector<uint8_t> &p_cur) {
	const size_t n = p_cur.size();
	put_uvarint(&w, (uint32_t)n);
	std::vector<uint8_t> mask((n + 7) / 8, 0);
	for (size_t i = 0; i < n; i++) {
		const uint8_t b = p_base && i < p_base->size() ? (*p_base)[i] : 0;
		if (p_cur[i] != b) {
			mask[i >> 3] |= (uint8_t)(1u << (i & 7));
		}
	}
	w.put_data(mask.data(), mask.size());
	for (size_t i = 0; i < n; i++) {
		if (mask[i >> 3] & (1u << (i & 7))) {
			w.put_u8(p_cur[i]);
		}
	}
}

inline bool get_byte_delta(Reader &r, const std::vector<uint8_t> *p_base, std::vector<uint8_t> &r_out) {
	const uint32_t n = get_uvarint(&r);
	if (n > (uint32_t)SAFE_PACKET_BYTES * 8) {
		return false; // more than any packet could carry
	}
	const uint8_t *mask = r.get_data((n + 7) / 8);
	if (!mask) {
		return false;
	}
	r_out.resize(n);
	for (uint32_t i = 0; i < n; i++) {
		if (mask[i >> 3] & (1u << (i & 7))) {
			r_out[i] = r.get_u8();
		} else {
			r_out[i] = p_base && i < p_base->size() ? (*p_base)[i] : 0;
		}
	}
	return !r.failed();
}

// The client end.
class CommandSender {
public:
	// Queue one command. Returns its seq.
	uint16_t push(uint32_t p_stream, const uint8_t *p_data, size_t p_size, uint32_t p_now) {
		Cmd c;
		c.seq = next_seq_;
		c.stream = p_stream;
		c.bytes.assign(p_data, p_data + p_size);
		next_seq_ = command_seq_next(next_seq_);
		ring_.push_back(std::move(c));
		if (ring_.size() > (size_t)COMMAND_RING) {
			ring_.pop_front();
			dropped_++;
		}
		last_push_ms_ = p_now;
		pushed_ = true;
		return ring_.back().seq;
	}

	// The command ack from a snapshot header: everything up to p_seq arrived.
	void on_ack(uint16_t p_seq) {
		if (p_seq == 0 || ring_.empty() || !seq_le(ring_.front().seq, p_seq) || seq_newer(p_seq, ring_.back().seq)) {
			return; // nothing new, or not something we sent
		}
		while (!ring_.empty() && seq_le(ring_.front().seq, p_seq)) {
			ring_.pop_front();
		}
	}

	// Packets per second as an interval (GoldSrc cl_cmdrate). 0 = whenever there is something to send.
	void set_interval_ms(uint32_t p_ms) { interval_ms_ = p_ms; }
	uint32_t interval_ms() const { return interval_ms_; }
	void set_backup(int p_backup) { backup_ = p_backup < 0 ? 0 : p_backup; }
	// The rate this client asks the server for, in bytes/sec (GoldSrc `rate`). 0 = no limit.
	void set_rate(uint32_t p_bytes_per_sec) { rate_ = p_bytes_per_sec; }
	uint32_t rate() const { return rate_; }
	// The snapshots/sec this client asks for (GoldSrc cl_updaterate). 0 = as many as the server sends.
	void set_update_rate(uint32_t p_hz) { update_rate_ = p_hz; }
	uint32_t update_rate() const { return update_rate_; }

	// The client's other messages (netchan.h).
	void queue_reliable(const uint8_t *p_data, size_t p_size) { rel_.queue(p_data, p_size); }
	bool queue_unreliable(const uint8_t *p_data, size_t p_size) { return dgram_.queue(p_data, p_size); }
	bool overflowed() const { return rel_.overflowed(); }
	uint64_t reliable_queued() const { return rel_.queued(); }
	uint64_t reliable_delivered() const { return rel_.delivered(); }
	// From a snapshot header: what the server has received of this channel. The round trip is timed
	// here, from the newest packet received, the first time it is named: a packet goes once, so its ack
	// says which send arrived, however often the commands in it went again. Timed from commands, a
	// round trip longer than the ring was never measured, and every packet then resent all of it.
	// p_now: the clock write() is given.
	void on_channel_ack(const ChannelAck &p_ack, uint32_t p_now) {
		rel_.on_ack(p_ack);
		if (!chan_acked_ || seq_newer(p_ack.received, chan_received_)) {
			chan_acked_ = true;
			chan_received_ = p_ack.received;
			const int slot = p_ack.received & (SENT_RING - 1);
			if (sent_seq_[slot] == p_ack.received) {
				const uint32_t sample = p_now - sent_ms_[slot];
				srtt_ms_ = srtt_ms_ == 0 ? sample : (srtt_ms_ * 7 + sample) / 8;
			}
		}
	}

	// Whether a packet should go now: a command not sent yet, or one overdue, or a message, and the
	// interval is up.
	bool due(uint32_t p_now) const {
		if (interval_ms_ > 0 && sent_any_ && p_now - last_send_ms_ < interval_ms_) {
			return false;
		}
		if (rel_.due() || !dgram_.empty()) {
			return true;
		}
		for (const Cmd &c : ring_) {
			if (c.sends == 0 || p_now - c.last_sent_ms >= overdue_ms() || proven_lost(c)) {
				return true;
			}
		}
		return false;
	}

	// How long ago the last command was pushed, so a host can tell an active stream from an idle one.
	uint32_t idle_ms(uint32_t p_now) const { return pushed_ ? p_now - last_push_ms_ : 0xFFFFFFFFu; }

	// Build the next uplink packet, carrying p_snapshot_ack, what this end has received of the server's
	// channel (Client::channel_ack), and whatever is due.
	void write(uint32_t p_now, uint16_t p_snapshot_ack, const ChannelAckSource &p_received, Writer &r_out) {
		const uint16_t seq = seq_;
		seq_ = command_seq_next(seq_);
		const ChannelAck ack = p_received.at(p_now);
		std::vector<uint8_t> block;
		const uint8_t block_flags = rel_.write(seq, block);
		const int channel_bytes = (int)block.size() + (ack.fragments ? 2 : 0);
		std::vector<uint8_t> datagram;
		dgram_.take(SAFE_PACKET_BYTES - HEADER_BYTES - channel_bytes, datagram);

		const size_t size = ring_.size();
		size_t first_new = size;
		while (first_new > 0 && ring_[first_new - 1].sends == 0) {
			first_new--;
		}
		const size_t fresh_from = first_new < size ? (first_new > (size_t)backup_ ? first_new - backup_ : 0) : size;
		const uint32_t overdue = overdue_ms();
		std::vector<size_t> picks;
		std::vector<uint32_t> streams;
		for (size_t i = 0; i < size; i++) {
			if (i < fresh_from && p_now - ring_[i].last_sent_ms < overdue && !proven_lost(ring_[i])) {
				continue;
			}
			if (std::find(streams.begin(), streams.end(), ring_[i].stream) == streams.end()) {
				if (streams.size() == 255) {
					continue;
				}
				streams.push_back(ring_[i].stream);
			}
			picks.push_back(i);
		}

		const int budget = SAFE_PACKET_BYTES - HEADER_BYTES - 4 * (int)streams.size() - (int)datagram.size() - channel_bytes;
		Writer body;
		std::vector<const std::vector<uint8_t> *> prev(streams.size(), nullptr);
		std::vector<size_t> sent;
		uint16_t last_seq = 0;
		for (size_t i : picks) {
			const Cmd &c = ring_[i];
			const size_t si = std::find(streams.begin(), streams.end(), c.stream) - streams.begin();
			Writer w;
			if (!sent.empty()) {
				put_uvarint(&w, (uint16_t)(c.seq - last_seq - 1));
			}
			if (streams.size() > 1) {
				w.put_u8((uint8_t)si);
			}
			put_byte_delta(w, prev[si], c.bytes);
			// Past the budget it waits for a later packet. The first always goes, so a command bigger
			// than a packet can't stall everything behind it.
			if (sent.size() == 255 || (!sent.empty() && (int)(body.size() + w.size()) > budget)) {
				continue;
			}
			body.put_data(w);
			prev[si] = &c.bytes;
			last_seq = c.seq;
			sent.push_back(i);
		}

		r_out.put_u8(SNAPSHOT_VERSION);
		r_out.put_u16(seq);
		r_out.put_u16(p_snapshot_ack);
		r_out.put_u16((uint16_t)p_now);
		put_uvarint(&r_out, (rate_ + 99) / 100);
		put_uvarint(&r_out, update_rate_);
		put_channel(r_out, ack, block_flags, block, datagram);
		r_out.put_u16(ring_.empty() ? next_seq_ : ring_.front().seq);
		r_out.put_u8((uint8_t)streams.size());
		for (uint32_t s : streams) {
			r_out.put_u32(s);
		}
		r_out.put_u8((uint8_t)sent.size());
		if (!sent.empty()) {
			r_out.put_u16(ring_[sent.front()].seq);
			r_out.put_data(body);
		}
		for (size_t i : sent) {
			Cmd &c = ring_[i];
			c.sends++;
			c.last_sent_ms = p_now;
			c.last_packet = seq;
		}
		sent_seq_[seq & (SENT_RING - 1)] = seq;
		sent_ms_[seq & (SENT_RING - 1)] = p_now;
		last_send_ms_ = p_now;
		sent_any_ = true;
	}

	size_t pending() const { return ring_.size(); }
	uint32_t dropped() const { return dropped_; }
	uint32_t rtt_ms() const { return srtt_ms_; }

	// A fresh session: the server knows nothing of the old one's seqs.
	void reset() { *this = CommandSender(); }

private:
	struct Cmd {
		uint16_t seq = 0;
		uint32_t stream = 0;
		std::vector<uint8_t> bytes;
		uint32_t sends = 0;
		uint32_t last_sent_ms = 0;
		uint16_t last_packet = 0; // the uplink packet that last carried it
	};

	uint32_t overdue_ms() const { return goldnet::overdue_ms(srtt_ms_); }

	// The server has a packet newer than the one that last carried this command and still hasn't acked
	// it: lost, or held behind one that was. Either way it goes again now rather than when overdue,
	// the netchan's rule for a block. A held one arrives twice, which the server ignores.
	bool proven_lost(const Cmd &p_c) const {
		return p_c.sends > 0 && chan_acked_ && seq_newer(chan_received_, p_c.last_packet);
	}

	// Packets whose send time is kept for timing their ack: about 8 s at one packet a tick.
	static constexpr int SENT_RING = 512;
	// The packet's fixed part, with the rates at their widest and an empty channel part.
	static constexpr int HEADER_BYTES = 23;

	std::deque<Cmd> ring_; // unacked, oldest first
	uint16_t next_seq_ = 1;
	uint32_t interval_ms_ = 0;
	uint32_t rate_ = 0;
	uint32_t update_rate_ = 0;
	int backup_ = COMMAND_BACKUP_DEFAULT;
	uint32_t last_send_ms_ = 0;
	bool sent_any_ = false;
	uint32_t last_push_ms_ = 0;
	bool pushed_ = false;
	uint32_t srtt_ms_ = 0;
	uint16_t sent_seq_[SENT_RING] = {};
	uint32_t sent_ms_[SENT_RING] = {};
	uint32_t dropped_ = 0;
	uint16_t seq_ = 1;
	bool chan_acked_ = false;
	uint16_t chan_received_ = 0; // the newest uplink packet the server has received
	ReliableSender rel_;
	DatagramSender dgram_;
};

// The server end: one sequence per peer, delivered in order.
class CommandReceiver {
public:
	// One uplink packet from p_peer. False if it was malformed or from a mismatched build. p_now: the
	// host's clock, the one it writes its snapshots by.
	template <class Host>
	bool receive(int32_t p_peer, const uint8_t *p_data, size_t p_size, Host &p_host, uint32_t p_now);

	// The newest command from p_peer delivered with none missing before it. 0 = none yet.
	uint16_t ack(int32_t p_peer) const {
		auto it = peers_.find(p_peer);
		return it == peers_.end() || !it->second.delivered ? 0 : command_seq_prev(it->second.next);
	}

	// What the server has received of p_peer's channel, for Server::set_uplink_ack.
	ChannelAckSource channel_ack(int32_t p_peer) const {
		auto it = peers_.find(p_peer);
		return it == peers_.end() ? ChannelAckSource() : it->second.chan.ack_source();
	}

	// Whether p_peer should hear from the server though its command ack hasn't moved (Server's
	// set_command_ack): it sent commands the server already had, so it lacks the ack, or the server is
	// holding commands behind a lost one and the peer needs to see which packets arrived to send it
	// again (CommandSender::proven_lost). Cleared by the call.
	bool take_news(int32_t p_peer) {
		auto it = peers_.find(p_peer);
		if (it == peers_.end() || !it->second.news) {
			return false;
		}
		it->second.news = false;
		return true;
	}

	// How far behind its best p_peer's uplink is arriving, ms, and its packets arriving per second
	// (auto_rate.h): for the peer's snapshot header (Server::set_uplink_report).
	uint32_t uplink_lag_ms(int32_t p_peer) const {
		auto it = peers_.find(p_peer);
		return it == peers_.end() ? 0 : it->second.lag_ms;
	}
	uint32_t uplink_pps(int32_t p_peer) const {
		auto it = peers_.find(p_peer);
		return it == peers_.end() ? 0 : it->second.arrivals.pps();
	}

	// The rate p_peer asks for, bytes/sec; 0 = no limit.
	uint32_t rate(int32_t p_peer) const {
		auto it = peers_.find(p_peer);
		return it == peers_.end() ? 0 : it->second.rate;
	}
	// The snapshots/sec p_peer asks for; 0 = as many as the server sends.
	uint32_t update_rate(int32_t p_peer) const {
		auto it = peers_.find(p_peer);
		return it == peers_.end() ? 0 : it->second.update_rate;
	}

	// Commands p_peer gave up on before they arrived.
	uint32_t skipped(int32_t p_peer) const {
		auto it = peers_.find(p_peer);
		return it == peers_.end() ? 0 : it->second.skipped;
	}

	void remove_peer(int32_t p_peer) { peers_.erase(p_peer); }

private:
	struct Held {
		uint32_t stream = 0;
		std::vector<uint8_t> bytes;
	};
	struct Peer {
		bool started = false;
		bool delivered = false;
		uint16_t next = 1; // the command to deliver next
		std::unordered_map<uint16_t, Held> held;
		uint32_t skipped = 0;
		bool warned = false;
		bool news = false; // see take_news
		ChannelReceiver chan;
		// The client's 16-bit send times, unwrapped against the newest seen.
		bool stamped = false;
		uint16_t stamp_last = 0;
		int64_t stamp_ms = 0;
		QueueMeter queue;
		ArrivalMeter arrivals;
		uint32_t lag_ms = 0;
		uint32_t rate = 0;
		uint32_t update_rate = 0;
	};
	// Commands held for a gap to fill are bounded by what a client can have unacked.
	static const int MAX_AHEAD = COMMAND_RING * 2;
	std::unordered_map<int32_t, Peer> peers_;
};

template <class Host>
bool CommandReceiver::receive(int32_t p_peer, const uint8_t *p_data, size_t p_size, Host &p_host, uint32_t p_now) {
	Reader r(p_data, p_size);
	const uint8_t version = r.get_u8();
	if (r.failed()) {
		return false;
	}
	if (version != SNAPSHOT_VERSION) {
		Peer &p = peers_[p_peer];
		if (!p.warned) {
			p.warned = true;
			p_host.protocol_mismatch(p_peer);
		}
		return false;
	}
	const uint16_t packet_seq = r.get_u16();
	const uint16_t snapshot_ack = r.get_u16();
	const uint16_t send_time = r.get_u16();
	const uint32_t rate = get_uvarint(&r);
	const uint32_t update_rate = get_uvarint(&r);
	ChannelPart chan;
	chan.read(r);
	uint16_t floor = r.get_u16();
	const int stream_ct = r.get_u8();
	std::vector<uint32_t> streams;
	for (int i = 0; i < stream_ct; i++) {
		streams.push_back(r.get_u32());
	}
	struct In {
		uint16_t seq;
		int stream;
		std::vector<uint8_t> bytes;
	};
	const int cmd_ct = r.get_u8();
	std::vector<In> cmds(cmd_ct);
	std::vector<const std::vector<uint8_t> *> prev(stream_ct, nullptr);
	uint16_t seq = cmd_ct > 0 ? r.get_u16() : 0;
	for (int i = 0; i < cmd_ct && !r.failed(); i++) {
		if (i > 0) {
			seq = (uint16_t)(seq + 1 + get_uvarint(&r));
		}
		const int si = stream_ct > 1 ? r.get_u8() : 0;
		if (si >= stream_ct || !get_byte_delta(r, prev[si], cmds[i].bytes)) {
			return false;
		}
		cmds[i].seq = seq;
		cmds[i].stream = si;
		prev[si] = &cmds[i].bytes;
	}
	if (r.failed()) {
		return false; // nothing from a truncated packet, not even its ack
	}

	if (snapshot_ack != 0) {
		p_host.snapshot_ack(p_peer, snapshot_ack);
	}
	Peer &p = peers_[p_peer];
	int64_t sent_ms = send_time;
	if (p.stamped) {
		sent_ms = p.stamp_ms + (int16_t)(uint16_t)(send_time - p.stamp_last);
	}
	if (!p.stamped || sent_ms > p.stamp_ms) {
		p.stamped = true;
		p.stamp_last = send_time;
		p.stamp_ms = sent_ms;
	}
	p.lag_ms = p.queue.note(p_now, sent_ms);
	p.arrivals.note(p_now);
	p.rate = rate > 0xFFFFFFFFu / 100 ? 0xFFFFFFFFu : rate * 100;
	p.update_rate = update_rate;
	// The channel only from a packet newer than every one before it. Commands carry their own seqs,
	// so a reordered packet still delivers those.
	std::vector<std::vector<uint8_t>> reliable;
	std::vector<std::vector<uint8_t>> unreliable;
	if (p.chan.accept(packet_seq)) {
		p_host.channel_ack(p_peer, chan.ack);
		if (!chan.take(p.chan, p_now, reliable, unreliable)) {
			return false;
		}
	}
	for (const std::vector<uint8_t> &m : reliable) {
		p_host.message(p_peer, m.data(), m.size(), true);
	}
	if (floor == 0) {
		floor = 1;
	}
	if (!p.started) {
		p.started = true;
		p.next = floor;
	} else if (seq_newer(floor, p.next)) {
		// The client gave up on everything before its floor. Waiting for those would stall forever.
		p.skipped += (uint16_t)(floor - p.next);
		for (auto it = p.held.begin(); it != p.held.end();) {
			it = seq_newer(floor, it->first) ? p.held.erase(it) : std::next(it);
		}
		p.next = floor;
		p.delivered = true;
	}
	bool any_new = false;
	for (In &c : cmds) {
		if (seq_newer(p.next, c.seq) || p.held.count(c.seq) || (uint16_t)(c.seq - p.next) >= MAX_AHEAD) {
			continue; // delivered already, a repeat, or implausibly far ahead
		}
		any_new = true;
		p.held[c.seq] = Held{ streams[c.stream], std::move(c.bytes) };
	}
	for (auto it = p.held.find(p.next); it != p.held.end(); it = p.held.find(p.next)) {
		const Held h = std::move(it->second);
		p.held.erase(it);
		p.next = command_seq_next(p.next);
		p.delivered = true;
		p_host.command(p_peer, h.stream, h.bytes.data(), h.bytes.size());
	}
	// Nothing new in a packet of commands: backups ride with new ones, so this is the client resending
	// for want of our ack. Or the server is stuck behind a lost one.
	if ((!cmds.empty() && !any_new) || !p.held.empty()) {
		p.news = true;
	}
	for (const std::vector<uint8_t> &m : unreliable) {
		p_host.message(p_peer, m.data(), m.size(), false);
	}
	return true;
}

} // namespace goldnet
