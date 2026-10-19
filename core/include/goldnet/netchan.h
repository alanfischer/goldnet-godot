#pragma once
//
// GoldSrc's netchan on goldnet's packets: a reliable stream and an unreliable datagram riding every
// packet both ways, the snapshots down and the uplink packets up. The host's messages are opaque
// bytes; in Godot each is one RPC.
//
// Reliable: one block in flight at a time, marked with an alternating bit. Messages queued while a
// block is out wait for it. The other end acks with the newest packet it received and the bit of
// the last block it took. An ack of a packet newer than the one that last carried the block, with
// the bit unflipped, means the block was lost, and it goes again in the next packet: loss is learned
// from acks, never from a timer, so a block that arrived is never sent twice. Pending unfragmented
// data past RELIABLE_OVERFLOW_BYTES is an overflow, which GoldSrc dropped the client for.
//
// A message bigger than a block is cut into numbered fragments and takes the block's turn: nothing
// else reliable goes until all of it has arrived, so order holds. Unlike GoldSrc, which sent one
// fragment per round trip, up to RELIABLE_FRAGMENT_WINDOW are in flight, one per packet. The
// receiver says which fragment it needs next, and that one goes again once an ack of a newer packet
// shows it still missing, the block's rule.
//
// Datagram: messages for the next packet only, all of them or none (GoldSrc: "unreliable would
// overflow, ignoring"). The receiver takes a packet's channel part only from a packet newer than
// every one before it, so an old datagram never plays after a newer one.
//
// On the wire, a packet's channel part is:
//   u16  ack            the newest packet received from the other end
//   u8   flags          CHAN_* below
//   u16  next_fragment  only with CHAN_FRAGMENT_ACK: the fragment this end needs next
//   u16  block_len      only with CHAN_RELIABLE, then the block's bytes
//   u16  fragment, u16 remaining, u16 len   only with CHAN_FRAGMENT: its number, how many of its
//                       message follow it, then its bytes
//   u16  datagram_len   then the datagram's bytes
// A block, a message's fragments joined, and a datagram are each a run of messages: uvarint len,
// then len bytes.
//
#include "goldnet/buffer.h"
#include "goldnet/codec.h"
#include "goldnet/protocol.h"

#include <algorithm>
#include <deque>
#include <map>
#include <vector>

namespace goldnet {

enum ChanFlags : uint8_t {
	CHAN_RELIABLE = 1,     // a reliable block follows
	CHAN_RELIABLE_BIT = 2, // that block's alternating bit
	CHAN_FRAGMENT = 4,     // a fragment follows
	CHAN_FRAGMENT_ACK = 8, // the next fragment this end needs follows the flags
	CHAN_ACK_BIT = 16,     // the bit of the last block this end took from the other
};

// What one end tells the other it has received, in each packet's channel part.
struct ChannelAck {
	uint16_t received = 0;      // the newest packet
	bool bit = false;           // the bit of the last block taken
	bool fragments = false;     // whether next_fragment is said
	uint16_t next_fragment = 0; // the fragment needed next
};

// Appends one message, length first, to a block or datagram.
inline void put_message(std::vector<uint8_t> &r_out, const uint8_t *p_data, size_t p_size) {
	struct Sink {
		std::vector<uint8_t> *v;
		void put_u8(uint8_t b) { v->push_back(b); }
	} sink{ &r_out };
	put_uvarint(&sink, (uint32_t)p_size);
	r_out.insert(r_out.end(), p_data, p_data + p_size);
}

// Splits a run of messages. False if it doesn't parse to the end.
inline bool split_messages(const uint8_t *p_data, size_t p_size, std::vector<std::vector<uint8_t>> &r_out) {
	Reader r(p_data, p_size);
	while (r.remaining() > 0) {
		const uint32_t len = get_uvarint(&r);
		const uint8_t *d = r.get_data(len);
		if (!d) {
			return false;
		}
		r_out.emplace_back(d, d + len);
	}
	return !r.failed();
}

inline bool fragment_before(uint16_t a, uint16_t b) { return (int16_t)(a - b) < 0; }

class ReliableSender {
public:
	void queue(const uint8_t *p_data, size_t p_size) {
		std::vector<uint8_t> m;
		put_message(m, p_data, p_size);
		if (m.size() <= (size_t)RELIABLE_BLOCK_BYTES) {
			pending_bytes_ += m.size();
		}
		pending_.push_back(std::move(m));
		queued_++;
	}

