#pragma once
//
// Which node each net_id names, on either end. The server hands ids out, counting from 1 and never
// twice in a session, so they stay small on the wire (a byte under 128, two under 16384). A client
// learns its ids from the server's spawn and bind records, and drops them for a new session. Nodes
// are kept by ObjectID, so a freed one is never dereferenced from here.
//
#include <godot_cpp/templates/hash_map.hpp>

#include <cstdint>
#include <vector>

class NetIds {
public:
	// Server: p_obj's id, handing it the next one if it has none.
	uint32_t assign(uint64_t p_obj) {
		if (const uint32_t *id = by_obj_.getptr(p_obj)) {
			return *id;
		}
		const uint32_t id = next_++;
		put(id, p_obj, false);
		return id;
	}

	// Client: the server says p_id names p_obj. Replaces whatever either named before.
	void bind(uint32_t p_id, uint64_t p_obj) {
		drop(p_id);
		release(p_obj);
		put(p_id, p_obj, true);
	}

	// The id p_obj has, or 0; the node p_id names, or 0.
	uint32_t id(uint64_t p_obj) const {
		const uint32_t *id = by_obj_.getptr(p_obj);
		return id ? *id : 0;
	}
	uint64_t object(uint32_t p_id) const {
		const Entry *e = by_id_.getptr(p_id);
		return e ? e->obj : 0;
	}

	// p_obj has no id any more; the one it had, or 0.
	uint32_t release(uint64_t p_obj) {
		const uint32_t id = this->id(p_obj);
		drop(id);
		return id;
	}
	// p_id names nothing any more.
	void drop(uint32_t p_id) {
		if (const Entry *e = by_id_.getptr(p_id)) {
			by_obj_.erase(e->obj);
			by_id_.erase(p_id);
		}
	}

	// Client: forget every id the server gave, for a new session. A host's own, its server's, stay.
	void forget_learned() {
		std::vector<uint32_t> learned;
		for (const godot::KeyValue<uint32_t, Entry> &kv : by_id_) {
			if (kv.value.learned) {
				learned.push_back(kv.key);
			}
		}
		for (uint32_t id : learned) {
			drop(id);
		}
	}

private:
	struct Entry {
		uint64_t obj = 0;
		bool learned = false; // bound from a server's record, not handed out here
	};

	void put(uint32_t p_id, uint64_t p_obj, bool p_learned) {
		by_id_[p_id] = Entry{ p_obj, p_learned };
		by_obj_[p_obj] = p_id;
	}

	godot::HashMap<uint32_t, Entry> by_id_;
	godot::HashMap<uint64_t, uint32_t> by_obj_;
	uint32_t next_ = 1;
};
