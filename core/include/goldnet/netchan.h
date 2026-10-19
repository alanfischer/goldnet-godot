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
// from acks, never from a timer, so a block that arrived is never sent twice. A message bigger than a
// block is cut into fragments, one per block, so it moves one fragment per round trip. Pending
// unfragmented data past RELIABLE_OVERFLOW_BYTES is an overflow, which GoldSrc dropped the client for.
//
// Datagram: messages for the next packet only, all of them or none (GoldSrc: "unreliable would
// overflow, ignoring"). The receiver takes a packet's block and datagram only from a packet newer
// than every one before it, so an old datagram never plays after a newer one.
//
// On the wire, a packet's channel part is:
//   u16  ack           the newest packet received from the other end
//   u8   flags         CHAN_* below
//   u16  block_len     only with CHAN_RELIABLE, then the block's bytes
//   u16  datagram_len  then the datagram's bytes
// A block (its fragments joined) and a datagram are each a run of messages: uvarint len, then len bytes.
//
#include "goldnet/buffer.h"
#include "goldnet/codec.h"
#include "goldnet/protocol.h"

#include <algorithm>
#include <deque>
#include <vector>

namespace goldnet {

enum ChanFlags : uint8_t {
	CHAN_RELIABLE = 1,      // a reliable block follows
	CHAN_RELIABLE_BIT = 2,  // that block's alternating bit
	CHAN_FRAGMENT = 4,      // the block is one fragment of a message
	CHAN_LAST_FRAGMENT = 8, // and the last one
	CHAN_ACK_BIT = 16,      // the bit of the last block this end took from the other
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

	// The other end's ack: the newest packet it received from us, and the bit of the last block it took.
	void on_ack(uint16_t p_received, bool p_bit) {
		if (acked_any_ && !seq_newer(p_received, ack_received_)) {
			return;
		}
		acked_any_ = true;
		ack_received_ = p_received;
		if (in_flight_ && p_bit == bit_) {
			in_flight_ = false;
			delivered_ += block_msgs_;
			block_msgs_ = 0;
			block_.clear();
		}
	}

	// Whether the next packet carries a block: a lost one to resend, or a new one to start.
	bool due() const { return in_flight_ ? lost() : (!frags_.empty() || !pending_.empty()); }

	// For packet p_seq, which goes: the CHAN_* flags and the block it carries. 0 = none.
	uint8_t write(uint16_t p_seq, std::vector<uint8_t> &r_block) {
		if (in_flight_ ? !lost() : !start_block()) {
			return 0;
		}
		sent_ = true;
		sent_seq_ = p_seq;
		r_block = block_;
		return block_flags_;
	}

	bool overflowed() const { return pending_bytes_ > (size_t)RELIABLE_OVERFLOW_BYTES; }
	// Messages ever queued, and how many of them are proven delivered. Both count in queue order, so
	// message n (0-based) has arrived once delivered() > n.
	uint64_t queued() const { return queued_; }
	uint64_t delivered() const { return delivered_; }
	bool idle() const { return !in_flight_ && frags_.empty() && pending_.empty(); }

private:
	bool lost() const { return sent_ && acked_any_ && seq_newer(ack_received_, sent_seq_); }

	bool start_block() {
		if (frags_.empty() && !pending_.empty() && pending_.front().size() > (size_t)RELIABLE_BLOCK_BYTES) {
			const std::vector<uint8_t> &m = pending_.front();
			for (size_t at = 0; at < m.size(); at += RELIABLE_BLOCK_BYTES) {
				const size_t n = std::min((size_t)RELIABLE_BLOCK_BYTES, m.size() - at);
				frags_.emplace_back(m.begin() + at, m.begin() + at + n);
			}
			pending_.pop_front();
		}
		block_.clear();
		uint8_t flags = 0;
		if (!frags_.empty()) {
			block_ = std::move(frags_.front());
			frags_.pop_front();
			flags = CHAN_FRAGMENT | (frags_.empty() ? CHAN_LAST_FRAGMENT : 0);
			block_msgs_ = frags_.empty() ? 1 : 0;
		} else {
			while (!pending_.empty() && block_.size() + pending_.front().size() <= (size_t)RELIABLE_BLOCK_BYTES) {
				block_.insert(block_.end(), pending_.front().begin(), pending_.front().end());
				pending_bytes_ -= pending_.front().size();
				pending_.pop_front();
				block_msgs_++;
			}
		}
		if (block_.empty()) {
			return false;
		}
		bit_ = !bit_;
		in_flight_ = true;
		sent_ = false;
		block_flags_ = CHAN_RELIABLE | (bit_ ? CHAN_RELIABLE_BIT : 0) | flags;
		return true;
	}