	// What the other end has received from us.
	void on_ack(const ChannelAck &p_ack) {
		if (acked_any_ && !seq_newer(p_ack.received, ack_received_)) {
			return;
		}
		acked_any_ = true;
		ack_received_ = p_ack.received;
		if (in_flight_ && p_ack.bit == bit_) {
			in_flight_ = false;
			delivered_ += block_msgs_;
			block_msgs_ = 0;
			block_.clear();
		}
		if (p_ack.fragments) {
			while (!frags_.empty() && fragment_before(frags_.front().num, p_ack.next_fragment)) {
				if (frags_.front().remaining == 0) {
					delivered_++;
				}
				frags_.pop_front();
			}
		}
	}

	// Whether the next packet carries reliable data: a lost block, a fragment, or a new block.
	bool due() const {
		if (in_flight_) {
			return block_lost();
		}
		if (!frags_.empty()) {
			return fragment_to_send() >= 0;
		}
		return !pending_.empty();
	}

	// For packet p_seq, which goes: the CHAN_* flags of the reliable data it carries and that data,
	// as it goes on the wire (see the top of this file). 0 = none.
	uint8_t write(uint16_t p_seq, std::vector<uint8_t> &r_part) {
		r_part.clear();
		Writer w;
		uint8_t flags = 0;
		if (in_flight_ ? block_lost() : (frags_.empty() && !cut_fragments() && start_block())) {
			sent_ = true;
			sent_seq_ = p_seq;
			w.put_u16((uint16_t)block_.size());
			w.put_data(block_.data(), block_.size());
			flags = CHAN_RELIABLE | (bit_ ? CHAN_RELIABLE_BIT : 0);
		} else if (!in_flight_ && !frags_.empty()) {
			const int i = fragment_to_send();
			if (i < 0) {
				return 0;
			}
			Frag &f = frags_[i];
			f.sent = true;
			f.sent_packet = p_seq;
			w.put_u16(f.num);
			w.put_u16(f.remaining);
			w.put_u16((uint16_t)f.bytes.size());
			w.put_data(f.bytes.data(), f.bytes.size());
			flags = CHAN_FRAGMENT;
		}
		r_part.assign(w.data(), w.data() + w.size());
		return flags;
	}

	bool overflowed() const { return pending_bytes_ > (size_t)RELIABLE_OVERFLOW_BYTES; }
	// Messages ever queued, and how many of them are proven delivered. Both count in queue order, so
	// message n (0-based) has arrived once delivered() > n.
	uint64_t queued() const { return queued_; }
	uint64_t delivered() const { return delivered_; }
	bool idle() const { return !in_flight_ && frags_.empty() && pending_.empty(); }

private:
	struct Frag {
		uint16_t num = 0;
		uint16_t remaining = 0; // fragments of its message after it
		std::vector<uint8_t> bytes;
		bool sent = false;
		uint16_t sent_packet = 0;
	};

	bool block_lost() const { return sent_ && acked_any_ && seq_newer(ack_received_, sent_seq_); }

	// The first fragment the other end lacks if it is proven lost, else the first not sent yet within
	// the window. -1 = none.
	int fragment_to_send() const {
		const Frag &head = frags_.front();
		if (head.sent && acked_any_ && seq_newer(ack_received_, head.sent_packet)) {
			return 0;
		}
		const int n = std::min((int)frags_.size(), RELIABLE_FRAGMENT_WINDOW);
		for (int i = 0; i < n; i++) {
			if (!frags_[i].sent) {
				return i;
			}
		}
		return -1;
	}

	// Cuts the next message into fragments if it is too big for a block. True if it did.
	bool cut_fragments() {
		if (pending_.empty() || pending_.front().size() <= (size_t)RELIABLE_BLOCK_BYTES) {
			return false;
		}
		const std::vector<uint8_t> &m = pending_.front();
		const size_t count = (m.size() + RELIABLE_BLOCK_BYTES - 1) / RELIABLE_BLOCK_BYTES;
		for (size_t i = 0; i < count; i++) {
			Frag f;
			f.num = next_fragment_++;
			f.remaining = (uint16_t)(count - 1 - i);
			const size_t at = i * RELIABLE_BLOCK_BYTES;
			f.bytes.assign(m.begin() + at, m.begin() + std::min(m.size(), at + RELIABLE_BLOCK_BYTES));
			frags_.push_back(std::move(f));
		}
		pending_.pop_front();
		return true;
	}

