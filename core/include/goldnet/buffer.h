#pragma once
//
// Byte buffers for the snapshot wire. Little-endian throughout, matching the StreamPeerBuffer
// default goldnet-godot used to build packets with, so the bytes on the wire are unchanged.
//
// A Reader never reads past its end. An overrun sets failed() and yields zeros from then on, and
// the caller drops the packet: a snapshot is unreliable UDP whose bytes are corruption-reachable,
// so running off the end is an input condition to handle, not a bug to assert on.
//
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace goldnet {

// IEEE 754 binary16, round-to-nearest-even, with subnormals. Standard on both sides, so any
// conforming decoder reads what this writes.
inline uint16_t float_to_half(float p_f) {
	uint32_t x;
	memcpy(&x, &p_f, sizeof(x));
	const uint16_t sign = (uint16_t)((x >> 16) & 0x8000u);
	const uint32_t exp = (x >> 23) & 0xFFu;
	uint32_t mant = x & 0x7FFFFFu;
	if (exp == 0xFFu) {
		return sign | 0x7C00u | (mant ? 0x200u : 0u); // inf stays inf, NaN stays a (quiet) NaN
	}
	const int32_t e = (int32_t)exp - 127 + 15;
	if (e >= 0x1F) {
		return sign | 0x7C00u; // too large for a half: infinity
	}
	if (e <= 0) {
		if (e < -10) {
			return sign; // below half the smallest subnormal: zero
		}
		mant |= 0x800000u; // the implicit leading 1 becomes explicit in a subnormal
		const uint32_t shift = (uint32_t)(14 - e);
		uint32_t h = mant >> shift;
		const uint32_t rem = mant & ((1u << shift) - 1u);
		const uint32_t halfway = 1u << (shift - 1);
		if (rem > halfway || (rem == halfway && (h & 1u))) {
			h++; // a carry into bit 10 is the smallest normal, which is the right answer
		}
		return sign | (uint16_t)h;
	}
	uint32_t h = ((uint32_t)e << 10) | (mant >> 13);
	const uint32_t rem = mant & 0x1FFFu;
	if (rem > 0x1000u || (rem == 0x1000u && (h & 1u))) {
		h++; // a carry into the exponent rounds up to the next binade, or to infinity
	}
	return sign | (uint16_t)h;
}

inline float half_to_float(uint16_t p_h) {
	const uint32_t sign = (uint32_t)(p_h & 0x8000u) << 16;
	const uint32_t exp = (p_h >> 10) & 0x1Fu;
	uint32_t mant = p_h & 0x3FFu;
	uint32_t x;
	if (exp == 0) {
		if (mant == 0) {
			x = sign;
		} else {
			int e = -1;
			do {
				e++;
				mant <<= 1;
			} while (!(mant & 0x400u));
			x = sign | ((uint32_t)(112 - e) << 23) | ((mant & 0x3FFu) << 13);
		}
	} else if (exp == 0x1Fu) {
		x = sign | 0x7F800000u | (mant << 13);
	} else {
		x = sign | ((exp + 112) << 23) | (mant << 13);
	}
	float f;
	memcpy(&f, &x, sizeof(f));
	return f;
}

class Writer {
public:
	void put_u8(uint8_t p_v) { bytes_.push_back(p_v); }
	void put_u16(uint16_t p_v) {
		put_u8((uint8_t)(p_v & 0xFFu));
		put_u8((uint8_t)(p_v >> 8));
	}
	void put_u32(uint32_t p_v) {
		for (int i = 0; i < 4; i++) {
			put_u8((uint8_t)(p_v >> (8 * i)));
		}
	}
	void put_float(float p_f) {
		uint32_t u;
		memcpy(&u, &p_f, sizeof(u));
		put_u32(u);
	}
	void put_half(float p_f) { put_u16(float_to_half(p_f)); }
	void put_data(const uint8_t *p_data, size_t p_size) {
		if (p_size > 0) {
			bytes_.insert(bytes_.end(), p_data, p_data + p_size);
		}
	}
	void put_data(const Writer &p_other) { put_data(p_other.data(), p_other.size()); }

	const uint8_t *data() const { return bytes_.data(); }
	size_t size() const { return bytes_.size(); }
	void clear() { bytes_.clear(); }

private:
	std::vector<uint8_t> bytes_;
};

class Reader {
public:
	Reader(const uint8_t *p_data, size_t p_size) :
			data_(p_data), size_(p_size) {}

	uint8_t get_u8() {
		if (pos_ >= size_) {
			failed_ = true;
			return 0;
		}
		return data_[pos_++];
	}
	uint16_t get_u16() {
		uint16_t lo = get_u8();
		uint16_t hi = get_u8();
		return (uint16_t)(lo | (hi << 8));
	}
	uint32_t get_u32() {
		uint32_t v = 0;
		for (int i = 0; i < 4; i++) {
			v |= (uint32_t)get_u8() << (8 * i);
		}
		return v;
	}
	float get_float() {
		uint32_t u = get_u32();
		float f;
		memcpy(&f, &u, sizeof(f));
		return f;
	}
	float get_half() { return half_to_float(get_u16()); }
	// The next p_size bytes in place, or null (and failed) if fewer remain.
	const uint8_t *get_data(size_t p_size) {
		if (p_size > size_ - pos_) {
			failed_ = true;
			pos_ = size_;
			return nullptr;
		}
		const uint8_t *p = data_ + pos_;
		pos_ += p_size;
		return p;
	}

	bool failed() const { return failed_; }
	size_t remaining() const { return size_ - pos_; }

private:
	const uint8_t *data_;
	size_t size_;
	size_t pos_ = 0;
	bool failed_ = false;
};

} // namespace goldnet
