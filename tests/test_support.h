#pragma once
//
// Shared support for the standalone tests: the CHECK macro and minimal stand-ins for the
// buffer and container types goldnet/codec.h is templated over. The container fakes are
// insertion-ordered, so the bookkeeping tests can assert on emission order.
//
// The fakes deliberately mimic only the surface codec.h uses. They are not general
// containers and shouldn't grow into one.
//
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

// Assertion with file/line reporting and a passed count, shared so the two test binaries
// can't drift into reporting failures differently. `checks` is per-translation-unit,
// which is what we want: each test is its own executable.
static int checks = 0;

#define CHECK(cond)                                                              \
	do {                                                                         \
		checks++;                                                                \
		if (!(cond)) {                                                           \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
			assert(false && #cond);                                              \
		}                                                                        \
	} while (0)

// Byte buffer with a read cursor, matching StreamPeerBuffer's put_*/get_* surface.
// Accessed through `->` in the codec, so tests pass a FakeBuf*.
struct FakeBuf {
	std::vector<uint8_t> bytes;
	size_t read_pos = 0;

	void put_u8(uint8_t v) { bytes.push_back(v); }

	uint8_t get_u8() {
		assert(read_pos < bytes.size() && "read past end of buffer");
		return bytes[read_pos++];
	}

	// StreamPeerBuffer is little-endian by default.
	void put_u16(uint16_t v) {
		bytes.push_back((uint8_t)(v & 0xFF));
		bytes.push_back((uint8_t)(v >> 8));
	}

	uint16_t get_u16() {
		assert(read_pos + 1 < bytes.size() && "read past end of buffer");
		uint16_t lo = bytes[read_pos++];
		uint16_t hi = bytes[read_pos++];
		return (uint16_t)(lo | (hi << 8));
	}

	size_t size() const { return bytes.size(); }
	void rewind() { read_pos = 0; }
	void clear() {
		bytes.clear();
		read_pos = 0;
	}
};

// Insertion-ordered set with std::unordered_set's count()/insert() surface, iterating by value.
// Ordered rather than hashed so the derived-removal tests can assert on emission order.
struct FakeSet {
	std::vector<uint32_t> items;

	size_t count(uint32_t v) const {
		for (uint32_t i : items) {
			if (i == v) {
				return 1;
			}
		}
		return 0;
	}

	void insert(uint32_t v) {
		if (!count(v)) {
			items.push_back(v);
		}
	}

	size_t size() const { return items.size(); }
	std::vector<uint32_t>::const_iterator begin() const { return items.begin(); }
	std::vector<uint32_t>::const_iterator end() const { return items.end(); }
};