	bool start_block() {
		block_.clear();
		while (!pending_.empty() && block_.size() + pending_.front().size() <= (size_t)RELIABLE_BLOCK_BYTES) {
			block_.insert(block_.end(), pending_.front().begin(), pending_.front().end());
			pending_bytes_ -= pending_.front().size();
			pending_.pop_front();
			block_msgs_++;
		}
		if (block_.empty()) {
			return false;
		}
		bit_ = !bit_;
		in_flight_ = true;
		sent_ = false;
		return true;
	}

	std::deque<std::vector<uint8_t>> pending_; // messages, length-prefixed, oldest first
	size_t pending_bytes_ = 0;                 // of the ones small enough not to fragment
	std::deque<Frag> frags_;                   // the message being fragmented, its unacked fragments
	uint16_t next_fragment_ = 0;
	std::vector<uint8_t> block_; // the block in flight
	uint32_t block_msgs_ = 0;    // messages it completes
	bool in_flight_ = false;
	bool bit_ = false;
	bool sent_ = false;
	uint16_t sent_seq_ = 0; // the packet that last carried it
	bool acked_any_ = false;
	uint16_t ack_received_ = 0;
	uint64_t queued_ = 0;
	uint64_t delivered_ = 0;
};

// What a ChannelReceiver has to say back, kept apart from it so a host can hand it to whatever
// writes the next packet: the ack is made when that packet is written.
struct ChannelAckSource {
	uint16_t received = 0;
	bool bit = false;
	bool fragment_seen = false;
	uint32_t last_fragment_ms = 0;
	uint16_t next_fragment = 0;

	ChannelAck at(uint32_t p_now) const {
		ChannelAck a;
		a.received = received;
		a.bit = bit;
		a.fragments = fragment_seen && p_now - last_fragment_ms < FRAGMENT_ACK_MS;
		a.next_fragment = next_fragment;
		return a;
	}
};

class ChannelReceiver {
public:
	// Whether packet p_seq is newer than every one before it. Only then does its channel part count.
	bool accept(uint16_t p_seq) {
		if (has_ && !seq_newer(p_seq, src_.received)) {
			return false;
		}
		has_ = true;
		src_.received = p_seq;
		return true;
	}

	// A packet's block: the messages it completes, appended to r_msgs. False if it doesn't parse.
	bool take_block(uint8_t p_flags, const uint8_t *p_data, size_t p_size, std::vector<std::vector<uint8_t>> &r_msgs) {
		const bool bit = (p_flags & CHAN_RELIABLE_BIT) != 0;
		if (bit == src_.bit) {
			return true; // taken already
		}
		src_.bit = bit;
		return split_messages(p_data, p_size, r_msgs);
	}

	// A packet's fragment: the message it completes, if any, appended to r_msgs.
	bool take_fragment(uint16_t p_num, uint16_t p_remaining, const uint8_t *p_data, size_t p_size, uint32_t p_now,
			std::vector<std::vector<uint8_t>> &r_msgs) {
		src_.fragment_seen = true;
		src_.last_fragment_ms = p_now;
		if (fragment_before(p_num, src_.next_fragment) || (uint16_t)(p_num - src_.next_fragment) >= 4 * RELIABLE_FRAGMENT_WINDOW) {
			return true; // had it already, or implausibly far ahead
		}
		held_[p_num] = Held{ p_remaining, std::vector<uint8_t>(p_data, p_data + p_size) };
		for (auto it = held_.find(src_.next_fragment); it != held_.end(); it = held_.find(src_.next_fragment)) {
			buf_.insert(buf_.end(), it->second.bytes.begin(), it->second.bytes.end());
			const uint16_t remaining = it->second.remaining;
			held_.erase(it);
			src_.next_fragment++;
			if (remaining == 0) {
				const bool ok = split_messages(buf_.data(), buf_.size(), r_msgs);
				buf_.clear();
				if (!ok) {
					return false;
				}
			}
		}
		return true;
	}