	std::deque<std::vector<uint8_t>> pending_; // messages, length-prefixed, oldest first
	size_t pending_bytes_ = 0;                 // of the ones small enough not to fragment
	std::deque<std::vector<uint8_t>> frags_;   // the rest of the message being fragmented
	std::vector<uint8_t> block_;               // the block in flight
	uint8_t block_flags_ = 0;
	uint32_t block_msgs_ = 0; // messages it completes
	bool in_flight_ = false;
	bool bit_ = false;
	bool sent_ = false;
	uint16_t sent_seq_ = 0; // the packet that last carried it
	bool acked_any_ = false;
	uint16_t ack_received_ = 0;
	uint64_t queued_ = 0;
	uint64_t delivered_ = 0;
};

class ChannelReceiver {
public:
	// Whether packet p_seq is newer than every one before it. Only then do its block and datagram count.
	bool accept(uint16_t p_seq) {
		if (has_ && !seq_newer(p_seq, received_)) {
			return false;
		}
		has_ = true;
		received_ = p_seq;
		return true;
	}

	// A packet's block: the messages it completes, appended to r_msgs. False if it doesn't parse.
	bool take(uint8_t p_flags, const uint8_t *p_data, size_t p_size, std::vector<std::vector<uint8_t>> &r_msgs) {
		const bool bit = (p_flags & CHAN_RELIABLE_BIT) != 0;
		if (bit == bit_) {
			return true; // taken already
		}
		bit_ = bit;
		if (!(p_flags & CHAN_FRAGMENT)) {
			return split_messages(p_data, p_size, r_msgs);
		}
		frag_.insert(frag_.end(), p_data, p_data + p_size);
		if (!(p_flags & CHAN_LAST_FRAGMENT)) {
			return true;
		}
		const bool ok = split_messages(frag_.data(), frag_.size(), r_msgs);
		frag_.clear();
		return ok;
	}

	bool has() const { return has_; }
	uint16_t received() const { return received_; }
	bool bit() const { return bit_; }

private:
	bool has_ = false;
	uint16_t received_ = 0;
	bool bit_ = false;
	std::vector<uint8_t> frag_;
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

// The channel part of a packet, as written.
inline void put_channel(Writer &w, uint16_t p_ack, bool p_ack_bit, uint8_t p_block_flags,
		const std::vector<uint8_t> &p_block, const std::vector<uint8_t> &p_datagram) {
	w.put_u16(p_ack);
	w.put_u8((uint8_t)(p_block_flags | (p_ack_bit ? CHAN_ACK_BIT : 0)));
	if (p_block_flags & CHAN_RELIABLE) {
		w.put_u16((uint16_t)p_block.size());
		w.put_data(p_block.data(), p_block.size());
	}
	w.put_u16((uint16_t)p_datagram.size());
	w.put_data(p_datagram.data(), p_datagram.size());
}

// The channel part of a packet, as read. The pointers are into the packet.
struct ChannelPart {
	uint16_t ack = 0;
	uint8_t flags = 0;
	const uint8_t *block = nullptr;
	size_t block_len = 0;
	const uint8_t *datagram = nullptr;
	size_t datagram_len = 0;

	bool ack_bit() const { return (flags & CHAN_ACK_BIT) != 0; }

	void read(Reader &r) {
		ack = r.get_u16();
		flags = r.get_u8();
		if (flags & CHAN_RELIABLE) {
			block_len = r.get_u16();
			block = r.get_data(block_len);
		}
		datagram_len = r.get_u16();
		datagram = r.get_data(datagram_len);
	}

	// The messages this packet delivers: its block's (if it completes any), then its datagram's. False
	// if either doesn't parse.
	bool take(ChannelReceiver &p_in, std::vector<std::vector<uint8_t>> &r_reliable,
			std::vector<std::vector<uint8_t>> &r_unreliable) const {
		if ((flags & CHAN_RELIABLE) && !p_in.take(flags, block, block_len, r_reliable)) {
			return false;
		}
		return split_messages(datagram, datagram_len, r_unreliable);
	}
};

} // namespace goldnet
