#pragma once
//
// Shared support for the standalone tests: the CHECK macro and minimal stand-ins for the
// two engine types goldnet_codec.h is templated over. godot-cpp's StreamPeerBuffer and
// HashMap both route through GDExtension function pointers that only exist inside a
// running Godot, so the real ones can't be constructed here.
//
// The fakes deliberately mimic only the surface goldnet_codec.h uses. They are not
// general containers and shouldn't grow into one — if a helper starts needing more than
// this, that's a signal the helper isn't engine-agnostic after all.
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

// Insertion-ordered map with godot HashMap's has()/erase()/operator[] surface, iterating
// as {.key, .value}. Ordered rather than hashed so test failures are reproducible.
struct FakeMap {
	struct Entry {
		uint32_t key;
		uint16_t value;
	};
	std::vector<Entry> entries;

	bool has(uint32_t k) const {
		for (const Entry &e : entries) {
			if (e.key == k) {
				return true;
			}
		}
		return false;
	}

	uint16_t &operator[](uint32_t k) {
		for (Entry &e : entries) {
			if (e.key == k) {
				return e.value;
			}
		}
		entries.push_back({ k, 0 });
		return entries.back().value;
	}

	void erase(uint32_t k) {
		for (size_t i = 0; i < entries.size(); i++) {
			if (entries[i].key == k) {
				entries.erase(entries.begin() + i);
				return;
			}
		}
	}

	size_t size() const { return entries.size(); }
	std::vector<Entry>::const_iterator begin() const { return entries.begin(); }
	std::vector<Entry>::const_iterator end() const { return entries.end(); }
};