	bool has() const { return has_; }
	uint16_t received() const { return src_.received; }
	bool bit() const { return src_.bit; }
	const ChannelAckSource &ack_source() const { return src_; }

private:
	struct Held {
		uint16_t remaining = 0;
		std::vector<uint8_t> bytes;
	};
	bool has_ = false;
	ChannelAckSource src_;
	std::map<uint16_t, Held> held_; // fragments past a missing one
	std::vector<uint8_t> buf_;      // the message being joined
};

class DatagramSender {
public:
	// False if the datagram is full: the message is dropped.
	bool queue(const uint8_t *p_data, size_t p_size) {
		std::vector<uint8_t> m;
		put_message(m, p_data, p_size);
		if (bytes_.size() + m.size() > (size_t)SAFE_PACKET_BYTES) {
			dropped_++;
			return false;
		}
		bytes_.insert(bytes_.end(), m.begin(), m.end());
		return true;
	}

	// The datagram for the next packet if it fits in p_room, else nothing. Emptied either way.
	void take(int p_room, std::vector<uint8_t> &r_out) {
		if ((int)bytes_.size() <= p_room) {
			r_out = std::move(bytes_);
		} else if (!bytes_.empty()) {
			dropped_++;
		}
		bytes_.clear();
	}

	bool empty() const { return bytes_.empty(); }
	uint32_t dropped() const { return dropped_; }

private:
	std::vector<uint8_t> bytes_;
	uint32_t dropped_ = 0;
};

// The channel part of a packet, as written. p_part is ReliableSender::write's.
inline void put_channel(Writer &w, const ChannelAck &p_ack, uint8_t p_part_flags, const std::vector<uint8_t> &p_part,
		const std::vector<uint8_t> &p_datagram) {
	w.put_u16(p_ack.received);
	w.put_u8((uint8_t)(p_part_flags | (p_ack.bit ? CHAN_ACK_BIT : 0) | (p_ack.fragments ? CHAN_FRAGMENT_ACK : 0)));
	if (p_ack.fragments) {
		w.put_u16(p_ack.next_fragment);
	}
	w.put_data(p_part.data(), p_part.size());
	w.put_u16((uint16_t)p_datagram.size());
	w.put_data(p_datagram.data(), p_datagram.size());
}

// The channel part of a packet, as read. The pointers are into the packet.
struct ChannelPart {
	ChannelAck ack;
	uint8_t flags = 0;
	const uint8_t *block = nullptr;
	size_t block_len = 0;
	uint16_t fragment = 0;
	uint16_t fragment_remaining = 0;
	const uint8_t *fragment_data = nullptr;
	size_t fragment_len = 0;
	const uint8_t *datagram = nullptr;
	size_t datagram_len = 0;

	void read(Reader &r) {
		ack.received = r.get_u16();
		flags = r.get_u8();
		ack.bit = (flags & CHAN_ACK_BIT) != 0;
		ack.fragments = (flags & CHAN_FRAGMENT_ACK) != 0;
		if (ack.fragments) {
			ack.next_fragment = r.get_u16();
		}
		if (flags & CHAN_RELIABLE) {
			block_len = r.get_u16();
			block = r.get_data(block_len);
		}
		if (flags & CHAN_FRAGMENT) {
			fragment = r.get_u16();
			fragment_remaining = r.get_u16();
			fragment_len = r.get_u16();
			fragment_data = r.get_data(fragment_len);
		}
		datagram_len = r.get_u16();
		datagram = r.get_data(datagram_len);
	}

	// The messages this packet delivers: the reliable ones its block or fragment completes, then its
	// datagram's. False if any of it doesn't parse.
	bool take(ChannelReceiver &p_in, uint32_t p_now, std::vector<std::vector<uint8_t>> &r_reliable,
			std::vector<std::vector<uint8_t>> &r_unreliable) const {
		if ((flags & CHAN_RELIABLE) && !p_in.take_block(flags, block, block_len, r_reliable)) {
			return false;
		}
		if ((flags & CHAN_FRAGMENT) &&
				!p_in.take_fragment(fragment, fragment_remaining, fragment_data, fragment_len, p_now, r_reliable)) {
			return false;
		}
		return split_messages(datagram, datagram_len, r_unreliable);
	}
};

} // namespace goldnet
