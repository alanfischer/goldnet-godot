#include "goldnet_multiplayer.h"
#include "goldnet_link.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <godot_cpp/classes/e_net_multiplayer_peer.hpp>
#include <godot_cpp/classes/e_net_packet_peer.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/multiplayer_spawner.hpp>
#include <godot_cpp/classes/scene_multiplayer.hpp>
#include <godot_cpp/classes/scene_replication_config.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/classes/stream_peer_buffer.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/vector3.hpp>

using namespace godot;

// Default snapshot cadence when a synchronizer reports no interval (30 Hz).
static const uint32_t DEFAULT_INTERVAL_MS = 33;

// GOLDNET_RPC_STATS' per-send overhead guess: SceneMultiplayer's command byte, node and method ids
// (about 4 bytes) plus ENet's per-command header (about 8). It is a constant, so it shifts every
// row by the same amount and never reorders them.
static const int RPC_HEADER_EST = 12;
// What a snapshot costs on the wire beyond its bytes, for the rate budget: IP + UDP (28), ENet's
// header and command (~10), and the _gn_recv RPC carrying it (~12). Close enough for a budget.
static const int SNAPSHOT_WIRE_OVERHEAD = 50;
// The same for a game RPC, whose own header rpc_wire_bytes already counts.
static const int RPC_WIRE_OVERHEAD = 38;

// An entity's cross-peer identity: a hash of the node's scene path. Server and client
// derive it the same way from the same path, so it matches without a handshake. Used for
// synchronizers (state), spawned nodes (spawn record), and spawners — must stay in sync
// everywhere, hence one helper.
static uint32_t net_id_for(Node *p_node) {
	return (uint32_t)String(p_node->get_path()).hash();
}

// Split a replication-config property path (relative to the synchronizer's root
// node, e.g. ".:position" or "NetInterp:net_pos") into the target node under
// `p_root` and the property path to get_indexed/set_indexed on it. Returns null
// if the target node can't be resolved.
// p_method's @rpc config, from the two places the engine merges: a node-level rpc_config() (an
// explicit per-node override) and the script's @rpc annotations (the common case, which
// get_node_rpc_config() does NOT surface). False if it's in neither, or the target has no script.
static bool rpc_method_config(Object *p_object, const StringName &p_method, Dictionary &r_config) {
	Node *n = Object::cast_to<Node>(p_object);
	Variant cfg = n ? n->get_node_rpc_config() : Variant();
	if (cfg.get_type() != Variant::DICTIONARY || !Dictionary(cfg).has(p_method)) {
		Ref<Script> scr = p_object->get_script();
		cfg = scr.is_valid() ? scr->get_rpc_config() : Variant();
	}
	if (cfg.get_type() != Variant::DICTIONARY || !Dictionary(cfg).has(p_method)) {
		return false;
	}
	r_config = Dictionary(cfg)[p_method];
	return true;
}

static Node *resolve_property(Node *p_root, const NodePath &p_cfg_path, NodePath &r_prop) {
	String node_path;
	int64_t nc = p_cfg_path.get_name_count();
	for (int64_t i = 0; i < nc; i++) {
		String n = p_cfg_path.get_name(i);
		if (n == ".") {
			continue;
		}
		node_path += (node_path.is_empty() ? String() : String("/")) + n;
	}
	Node *target = node_path.is_empty() ? p_root : p_root->get_node_or_null(NodePath(node_path));
	if (!target) {
		return nullptr;
	}
	String prop;
	int64_t sc = p_cfg_path.get_subname_count();
	for (int64_t i = 0; i < sc; i++) {
		prop += String(":") + p_cfg_path.get_subname(i);
	}
	r_prop = NodePath(prop);
	return target;
}

// Compact, self-delimiting tagged values (tags in goldnet/protocol.h), for RPC arguments and for a
// slot whose kind is only known at runtime (see SlotKind). Stock Variant put_var tags every value
// with a 4-byte type header and stores floats/ints at 64-bit width; these pack the common types
// behind a 1-byte tag, 32-bit floats and zig-zag varint ints, and fall back to Godot's own Variant
// encoding (TAG_OPAQUE) for anything else, so any Variant still round-trips. A "gn_quant" hint
// (see _read_quant) picks a lossy tag; the tag says which, so the decoder needs no hint.
using goldnet::QUANT_AUTO;

static int rpc_wire_bytes(const Array &p_args);

// Map a "gn_quant" hint name to its tag, or QUANT_AUTO if the name is unknown.
static uint8_t gn_quant_from_name(const String &name) {
	if (name == "angle16") {
		return goldnet::TAG_ANGLE16;
	}
	if (name == "angle8") {
		return goldnet::TAG_ANGLE8;
	}
	if (name == "half") {
		return goldnet::TAG_HALF;
	}
	if (name == "vec3_half") {
		return goldnet::TAG_VEC3_HALF;
	}
	if (name == "time_delta") {
		return goldnet::TAG_TIME_DELTA;
	}
	return QUANT_AUTO;
}

// Read an entity's priority-ordered-overflow weight from its synchronizer's "gn_priority" meta —
// a plain float (or int), not a per-slot dict like gn_quant, since importance is a property of the
// whole entity, not one of its fields. Unset, non-numeric, or non-positive all fall back to 1.0 (a
// non-positive weight would invert the staleness sort instead of just weighting it, which is never
// what's intended).
static float gn_read_priority(MultiplayerSynchronizer *p_sync) {
	const StringName meta_key("gn_priority");
	if (!p_sync->has_meta(meta_key)) {
		return 1.0f;
	}
	Variant mv = p_sync->get_meta(meta_key);
	if (mv.get_type() != Variant::FLOAT && mv.get_type() != Variant::INT) {
		return 1.0f;
	}
	float v = (float)(double)mv;
	return v > 0.0f ? v : 1.0f;
}

// A tagged value's payload, after its tag. p_now_ms is the CURRENT packet's own header timestamp —
// the reference TAG_TIME_DELTA encodes against. Every call site has one on hand (it's already in
// every snapshot header), so this needs no extra per-game config.
static void gn_put_payload(goldnet::Writer &w, uint8_t p_tag, const Variant &v, uint32_t p_now_ms) {
	using namespace goldnet;
	switch (p_tag) {
		case TAG_ANGLE16:
			put_angle16(&w, (float)(double)v);
			return;
		case TAG_ANGLE8:
			put_angle8(&w, (float)(double)v);
			return;
		case TAG_HALF:
			w.put_half((float)(double)v);
			return;
		case TAG_VEC3_HALF: {
			const Vector3 vv = v;
			w.put_half(vv.x);
			w.put_half(vv.y);
			w.put_half(vv.z);
			return;
		}
		case TAG_TIME_DELTA:
			// Zigzag varint of the signed gap to THIS packet's own send time. A stamp from the same
			// tick (the overwhelmingly common case — an entity publishes its shadow state and the
			// snapshot the instant after) encodes as ~0, one byte, forever — independent of how long
			// the server has been up, unlike the raw absolute ms value this replaces.
			put_varint(&w, (int64_t)v - (int64_t)p_now_ms);
			return;
		case TAG_FLOAT:
			w.put_float((float)(double)v);
			return;
		case TAG_INT:
			put_varint(&w, (int64_t)v);
			return;
		case TAG_VEC3: {
			const Vector3 vv = v;
			w.put_float(vv.x);
			w.put_float(vv.y);
			w.put_float(vv.z);
			return;
		}
		case TAG_BOOL:
			w.put_u8((bool)v ? 1 : 0);
			return;
		default: {
			// Godot's Variant encoding behind a u32 length — byte-for-byte what StreamPeer::put_var
			// writes, so this tag's wire format is unchanged from when packets were StreamPeerBuffers.
			PackedByteArray b = UtilityFunctions::var_to_bytes(v);
			w.put_u32((uint32_t)b.size());
			w.put_data(b.ptr(), (size_t)b.size());
			return;
		}
	}
}

// The tag a value goes behind: its hint's, or its type's when there's no hint or it can't apply.
// TAG_TIME_DELTA only fits an integer ms stamp: the decoder reconstructs an INT, so applying it to
// a float slot would silently truncate the value AND change its type on the client.
static uint8_t gn_tag_for(const Variant &v, uint8_t p_quant) {
	using namespace goldnet;
	switch (p_quant) {
		case TAG_ANGLE16:
		case TAG_ANGLE8:
		case TAG_HALF:
		case TAG_VEC3_HALF:
			return p_quant;
		case TAG_TIME_DELTA:
			if (v.get_type() == Variant::INT) {
				return p_quant;
			}
			break;
		default:
			break;
	}
	switch (v.get_type()) {
		case Variant::FLOAT:
			return TAG_FLOAT;
		case Variant::INT:
			return TAG_INT;
		case Variant::VECTOR3:
			return TAG_VEC3;
		case Variant::BOOL:
			return TAG_BOOL;
		default:
			return TAG_OPAQUE;
	}
}

static void gn_put_value(goldnet::Writer &w, const Variant &v, uint8_t p_quant, uint32_t p_now_ms) {
	const uint8_t tag = gn_tag_for(v, p_quant);
	w.put_u8(tag);
	gn_put_payload(w, tag, v, p_now_ms);
}

static Variant gn_bytes_to_var(const uint8_t *p_data, size_t p_size) {
	PackedByteArray b;
	b.resize((int64_t)p_size);
	if (p_size > 0) {
		memcpy(b.ptrw(), p_data, p_size);
	}
	return UtilityFunctions::bytes_to_var(b);
}

// p_ref_time_ms is the CURRENT packet's header timestamp (already parsed before the entity loop
// reaches this call), the same value the sender's p_now_ms was — TAG_TIME_DELTA reconstructs the
// absolute stamp by adding the wire delta back to it.
static Variant gn_get_tagged(uint8_t t, goldnet::Reader &r, uint32_t p_ref_time_ms) {
	using namespace goldnet;
	switch (t) {
		case TAG_FLOAT:
			return (double)r.get_float();
		case TAG_INT:
			return get_varint(&r);
		case TAG_VEC3: {
			float x = r.get_float();
			float y = r.get_float();
			float z = r.get_float();
			return Vector3(x, y, z);
		}
		case TAG_BOOL:
			return r.get_u8() != 0;
		case TAG_ANGLE16:
			return (double)get_angle16(&r);
		case TAG_ANGLE8:
			return (double)get_angle8(&r);
		case TAG_HALF:
			return (double)r.get_half();
		case TAG_VEC3_HALF: {
			float x = r.get_half();
			float y = r.get_half();
			float z = r.get_half();
			return Vector3(x, y, z);
		}
		case TAG_TIME_DELTA:
			return (int64_t)p_ref_time_ms + get_varint(&r);
		default: {
			uint32_t len = r.get_u32();
			const uint8_t *p = r.get_data(len);
			return p ? gn_bytes_to_var(p, len) : Variant();
		}
	}
}

static Variant gn_get_value(goldnet::Reader &r, uint32_t p_ref_time_ms) {
	const uint8_t t = r.get_u8();
	return gn_get_tagged(t, r, p_ref_time_ms);
}

// A slot's kind: how its value goes on the wire, which both ends work out the same way (see
// _make_slot_coding), so it carries no tag. A value tag (goldnet/protocol.h) means the slot goes
// whole as that tag's payload. The kinds below go as their step from the value the peer holds at
// its baseline when the core hands one over (`base`), and whole when it doesn't: an int as its
// step, an angle or a grid value as its step in quanta. SK_TAGGED is a slot whose property has no
// declared type the others fit: a tagged value, as gn_put_value writes it.
enum SlotKind : uint8_t {
	SK_INT_STEP = 32,
	SK_ANGLE16_STEP,
	SK_ANGLE8_STEP,
	SK_FLOAT_GRID,
	SK_VEC3_GRID,
	SK_TAGGED,
};

static bool slot_kind_stepped(uint8_t p_kind) { return p_kind >= SK_INT_STEP && p_kind < SK_TAGGED; }

// The shortest way round from one angle step to another, so a small turn across 0 is a small step.
static int64_t angle_step(int64_t p_to, int64_t p_from, int p_bits) {
	const int64_t full = (int64_t)1 << p_bits;
	return ((p_to - p_from + full / 2) & (full - 1)) - full / 2;
}

static int64_t grid_q(double p_v, double p_grid) {
	const double n = p_v / p_grid;
	return std::isfinite(n) ? (int64_t)std::llround(n) : 0;
}

static void put_slot(goldnet::Writer &w, uint8_t p_kind, double p_grid, uint8_t p_quant, const Variant &v,
		const Variant *p_base, uint32_t p_now_ms) {
	using namespace goldnet;
	switch (p_kind) {
		case SK_INT_STEP:
			put_varint(&w, (int64_t)((uint64_t)(int64_t)v - (uint64_t)(p_base ? (int64_t)*p_base : 0)));
			return;
		case SK_ANGLE16_STEP:
		case SK_ANGLE8_STEP: {
			const int bits = p_kind == SK_ANGLE16_STEP ? 16 : 8;
			if (!p_base) {
				gn_put_payload(w, bits == 16 ? TAG_ANGLE16 : TAG_ANGLE8, v, p_now_ms);
				return;
			}
			put_varint(&w, angle_step(angle_quanta((float)(double)v, bits), angle_quanta((float)(double)*p_base, bits), bits));
			return;
		}
		case SK_FLOAT_GRID:
			put_varint(&w, grid_q((double)v, p_grid) - (p_base ? grid_q((double)*p_base, p_grid) : 0));
			return;
		case SK_VEC3_GRID: {
			const Vector3 vv = v;
			const Vector3 bv = p_base ? (Vector3)*p_base : Vector3();
			for (int a = 0; a < 3; a++) {
				put_varint(&w, grid_q(vv[a], p_grid) - (p_base ? grid_q(bv[a], p_grid) : 0));
			}
			return;
		}
		case SK_TAGGED:
			gn_put_value(w, v, p_quant, p_now_ms);
			return;
		default:
			gn_put_payload(w, p_kind, v, p_now_ms);
			return;
	}
}

static Variant get_slot(goldnet::Reader &r, uint8_t p_kind, double p_grid, const Variant *p_base, uint32_t p_ref_time_ms) {
	using namespace goldnet;
	switch (p_kind) {
		case SK_INT_STEP:
			return (int64_t)((uint64_t)(p_base ? (int64_t)*p_base : 0) + (uint64_t)get_varint(&r));
		case SK_ANGLE16_STEP:
		case SK_ANGLE8_STEP: {
			const int bits = p_kind == SK_ANGLE16_STEP ? 16 : 8;
			if (!p_base) {
				return gn_get_tagged(bits == 16 ? TAG_ANGLE16 : TAG_ANGLE8, r, p_ref_time_ms);
			}
			const int64_t full = (int64_t)1 << bits;
			const int64_t q = (angle_quanta((float)(double)*p_base, bits) + get_varint(&r)) & (full - 1);
			return (double)(((float)q / (float)full) * TAU); // as get_angle16 / get_angle8 decode it
		}
		case SK_FLOAT_GRID:
			return (double)((p_base ? grid_q((double)*p_base, p_grid) : 0) + get_varint(&r)) * p_grid;
		case SK_VEC3_GRID: {
			const Vector3 bv = p_base ? (Vector3)*p_base : Vector3();
			Vector3 out;
			for (int a = 0; a < 3; a++) {
				out[a] = (real_t)((double)((p_base ? grid_q(bv[a], p_grid) : 0) + get_varint(&r)) * p_grid);
			}
			return out;
		}
		case SK_TAGGED:
			return gn_get_value(r, p_ref_time_ms);
		default:
			return gn_get_tagged(p_kind, r, p_ref_time_ms);
	}
}

// An RPC's arguments in a message (see _send_message): the snapshot's value tags where they fit, and
// the binding's own for the types an RPC passes most that they don't carry compactly.
static const uint8_t ARG_NIL = 64;
static const uint8_t ARG_STRING = 65;
static const uint8_t ARG_STRING_NAME = 66;
static const uint8_t ARG_BYTES = 67;
static const uint8_t ARG_DOUBLE = 68; // a float an f32 would change; the inner keeps those exact too

// An unreliable call bigger than this goes through the inner: a datagram is all or nothing, and one
// call this size would crowd out the rest of the packet's.
static const int MAX_UNRELIABLE_MESSAGE = 512;

static void put_str(goldnet::Writer &w, const String &p_s) {
	const CharString u = p_s.utf8();
	goldnet::put_uvarint(&w, (uint32_t)u.length());
	w.put_data((const uint8_t *)u.get_data(), (size_t)u.length());
}

static String get_str(goldnet::Reader &r) {
	const uint32_t n = goldnet::get_uvarint(&r);
	const uint8_t *p = r.get_data(n);
	return p ? String::utf8((const char *)p, n) : String();
}

static void put_arg(goldnet::Writer &w, const Variant &v) {
	switch (v.get_type()) {
		case Variant::NIL:
			w.put_u8(ARG_NIL);
			return;
		case Variant::STRING:
			w.put_u8(ARG_STRING);
			put_str(w, v);
			return;
		case Variant::STRING_NAME:
			w.put_u8(ARG_STRING_NAME);
			put_str(w, String(StringName(v)));
			return;
		case Variant::PACKED_BYTE_ARRAY: {
			const PackedByteArray b = v;
			w.put_u8(ARG_BYTES);
			goldnet::put_uvarint(&w, (uint32_t)b.size());
			w.put_data(b.ptr(), (size_t)b.size());
			return;
		}
		case Variant::FLOAT: {
			const double d = v;
			if ((double)(float)d == d) {
				break;
			}
			uint64_t u;
			memcpy(&u, &d, sizeof(u));
			w.put_u8(ARG_DOUBLE);
			w.put_u32((uint32_t)u);
			w.put_u32((uint32_t)(u >> 32));
			return;
		}
		default:
			break;
	}
	gn_put_value(w, v, goldnet::QUANT_AUTO, 0);
}

static Variant get_arg(goldnet::Reader &r) {
	const uint8_t t = r.get_u8();
	switch (t) {
		case ARG_NIL:
			return Variant();
		case ARG_STRING:
			return get_str(r);
		case ARG_STRING_NAME:
			return StringName(get_str(r));
		case ARG_BYTES: {
			const uint32_t n = goldnet::get_uvarint(&r);
			const uint8_t *p = r.get_data(n);
			PackedByteArray b;
			if (p) {
				b.resize(n);
				memcpy(b.ptrw(), p, n);
			}
			return b;
		}
		case ARG_DOUBLE: {
			const uint64_t lo = r.get_u32();
			const uint64_t hi = r.get_u32();
			const uint64_t u = lo | (hi << 32);
			double d;
			memcpy(&d, &u, sizeof(d));
			return d;
		}
		default:
			return gn_get_tagged(t, r, 0);
	}
}

GoldNetMultiplayer::GoldNetMultiplayer() {
	dbg = getenv("GOLDNET_DEBUG") != nullptr;
	rpc_stats = getenv("GOLDNET_RPC_STATS") != nullptr;
	{
		const char *loss = getenv("GOLDNET_LOSS");
		sim.set_loss_percent(loss ? atoi(loss) : 0); // via the setter, so the 0-100 clamp applies here too
		// GOLDNET_SIM_SEED=<n> makes the whole sim (loss rolls + latency draws) reproducible
		// run-to-run; without it the sim stays nondeterministic.
		const char *seed = getenv("GOLDNET_SIM_SEED");
		sim.set_seed(seed ? atoi(seed) : 0);
	}
	{
		// GOLDNET_LATENCY=min,max (per-leg ms range) or a single fixed value; GOLDNET_SPIKE=ms,interval,duration.
		// These let a headless server simulate its own send leg with no admin console.
		const char *lat = getenv("GOLDNET_LATENCY");
		if (lat) {
			PackedStringArray parts = String(lat).split(",");
			if (parts.size() >= 2) {
				sim.set_latency_min_ms(parts[0].to_int());
				sim.set_latency_max_ms(parts[1].to_int());
			} else if (parts.size() == 1 && !parts[0].is_empty()) {
				sim.set_latency_min_ms(parts[0].to_int());
				sim.set_latency_max_ms(parts[0].to_int());
			}
		}
		// GOLDNET_LINK=bytes_per_sec[,queue_ms]: a slow link (a modem, BLE) on this endpoint's sends.
		const char *link = getenv("GOLDNET_LINK");
		if (link) {
			PackedStringArray parts = String(link).split(",");
			if (parts.size() >= 1 && !parts[0].is_empty()) {
				sim.set_bandwidth_bps(parts[0].to_int());
			}
			if (parts.size() >= 2) {
				sim.set_queue_ms(parts[1].to_int());
			}
		}
		const char *spike = getenv("GOLDNET_SPIKE");
		if (spike) {
			PackedStringArray parts = String(spike).split(",");
			if (parts.size() >= 1 && !parts[0].is_empty()) {
				sim.set_spike_ms(parts[0].to_int());
			}
			if (parts.size() >= 2) {
				sim.set_spike_interval_s((float)parts[1].to_float());
			}
			if (parts.size() >= 3) {
				sim.set_spike_duration_s((float)parts[2].to_float());
			}
		}
	}
	server.set_packet_overhead(SNAPSHOT_WIRE_OVERHEAD);
	client.set_packet_overhead(SNAPSHOT_WIRE_OVERHEAD);
	_refresh_interval();
	inner = MultiplayerAPI::create_default_interface(); // a SceneMultiplayer

	SceneMultiplayer *sm = Object::cast_to<SceneMultiplayer>(inner.ptr());
	if (sm) {
		sm->set_root_path(NodePath("/root"));
	}

	inner->connect("peer_connected", callable_mp(this, &GoldNetMultiplayer::_relay_peer_connected));
	inner->connect("peer_disconnected", callable_mp(this, &GoldNetMultiplayer::_relay_peer_disconnected));
	inner->connect("connected_to_server", callable_mp(this, &GoldNetMultiplayer::_relay_connected_to_server));
	inner->connect("connection_failed", callable_mp(this, &GoldNetMultiplayer::_relay_connection_failed));
	inner->connect("server_disconnected", callable_mp(this, &GoldNetMultiplayer::_relay_server_disconnected));
}

GoldNetMultiplayer::~GoldNetMultiplayer() {}

void GoldNetMultiplayer::_reset_client_state() {
	client.reset();
	client_spawned.clear(); // spawner nodes are torn down with the session
	client_slot_codings.clear();
	warned_kinds.clear();
	update_rate_granted = -1;
	client.set_auto_command_rate(client.auto_command_rate(), (double)Engine::get_singleton()->get_physics_ticks_per_second());
	msg_registered.clear();
	msg_targets.clear();
}

// ENet throttles a peer's unreliable sends on its own: when the round trip jumps, as it does under
// a join's reliable burst, it backs off and drops nearly every unreliable packet, then takes tens
// of seconds to come back. That is the snapshot stream, which keeps its own rate and choke and
// can't see what ENet threw away, so the throttle is held where it starts. The setting travels to
// the remote end too, so the server's call alone covers both directions.
static void hold_enet_throttle(const Ref<MultiplayerPeer> &p_peer, int32_t p_id) {
	ENetMultiplayerPeer *enet = Object::cast_to<ENetMultiplayerPeer>(p_peer.ptr());
	if (!enet || enet->get_unique_id() != 1) {
		return; // a client only links to the server; its other peers are relayed, not ENet peers
	}
	Ref<ENetPacketPeer> pp = enet->get_peer(p_id);
	if (pp.is_valid()) {
		pp->throttle_configure(5000, 2, 0); // ENet's default interval and acceleration; never decelerate
	}
}

// Everything kept for one peer goes with it.
void GoldNetMultiplayer::_forget_peer(int32_t p_id) {
	server.remove_peer(p_id); // its send history, and its place in any pending despawn
	msg_registered.erase(p_id);
	msg_targets.erase(p_id);
	sim.forget_link(p_id);
}

// A hosted session ended, and its peers with it. SceneMultiplayer drops them without a
// peer_disconnected for any: when the host's own peer closes, which it reports as
// server_disconnected, and in silence when the peer is swapped.
void GoldNetMultiplayer::_end_server_session() {
	const std::vector<int32_t> peers = server.peers(); // a copy: _forget_peer edits the list
	for (int32_t peer : peers) {
		_forget_peer(peer);
	}
}

void GoldNetMultiplayer::_relay_peer_connected(int64_t p_id) {
	hold_enet_throttle(_get_multiplayer_peer(), (int32_t)p_id);
	if (inner->get_unique_id() == 1) {
		server.add_peer((int32_t)p_id); // before the game hears of it, so its handlers can send to it
	}
	emit_signal("peer_connected", p_id);
}
void GoldNetMultiplayer::_relay_peer_disconnected(int64_t p_id) {
	_forget_peer((int32_t)p_id);
	emit_signal("peer_disconnected", p_id);
}
void GoldNetMultiplayer::_relay_connected_to_server() {
	_reset_client_state(); // fresh session — old baselines are meaningless
	emit_signal("connected_to_server");
}
void GoldNetMultiplayer::_relay_connection_failed() { emit_signal("connection_failed"); }
void GoldNetMultiplayer::_relay_server_disconnected() {
	_end_server_session(); // a host's own peer closing comes here too
	_reset_client_state();
	sim.forget_link(1);
	emit_signal("server_disconnected");
}

// A property we replicate: Godot's ALWAYS (sync) and ON_CHANGE (watch) both describe
// per-tick state, and the snapshot serves both the same way — a slot equal to the peer's
// acked baseline is skipped, which is exactly what ON_CHANGE asks for. Reading only the
// sync flag made ON_CHANGE properties invisible in a MIXED config: _should_intercept saw
// the ALWAYS property and claimed the synchronizer, then the slot walk dropped the
// ON_CHANGE ones — so neither goldnet nor the inner replicated them, silently. (Both
// flags are false for NEVER, which is spawn-only and stays out.)
static bool is_replicated_slot(const Ref<SceneReplicationConfig> &p_cfg, const NodePath &p_path) {
	return p_cfg->property_get_sync(p_path) || p_cfg->property_get_watch(p_path);
}

// We own every synchronizer that carries at least one per-tick property. Since
// Phase 3 also owns the MultiplayerSpawners, there is no longer a map-static vs.
// spawner-managed split — movers, projectiles, and players all stream their state
// through the same delta path. (Spawn-only configs, if any, carry no per-tick state
// and are left alone.)
bool GoldNetMultiplayer::_should_intercept(MultiplayerSynchronizer *p_sync) const {
	Ref<SceneReplicationConfig> cfg = p_sync->get_replication_config();
	if (cfg.is_null()) {
		return false;
	}
	TypedArray<NodePath> props = cfg->get_properties();
	for (int i = 0; i < props.size(); i++) {
		if (is_replicated_slot(cfg, props[i])) {
			return true;
		}
	}
	return false;
}

// Find (or lazily create) the snapshot carrier at /root/__GoldNetLink. Stateless —
// resolved by path each call so a freed/reloaded node is never dereferenced. The
// node lives under the Window root, which outlives map scene changes.
GoldNetLink *GoldNetMultiplayer::_ensure_link() {
	SceneTree *tree = Object::cast_to<SceneTree>(Engine::get_singleton()->get_main_loop());
	if (!tree) {
		return nullptr;
	}
	Window *root = tree->get_root();
	if (!root) {
		return nullptr;
	}
	// Catch spawners already in the tree and every one added afterward, so their spawn_function is
	// wrapped before any spawn() call. First of three arming points: here (our first poll), again
	// when a peer is set, and finally the on-the-spot wrap in _object_configuration_add for a spawn
	// that beat both. A consumer never has to arm it by hand.
	capture_spawners();
	GoldNetLink *l = Object::cast_to<GoldNetLink>(root->get_node_or_null(NodePath("__GoldNetLink")));
	if (!l) {
		l = memnew(GoldNetLink);
		l->set_name("__GoldNetLink");
		root->add_child(l);
		l->set_multiplayer_authority(1);
		Dictionary rpc_cfg;
		rpc_cfg["rpc_mode"] = MultiplayerAPI::RPC_MODE_ANY_PEER;
		rpc_cfg["transfer_mode"] = MultiplayerPeer::TRANSFER_MODE_UNRELIABLE;
		rpc_cfg["call_local"] = false;
		rpc_cfg["channel"] = 0;
		l->rpc_config("_gn_recv", rpc_cfg);

		// Client → server uplink: the snapshot ack and any commands. Unreliable, like the snapshot:
		// each packet repeats the newest ack and every unacked command it can, so a lost one heals.
		l->rpc_config("_gn_up", rpc_cfg);
	}
	return l;
}

// Resolve a synchronizer from its stable ObjectID, returning null if it has been
// freed (godot-cpp has no is_instance_valid; ObjectDB::get_instance does the check).
static MultiplayerSynchronizer *sync_from_objid(uint64_t p_objid) {
	return Object::cast_to<MultiplayerSynchronizer>(ObjectDB::get_instance(p_objid));
}

// Collect the instance ids of every MultiplayerSynchronizer at or under p_node (used to gate a spawn
// on the entity's per-peer visibility — see goldnet::Server::SpawnRecord::vis_keys).
static void collect_syncs(Node *p_node, std::vector<uint64_t> &r_out) {
	if (Object::cast_to<MultiplayerSynchronizer>(p_node)) {
		r_out.push_back(p_node->get_instance_id());
	}
	TypedArray<Node> children = p_node->get_children();
	for (int i = 0; i < children.size(); i++) {
		Node *c = Object::cast_to<Node>((Object *)children[i]);
		if (c) {
			collect_syncs(c, r_out);
		}
	}
}

// --- Phase 3: spawn / despawn ---

// Replace a spawner's spawn_function with our trampoline, so a server spawn(data) hands us
// the reconstruction data. We must do this BEFORE the first spawn — the spawn itself is the
// first time object_configuration_add reveals the spawner, which is too late — so we hook
// SceneTree.node_added (and a one-time scan for spawners already in the tree). Idempotent.
void GoldNetMultiplayer::_wrap_spawner(MultiplayerSpawner *p_spawner) {
	if (!p_spawner) {
		return;
	}
	uint64_t objid = p_spawner->get_instance_id();
	if (spawners.has(objid)) {
		return;
	}
	SpawnerEntry e;
	e.net_id = net_id_for(p_spawner);
	e.orig_fn = p_spawner->get_spawn_function();
	spawners[objid] = e;
	spawner_netid_to_objid[e.net_id] = objid;
	p_spawner->set_spawn_function(
			callable_mp(this, &GoldNetMultiplayer::_spawn_trampoline).bind((int64_t)objid));
}

void GoldNetMultiplayer::_on_node_added(Node *p_node) {
	MultiplayerSpawner *sp = Object::cast_to<MultiplayerSpawner>(p_node);
	if (sp) {
		_wrap_spawner(sp);
	}
}

// Arm spawner capture: wrap every spawner currently in the tree and connect node_added so future ones
// are wrapped on entry. Idempotent, and private — the engine never announces a spawner (only
// synchronizers self-register), so this scan plus node_added is the only way to find them. Armed from
// our first poll and from _set_multiplayer_peer; a spawn that still beats both is caught and the
// spawner wrapped in _object_configuration_add, at the cost of that one node.
void GoldNetMultiplayer::capture_spawners() {
	if (spawners_scanned) {
		return;
	}
	SceneTree *tree = Object::cast_to<SceneTree>(Engine::get_singleton()->get_main_loop());
	if (!tree || !tree->get_root()) {
		return; // tree not ready yet — the first poll will retry
	}
	spawners_scanned = true;
	// node_added covers every spawner from here on; the sweep below covers the ones already in.
	tree->connect("node_added", callable_mp(this, &GoldNetMultiplayer::_on_node_added));
	TypedArray<Node> found = tree->get_root()->find_children("*", "MultiplayerSpawner", true, false);
	for (int i = 0; i < found.size(); i++) {
		_wrap_spawner(Object::cast_to<MultiplayerSpawner>(found[i]));
	}
}

// The wrapper installed over each spawner's spawn_function. On the server, spawn(data)
// calls this; we run the game's real function to build the node, stash the reconstruction
// data (the node isn't in the tree yet — _drain_pending_spawns promotes it once it is), and
// return the node so spawn() proceeds normally.
Variant GoldNetMultiplayer::_spawn_trampoline(Variant p_data, int64_t p_spawner_objid) {
	uint64_t sobj = (uint64_t)p_spawner_objid;
	Variant node_v;
	if (spawners.has(sobj)) {
		node_v = spawners[sobj].orig_fn.callv(Array::make(p_data));
	}
	Node *node = Object::cast_to<Node>(node_v);
	if (node) {
		PendingSpawn ps;
		ps.spawner_objid = sobj;
		ps.data = p_data;
		pending_spawns[node->get_instance_id()] = ps;
	}
	return node_v;
}

// Server: promote captured spawns to core spawn records once the node is in the tree (so its
// path — and thus net_id — is stable). The recipe is Variant-encoded once, here, rather than per send.
void GoldNetMultiplayer::_drain_pending_spawns() {
	if (pending_spawns.is_empty()) {
		return;
	}
	Vector<uint64_t> done;
	for (const KeyValue<uint64_t, PendingSpawn> &kv : pending_spawns) {
		Node *node = Object::cast_to<Node>(ObjectDB::get_instance(kv.key));
		if (!node) {
			done.push_back(kv.key); // freed before it entered the tree
			continue;
		}
		if (!node->is_inside_tree()) {
			continue; // wait for spawn() to add it
		}
		CoreServer::SpawnRecord rec;
		rec.spawner_net_id = spawners.has(kv.value.spawner_objid) ? spawners[kv.value.spawner_objid].net_id : 0;
		rec.node_key = kv.key;
		PackedByteArray data = UtilityFunctions::var_to_bytes(kv.value.data);
		rec.data.assign(data.ptr(), data.ptr() + data.size());
		collect_syncs(node, rec.vis_keys); // for lazy, visibility-gated spawning
		server.add_spawn(net_id_for(node), std::move(rec));
		done.push_back(kv.key);
	}
	for (int i = 0; i < done.size(); i++) {
		pending_spawns.erase(done[i]);
	}
}

// Server: a spawned node that has been freed becomes a despawn (see goldnet::Server::despawn).
void GoldNetMultiplayer::_detect_despawns() {
	std::vector<uint32_t> gone;
	for (const auto &kv : server.spawns()) {
		if (!ObjectDB::get_instance(kv.second.node_key)) {
			gone.push_back(kv.first);
		}
	}
	for (uint32_t net_id : gone) {
		server.despawn(net_id);
	}
}

// Client: build a spawned node from its recipe, add it under the spawner's spawn_path, and
// fire the spawner's `spawned` signal so the game runs its per-node bookkeeping. The node's
// child synchronizer auto-registers on enter-tree and streams state like any other entity.
// False when it can't be built yet; goldnet::Client then takes a later resend.
bool GoldNetMultiplayer::_apply_spawn(uint32_t p_net_id, uint32_t p_spawner_net_id, const Variant &p_data) {
	if (!spawner_netid_to_objid.has(p_spawner_net_id)) {
		return false; // spawner not registered here yet
	}
	uint64_t sobj = spawner_netid_to_objid[p_spawner_net_id];
	MultiplayerSpawner *spawner = Object::cast_to<MultiplayerSpawner>(ObjectDB::get_instance(sobj));
	if (!spawner || !spawners.has(sobj)) {
		return false;
	}
	Node *spawn_root = spawner->get_node_or_null(spawner->get_spawn_path());
	if (!spawn_root) {
		return false;
	}
	Variant node_v = spawners[sobj].orig_fn.callv(Array::make(p_data));
	Node *node = Object::cast_to<Node>(node_v);
	if (!node) {
		return false;
	}
	spawn_root->add_child(node);
	client_spawned[p_net_id] = node->get_instance_id();
	spawner->emit_signal("spawned", node);
	return true;
}

// Client: free a spawned node and fire the spawner's `despawned` signal.
void GoldNetMultiplayer::_apply_despawn(uint32_t p_net_id) {
	if (!client_spawned.has(p_net_id)) {
		return;
	}
	uint64_t objid = client_spawned[p_net_id];
	client_spawned.erase(p_net_id);
	Node *node = Object::cast_to<Node>(ObjectDB::get_instance(objid));
	if (!node) {
		return;
	}
	Node *parent = node->get_parent();
	for (const KeyValue<uint64_t, SpawnerEntry> &kv : spawners) {
		MultiplayerSpawner *s = Object::cast_to<MultiplayerSpawner>(ObjectDB::get_instance(kv.key));
		if (s && s->get_node_or_null(s->get_spawn_path()) == parent) {
			s->emit_signal("despawned", node);
			break;
		}
	}
	node->queue_free();
}

// The send cadence in MICROSECONDS, so the synchronizers' interval (a double, in seconds) survives
// at full precision: whole milliseconds would turn 60 Hz into 62.5 or 58.8.
uint64_t GoldNetMultiplayer::_min_interval_us() const {
	if (snapshot_rate_setting > 0.0) {
		return std::max<uint64_t>(1, (uint64_t)(1000000.0 / snapshot_rate_setting + 0.5));
	}
	uint64_t best = 0;
	for (const KeyValue<uint64_t, uint32_t> &kv : owned) {
		MultiplayerSynchronizer *s = sync_from_objid(kv.key);
		if (!s) {
			continue;
		}
		double sec = s->get_replication_interval();
		uint64_t us = sec > 0.0 ? (uint64_t)(sec * 1000000.0 + 0.5) : (uint64_t)DEFAULT_INTERVAL_MS * 1000ULL;
		if (us == 0) {
			us = 1;
		}
		if (best == 0 || us < best) {
			best = us;
		}
	}
	return best == 0 ? (uint64_t)DEFAULT_INTERVAL_MS * 1000ULL : best;
}

void GoldNetMultiplayer::_refresh_interval() {
	cached_min_interval_us = _min_interval_us();
	server.set_snapshot_rate(snapshot_rate_setting > 0.0 ? snapshot_rate_setting : 1000000.0 / (double)cached_min_interval_us);
}

// --- Config surface (Phase 5) ---

void GoldNetMultiplayer::set_snapshot_rate(double p_hz) {
	snapshot_rate_setting = p_hz > 0.0 ? p_hz : 0.0;
	_refresh_interval();
}
double GoldNetMultiplayer::get_snapshot_rate() const {
	return snapshot_rate_setting;
}

void GoldNetMultiplayer::set_update_rate(int p_hz) {
	client.set_update_rate(p_hz > 0 ? (uint32_t)p_hz : 0);
}
int GoldNetMultiplayer::get_update_rate() const {
	return (int)client.update_rate();
}

int GoldNetMultiplayer::get_protocol_version() const {
	return goldnet::WIRE_VERSION;
}

void GoldNetMultiplayer::set_manual_send(bool p_manual) {
	manual_send = p_manual;
}
bool GoldNetMultiplayer::get_manual_send() const {
	return manual_send;
}

void GoldNetMultiplayer::send_snapshots() {
	if (inner->get_multiplayer_peer().is_null() || inner->get_unique_id() != 1 || inner->get_peers().is_empty()) {
		return;
	}
	_drain_pending_spawns(); // this tick's spawns ride this tick's snapshot
	_detect_despawns();
	_server_tick();
}

void GoldNetMultiplayer::set_rate(int p_bps) {
	client.set_rate(p_bps > 0 ? (uint32_t)p_bps : 0);
}
int GoldNetMultiplayer::get_rate() const {
	return (int)client.rate();
}
void GoldNetMultiplayer::set_auto_rate(bool p_enabled) {
	client.set_auto_rate(p_enabled);
}
bool GoldNetMultiplayer::get_auto_rate() const {
	return client.auto_rate();
}

// Charge a game RPC to each recipient's rate (see goldnet::Server::peer_rate). Its size is only
// worked out when some recipient has a rate, so an unrated server pays nothing for this.
void GoldNetMultiplayer::_charge_rpc(int32_t p_peer, const Array &p_args) {
	if (inner->get_unique_id() != 1) {
		return;
	}
	PackedInt32Array targets;
	if (p_peer > 0) {
		targets.push_back(p_peer);
	} else {
		PackedInt32Array peers = inner->get_peers();
		for (int i = 0; i < peers.size(); i++) {
			if (!(p_peer < 0 && peers[i] == -p_peer)) {
				targets.push_back(peers[i]);
			}
		}
	}
	int bytes = -1;
	uint32_t now = (uint32_t)Time::get_singleton()->get_ticks_msec();
	for (int i = 0; i < targets.size(); i++) {
		if (server.peer_stats(targets[i]).rate == 0) {
			continue;
		}
		if (bytes < 0) {
			bytes = rpc_wire_bytes(p_args) + RPC_WIRE_OVERHEAD;
		}
		server.charge(targets[i], now, (size_t)bytes);
	}
}


// Replace, not merge: every key absent from p_cfg goes back to its default, so `sim_config = {}`
// turns the whole simulation off and a read-back always matches what was written. Callers that mean
// to change one knob read, modify and write (see WizardWars' /sim_* commands) — which is only safe
// because every setter below is idempotent.
template <class S>
static bool apply_sim_config(S &r_sim, const Dictionary &p_cfg, const char *p_property) {
	// Validate every key BEFORE applying any, so a typo rejects the whole write instead of half
	// configuring the sim. Unbinding the seven per-knob properties took GDScript's own typo check
	// away (an invalid property assignment used to be a runtime error); this replaces it. Without
	// it, `sim_config = {"los_percent": 100}` would silently reset the sim to defaults instead.
	const Array keys = p_cfg.keys();
	for (int i = 0; i < keys.size(); i++) {
		const String k = keys[i];
		if (k != "loss_percent" && k != "seed" && k != "latency_min_ms" && k != "latency_max_ms" &&
				k != "spike_ms" && k != "spike_interval_s" && k != "spike_duration_s" && k != "link_bps" &&
				k != "link_queue_ms" && k != "link_overhead") {
			ERR_FAIL_V_MSG(false, String("goldnet: unknown ") + p_property + " key '" + k +
					"'. Valid keys: loss_percent, seed, latency_min_ms, latency_max_ms, spike_ms, "
					"spike_interval_s, spike_duration_s, link_bps, link_queue_ms, link_overhead.");
		}
	}
	r_sim.set_loss_percent((int)p_cfg.get("loss_percent", S::LOSS_DEFAULT_PERCENT));
	r_sim.set_seed((int)p_cfg.get("seed", S::SEED_DEFAULT));
	r_sim.set_latency_min_ms((int)p_cfg.get("latency_min_ms", S::LATENCY_MIN_DEFAULT_MS));
	r_sim.set_latency_max_ms((int)p_cfg.get("latency_max_ms", S::LATENCY_MAX_DEFAULT_MS));
	r_sim.set_spike_ms((int)p_cfg.get("spike_ms", S::SPIKE_DEFAULT_MS));
	r_sim.set_spike_interval_s((float)p_cfg.get("spike_interval_s", S::SPIKE_INTERVAL_DEFAULT_S));
	r_sim.set_spike_duration_s((float)p_cfg.get("spike_duration_s", S::SPIKE_DURATION_DEFAULT_S));
	r_sim.set_bandwidth_bps((int)p_cfg.get("link_bps", S::BANDWIDTH_DEFAULT_BPS));
	r_sim.set_queue_ms((int)p_cfg.get("link_queue_ms", S::QUEUE_DEFAULT_MS));
	r_sim.set_packet_overhead((int)p_cfg.get("link_overhead", S::PACKET_OVERHEAD_DEFAULT));
	return true;
}

template <class S>
static Dictionary read_sim_config(const S &p_sim) {
	Dictionary d;
	d["loss_percent"] = p_sim.loss_percent();
	d["seed"] = p_sim.seed();
	d["latency_min_ms"] = p_sim.latency_min_ms();
	d["latency_max_ms"] = p_sim.latency_max_ms();
	d["spike_ms"] = p_sim.spike_ms();
	d["spike_interval_s"] = p_sim.spike_interval_s();
	d["spike_duration_s"] = p_sim.spike_duration_s();
	d["link_bps"] = p_sim.bandwidth_bps();
	d["link_queue_ms"] = p_sim.queue_ms();
	d["link_overhead"] = p_sim.packet_overhead();
	return d;
}

void GoldNetMultiplayer::set_sim_config(const Dictionary &p_cfg) {
	apply_sim_config(sim, p_cfg, "sim_config");
}

Dictionary GoldNetMultiplayer::get_sim_config() const {
	return read_sim_config(sim);
}

void GoldNetMultiplayer::set_recv_sim_config(const Dictionary &p_cfg) {
	if (apply_sim_config(recv_sim, p_cfg, "recv_sim_config") && recv_peer.is_valid()) {
		apply_sim_config(recv_peer->get_sim(), p_cfg, "recv_sim_config");
	}
}

Dictionary GoldNetMultiplayer::get_recv_sim_config() const {
	return read_sim_config(recv_sim);
}

Dictionary GoldNetMultiplayer::get_peer_stats(int p_peer) const {
	const bool on_server = inner->get_multiplayer_peer().is_valid() && inner->get_unique_id() == 1;
	const bool sim_peer = !on_server && recv_peer.is_valid();
	// Only one end's stats apply; the other's stay at their zeros.
	CoreServer::PeerStats sv;
	goldnet::Client<Variant>::Stats cl;
	if (on_server) {
		sv = server.peer_stats(p_peer);
	} else {
		cl = client.stats();
	}
	Dictionary d;
	d["chokes"] = (int64_t)sv.chokes;
	d["snapshots_received"] = (int64_t)cl.snapshots_received;
	d["snapshots_lost"] = (int64_t)cl.snapshots_lost;
	d["messages_queued"] = (int64_t)(on_server ? sv.messages_queued : cl.messages_queued);
	d["messages_delivered"] = (int64_t)(on_server ? sv.messages_delivered : cl.messages_delivered);
	d["commands_pending"] = (int64_t)cl.commands_pending;
	d["commands_dropped"] = (int64_t)cl.commands_dropped;
	d["command_rtt_ms"] = (int64_t)cl.command_rtt_ms;
	d["recv_sim_bytes"] = sim_peer ? recv_peer->get_delivered_bytes() : (int64_t)0;
	d["recv_sim_packets"] = sim_peer ? recv_peer->get_delivered_packets() : (int64_t)0;
	d["rate"] = (int64_t)(on_server ? sv.rate : cl.rate);
	d["update_rate"] = (int64_t)(on_server ? sv.update_rate : std::max(cl.update_rate_granted, 0));
	d["command_rate"] = cl.command_interval_ms == 0 ? 0.0 : 1000.0 / (double)cl.command_interval_ms;
	d["downlink_lag_ms"] = (int64_t)cl.downlink_lag_ms;
	d["uplink_lag_ms"] = (int64_t)(on_server ? sv.uplink_lag_ms : cl.uplink_lag_ms);
	d["uplink_pps"] = (int64_t)(on_server ? sv.uplink_pps : cl.uplink_pps);
	d["delivered_bps"] = cl.delivered_bps;
	return d;
}

void GoldNetMultiplayer::set_relevance_events(bool p_enabled) {
	server.set_relevance_events(p_enabled);
}
bool GoldNetMultiplayer::get_relevance_events() const {
	return server.relevance_events();
}

// An RPC's size on the wire, for the simulated link: SceneMultiplayer's command byte and node and
// method ids, then each argument. Bools and small ints pack into a byte or a few; anything else is
// taken at its Variant encoding, which is close (and errs a few bytes high). An estimate — the
// link sim needs a size, not an exact one.
static int rpc_wire_bytes(const Array &p_args) {
	int n = 4;
	for (int i = 0; i < p_args.size(); i++) {
		const Variant &v = p_args[i];
		switch (v.get_type()) {
			case Variant::NIL:
			case Variant::BOOL:
				n += 1;
				break;
			case Variant::INT: {
				int64_t x = v;
				n += 1 + (x >= -128 && x <= 127 ? 1 : (x >= -32768 && x <= 32767 ? 2 : (x >= INT32_MIN && x <= INT32_MAX ? 4 : 8)));
			} break;
			case Variant::PACKED_BYTE_ARRAY:
				n += 8 + (int)PackedByteArray(v).size();
				break;
			default:
				n += (int)UtilityFunctions::var_to_bytes(v).size();
				break;
		}
	}
	return n;
}

// Shared tail of both send funnels: this send's place on the simulated link and its latency/spike,
// then send now, queue it, or drop it (a droppable send to a link whose buffer is full). Under a
// link cap a broadcast is split per peer, since each peer's link is its own.
Error GoldNetMultiplayer::_sim_send(int32_t p_peer, Object *p_object, const StringName &p_method, const Array &p_args,
		bool p_droppable, int p_bytes) {
	if (sim.bandwidth_bps() > 0 && p_peer <= 0) {
		PackedInt32Array peers = inner->get_peers();
		for (int i = 0; i < peers.size(); i++) {
			if (p_peer < 0 && peers[i] == -p_peer) {
				continue;
			}
			_sim_send(peers[i], p_object, p_method, p_args, p_droppable, p_bytes);
		}
		return OK;
	}
	uint64_t now = Time::get_singleton()->get_ticks_msec();
	if (sim.bandwidth_bps() > 0 && p_bytes < 0) {
		p_bytes = rpc_wire_bytes(p_args);
	}
	int d = sim.schedule(now, p_peer, p_bytes > 0 ? (size_t)p_bytes : 0, p_droppable);
	if (d < 0) {
		return OK; // the link's buffer is full: dropped, as a real one would
	}
	if (d == 0) {
		return inner->rpc(p_peer, p_object, p_method, p_args);
	}
	PendingSend ps;
	ps.peer = p_peer;
	ps.object_id = p_object->get_instance_id();
	ps.method = p_method;
	ps.args = p_args;
	sim.enqueue(now, p_peer, d, ps);
	return OK;
}

// Resolve whether an @rpc method is unreliable (loss-droppable), caching per (object, method). The
// mode lives in two places the engine merges: node-level runtime rpc_config() (an explicit per-node
// override) and the script's @rpc annotation config (the common case — WizardWars uses only these,
// and get_node_rpc_config() does NOT surface them, so the script lookup is essential). A method in
// neither, or a scriptless target, is treated as reliable → never dropped (safe default).
bool GoldNetMultiplayer::_rpc_is_unreliable(Object *p_object, const StringName &p_method) {
	uint64_t oid = p_object->get_instance_id();
	HashMap<uint64_t, HashMap<StringName, bool>>::Iterator oit = _rpc_unreliable_cache.find(oid);
	if (oit != _rpc_unreliable_cache.end()) {
		HashMap<StringName, bool>::Iterator mit = oit->value.find(p_method);
		if (mit != oit->value.end()) {
			return mit->value;
		}
	}

	bool unreliable = false;
	Dictionary mc;
	if (rpc_method_config(p_object, p_method, mc)) {
		const int tm = (int)mc.get("transfer_mode", (int)MultiplayerPeer::TRANSFER_MODE_RELIABLE);
		unreliable = tm == MultiplayerPeer::TRANSFER_MODE_UNRELIABLE || tm == MultiplayerPeer::TRANSFER_MODE_UNRELIABLE_ORDERED;
	}
	_rpc_unreliable_cache[oid][p_method] = unreliable;
	return unreliable;
}

void GoldNetMultiplayer::mark_dirty(Object *p_obj) {
	if (!p_obj) {
		return;
	}
	uint64_t id = (uint64_t)p_obj->get_instance_id();
	// Passed the synchronizer itself — the direct case. One the core doesn't have yet is read whole
	// when it does.
	if (owned.has(id)) {
		server.mark_dirty(id);
		return;
	}
	// Passed the node whose state is replicated. Games think in entities, so this is the call
	// site that actually reads well; the node -> sync answer is memoized because finding it
	// means scanning children.
	if (const uint64_t *routed = dirty_route.getptr(id)) {
		server.mark_dirty(*routed);
		return; // a stale route just means the sync is gone; nothing to mark
	}
	Node *node = Object::cast_to<Node>(p_obj);
	if (!node) {
		return;
	}
	for (int i = 0; i < node->get_child_count(); i++) {
		MultiplayerSynchronizer *sync = Object::cast_to<MultiplayerSynchronizer>(node->get_child(i));
		if (!sync) {
			continue;
		}
		uint64_t sid = (uint64_t)sync->get_instance_id();
		if (owned.has(sid)) {
			server.mark_dirty(sid);
			dirty_route[id] = sid;
			return;
		}
	}
	// No owned synchronizer under this node. Remember that too, so a game that marks a node we
	// do not own does not re-scan its children on every call.
	dirty_route[id] = 0;
}

void GoldNetMultiplayer::_bind_methods() {
	ClassDB::bind_method(D_METHOD("mark_dirty", "object"), &GoldNetMultiplayer::mark_dirty);
	ClassDB::bind_method(D_METHOD("set_snapshot_rate", "hz"), &GoldNetMultiplayer::set_snapshot_rate);
	ClassDB::bind_method(D_METHOD("get_snapshot_rate"), &GoldNetMultiplayer::get_snapshot_rate);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "snapshot_rate"), "set_snapshot_rate", "get_snapshot_rate");
	ClassDB::bind_method(D_METHOD("set_update_rate", "hz"), &GoldNetMultiplayer::set_update_rate);
	ClassDB::bind_method(D_METHOD("get_update_rate"), &GoldNetMultiplayer::get_update_rate);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "update_rate"), "set_update_rate", "get_update_rate");
	ClassDB::bind_method(D_METHOD("get_protocol_version"), &GoldNetMultiplayer::get_protocol_version);
	ClassDB::bind_method(D_METHOD("set_rate", "bytes_per_sec"), &GoldNetMultiplayer::set_rate);
	ClassDB::bind_method(D_METHOD("get_rate"), &GoldNetMultiplayer::get_rate);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "rate"), "set_rate", "get_rate");
	ClassDB::bind_method(D_METHOD("set_auto_rate", "enabled"), &GoldNetMultiplayer::set_auto_rate);
	ClassDB::bind_method(D_METHOD("get_auto_rate"), &GoldNetMultiplayer::get_auto_rate);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "auto_rate"), "set_auto_rate", "get_auto_rate");
	ClassDB::bind_method(D_METHOD("set_auto_command_rate", "enabled"), &GoldNetMultiplayer::set_auto_command_rate);
	ClassDB::bind_method(D_METHOD("get_auto_command_rate"), &GoldNetMultiplayer::get_auto_command_rate);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "auto_command_rate"), "set_auto_command_rate", "get_auto_command_rate");
	ClassDB::bind_method(D_METHOD("set_manual_send", "manual"), &GoldNetMultiplayer::set_manual_send);
	ClassDB::bind_method(D_METHOD("get_manual_send"), &GoldNetMultiplayer::get_manual_send);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "manual_send"), "set_manual_send", "get_manual_send");
	ClassDB::bind_method(D_METHOD("send_snapshots"), &GoldNetMultiplayer::send_snapshots);
	ClassDB::bind_method(D_METHOD("set_sim_config", "config"), &GoldNetMultiplayer::set_sim_config);
	ClassDB::bind_method(D_METHOD("get_sim_config"), &GoldNetMultiplayer::get_sim_config);
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "sim_config"), "set_sim_config", "get_sim_config");
	ClassDB::bind_method(D_METHOD("set_recv_sim_config", "config"), &GoldNetMultiplayer::set_recv_sim_config);
	ClassDB::bind_method(D_METHOD("get_recv_sim_config"), &GoldNetMultiplayer::get_recv_sim_config);
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "recv_sim_config"), "set_recv_sim_config", "get_recv_sim_config");
	ClassDB::bind_method(D_METHOD("get_peer_stats", "peer"), &GoldNetMultiplayer::get_peer_stats);
	ClassDB::bind_method(D_METHOD("set_relevance_events", "enabled"), &GoldNetMultiplayer::set_relevance_events);
	ClassDB::bind_method(D_METHOD("get_relevance_events"), &GoldNetMultiplayer::get_relevance_events);
	ClassDB::bind_method(D_METHOD("set_command_rate", "hz"), &GoldNetMultiplayer::set_command_rate);
	ClassDB::bind_method(D_METHOD("get_command_rate"), &GoldNetMultiplayer::get_command_rate);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "command_rate"), "set_command_rate", "get_command_rate");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "relevance_events"), "set_relevance_events", "get_relevance_events");
	// Emitted on a client when an owned MultiplayerSynchronizer leaves this peer's PVS (the server
	// stopped sending it). The game hides/deactivates the entity in response. Entry is not signalled —
	// a re-entering entity arrives as a full delta and fires the synchronizer's own `synchronized`.
	ADD_SIGNAL(MethodInfo("entity_relevance_lost", PropertyInfo(Variant::OBJECT, "sync")));
	// Emitted on a client for every received snapshot, carrying the server's send-time (ms, the header
	// clock). An always-on server-time feed regardless of entity traffic — feed it to a clock estimator
	// (see the ServerClock addon helper) instead of running a separate server-time beacon RPC.
	ADD_SIGNAL(MethodInfo("server_time_received", PropertyInfo(Variant::INT, "server_time_ms")));
	// Emitted on a client when the update rate the server grants it changes, and on its first snapshot:
	// what a game sizes its interpolation delay by. 0 = every tick, at a rate the server doesn't know.
	ADD_SIGNAL(MethodInfo("update_rate_granted", PropertyInfo(Variant::INT, "rate")));
}

// The ordered list of a synchronizer's *synced* replication-config property paths.
// This "slot" order (config order, sync props only) is identical on server and
// client — the changed-field bitmask indexes into it. Capped at 32 (u32 mask).
static void get_sync_slots(MultiplayerSynchronizer *p_sync, Vector<NodePath> &r_paths) {
	Ref<SceneReplicationConfig> cfg = p_sync->get_replication_config();
	if (cfg.is_null()) {
		return;
	}
	TypedArray<NodePath> props = cfg->get_properties();
	for (int i = 0; i < props.size() && r_paths.size() < goldnet::MAX_SLOTS; i++) {
		NodePath p = props[i];
		if (is_replicated_slot(cfg, p)) {
			r_paths.push_back(p);
		}
	}
}

// Resolve a synchronizer's replicated slots down to (object id, property path) pairs, so the
// per-tick read is nothing but get_indexed. Everything resolved here — the replication config,
// which properties are replicated, and which node owns each one — is fixed for the life of the
// entity, but used to be recomputed for every entity on every tick: at ~390 entities and 60 Hz
// that was the single largest fixed cost in the server tick, and none of it varied.
static void cache_slot_plan(MultiplayerSynchronizer *p_sync, Vector<uint64_t> &r_target_ids,
		Vector<NodePath> &r_props) {
	r_target_ids.clear();
	r_props.clear();
	Vector<NodePath> slots;
	get_sync_slots(p_sync, slots);
	Node *root = p_sync->get_node_or_null(p_sync->get_root_path());
	for (int s = 0; s < slots.size(); s++) {
		NodePath prop;
		Node *target = root ? resolve_property(root, slots[s], prop) : nullptr;
		// An unresolved slot is kept as a hole (id 0) rather than dropped: the slot INDEX is the
		// wire position, so skipping one would silently shift every later slot's meaning.
		r_target_ids.push_back(target ? (uint64_t)target->get_instance_id() : 0);
		r_props.push_back(prop);
	}
}

// Build the per-slot quantization tags from a synchronizer's "gn_quant" meta: a Dictionary keyed
// by the sync property's leaf name (e.g. "net_yaw") -> hint name ("angle16" | "half" | "vec3_half").
// Leaves r_quant empty when there's no meta or no recognized hint (the common case = all-auto).
// The name the gn_* metas key a slot by: its property path's last subname.
static String slot_leaf(const NodePath &p_slot) {
	const int sc = p_slot.get_subname_count();
	return sc > 0 ? String(p_slot.get_subname(sc - 1)) : String();
}

// A synchronizer's leaf-keyed hint meta, or an empty Dictionary when it's absent or malformed.
static Dictionary leaf_meta(MultiplayerSynchronizer *p_sync, const StringName &p_key) {
	const Variant mv = p_sync->has_meta(p_key) ? p_sync->get_meta(p_key) : Variant();
	return mv.get_type() == Variant::DICTIONARY ? Dictionary(mv) : Dictionary();
}

void GoldNetMultiplayer::_read_quant(MultiplayerSynchronizer *p_sync, std::vector<uint8_t> &r_quant) {
	r_quant.clear();
	const Dictionary hints = leaf_meta(p_sync, "gn_quant");
	if (hints.is_empty()) {
		return;
	}
	Vector<NodePath> slots;
	get_sync_slots(p_sync, slots);
	std::vector<uint8_t> q(slots.size());
	bool any = false;
	for (int s = 0; s < slots.size(); s++) {
		const String leaf = slot_leaf(slots[s]);
		const uint8_t code = hints.has(leaf) ? gn_quant_from_name(String(hints[leaf])) : QUANT_AUTO;
		q[s] = code;
		if (code != QUANT_AUTO) {
			any = true;
		}
	}
	if (any) {
		r_quant = q;
	}
}

// Build the per-slot push declaration from a synchronizer's "gn_push" meta: an Array of the sync
// properties' leaf names (e.g. ["net_pos", "net_stamp"]) that the game promises to announce with
// mark_dirty(). Leaves r_push empty when there's no meta or it names nothing this sync has, which
// every caller reads as "poll every slot" — the safe answer for a game that declared nothing.
void GoldNetMultiplayer::_read_push(MultiplayerSynchronizer *p_sync, std::vector<uint8_t> &r_push) {
	r_push.clear();
	const StringName meta_key("gn_push");
	if (!p_sync->has_meta(meta_key)) {
		return;
	}
	Variant mv = p_sync->get_meta(meta_key);
	if (mv.get_type() != Variant::ARRAY) {
		return;
	}
	Array names = mv;
	Vector<NodePath> slots;
	get_sync_slots(p_sync, slots);
	std::vector<uint8_t> q(slots.size());
	bool any = false;
	for (int s = 0; s < slots.size(); s++) {
		const String leaf = slot_leaf(slots[s]);
		q[s] = !leaf.is_empty() && names.has(leaf) ? 1 : 0;
		any = any || q[s];
	}
	if (any) {
		r_push = q;
	}
}

// How each slot of a synchronizer goes on the wire (SlotKind), from its property's declared type and
// its gn_quant / gn_grid hint, given the slots cache_slot_plan resolved. Both ends build it from the
// same scene, so it travels with nothing but a one-byte check on whole entities: a client whose
// coding differs refuses the entity rather than misreading it.
void GoldNetMultiplayer::_make_slot_coding(MultiplayerSynchronizer *p_sync, const Vector<uint64_t> &p_target_ids,
		const Vector<NodePath> &p_props, SlotCoding &r_coding) {
	Vector<NodePath> slots;
	get_sync_slots(p_sync, slots);
	std::vector<uint8_t> quant;
	_read_quant(p_sync, quant);
	const Dictionary grids = leaf_meta(p_sync, "gn_grid");
	const int n = MIN(slots.size(), p_props.size());
	r_coding.kinds.assign(n, SK_TAGGED);
	r_coding.grids.assign(n, 0.0);
	r_coding.quant.assign(n, QUANT_AUTO);
	// Slots nearly always share one target, so its property types are read once.
	uint64_t types_of = 0;
	Dictionary types; // property name -> declared Variant::Type
	uint32_t h = 2166136261u; // FNV-1a
	auto mix = [&h](uint8_t b) { h = (h ^ b) * 16777619u; };
	for (int s = 0; s < n; s++) {
		const uint8_t q = (size_t)s < quant.size() ? quant[s] : QUANT_AUTO;
		const String leaf = slot_leaf(slots[s]);
		const double grid = grids.has(leaf) ? (double)grids[leaf] : 0.0;
		Object *target = p_target_ids[s] ? ObjectDB::get_instance(p_target_ids[s]) : nullptr;
		if (target && p_target_ids[s] != types_of) {
			types_of = p_target_ids[s];
			types.clear();
			const TypedArray<Dictionary> props = target->get_property_list();
			for (int64_t i = 0; i < props.size(); i++) {
				const Dictionary d = props[i];
				types[d.get("name", "")] = d.get("type", (int)Variant::NIL);
			}
		}
		// A property one subname deep with a declared type; untyped, nested or missing is NIL.
		const Variant::Type type = target && p_props[s].get_subname_count() == 1
				? (Variant::Type)(int)types.get(String(p_props[s].get_subname(0)), (int)Variant::NIL)
				: Variant::NIL;
		uint8_t kind = SK_TAGGED;
		switch (type) {
			case Variant::INT:
				kind = q == goldnet::TAG_TIME_DELTA ? goldnet::TAG_TIME_DELTA : SK_INT_STEP;
				break;
			case Variant::BOOL:
				kind = goldnet::TAG_BOOL;
				break;
			case Variant::FLOAT:
				kind = grid > 0.0 ? SK_FLOAT_GRID
						: q == goldnet::TAG_ANGLE16 ? SK_ANGLE16_STEP
						: q == goldnet::TAG_ANGLE8 ? SK_ANGLE8_STEP
						: q == goldnet::TAG_HALF ? goldnet::TAG_HALF
												  : goldnet::TAG_FLOAT;
				break;
			case Variant::VECTOR3:
				kind = grid > 0.0 ? SK_VEC3_GRID : q == goldnet::TAG_VEC3_HALF ? goldnet::TAG_VEC3_HALF : goldnet::TAG_VEC3;
				break;
			default:
				break;
		}
		r_coding.kinds[s] = kind;
		r_coding.grids[s] = grid;
		r_coding.quant[s] = q;
		mix(kind);
		if (kind == SK_FLOAT_GRID || kind == SK_VEC3_GRID) {
			const float g = (float)grid;
			uint32_t bits;
			memcpy(&bits, &g, sizeof(bits));
			for (int b = 0; b < 4; b++) {
				mix((uint8_t)(bits >> (8 * b)));
			}
		}
	}
	r_coding.check = (uint8_t)(h ^ (h >> 8) ^ (h >> 16) ^ (h >> 24));
}

// The Godot side of goldnet::Server::tick: synchronizers are the entities, their properties the
// slot values, their visibility the PVS, and the snapshot rides the link's unreliable RPC.
struct GoldNetMultiplayer::ServerHost {
	GoldNetMultiplayer *gn;
	GoldNetLink *link;

	bool live(CoreServer::Entity &e) {
		MultiplayerSynchronizer *sync = sync_from_objid(e.key());
		if (!sync || !sync->is_inside_tree() || !sync->is_multiplayer_authority()) {
			return false;
		}
		e.user.sync = sync;
		return true;
	}

	// An entity's slots nearly always live on one object (its own node), so resolving the id once
	// and reusing it across the run turns N lookups per entity into one. A target that has since
	// been freed reads as nil, exactly as an unresolved one does.
	void read(CoreServer::Entity &e, bool polled_only) {
		uint64_t last_id = 0;
		Object *last_obj = nullptr;
		for (int s = 0; s < e.slots(); s++) {
			if (polled_only && e.pushed(s)) {
				continue; // the game promised to mark this one, and it didn't — nothing changed
			}
			uint64_t id = e.user.slot_target_id[s];
			if (id != last_id) {
				last_id = id;
				last_obj = id ? UtilityFunctions::instance_from_id(id) : nullptr;
			}
			gn->server.stamp(e, s, last_obj ? last_obj->get_indexed(e.user.slot_prop[s]) : Variant());
		}
	}

	// Per-peer PVS. The game drives each synchronizer's peer_visibility via set_visibility_for, so
	// this native read is the whole gate — public_visibility for "visible to all" entities, else the
	// per-peer bit. (MultiplayerSynchronizer's visibility *filters* can't be used: is_visible_to
	// isn't bound.)
	bool visible(const CoreServer::Entity &e, int32_t peer) {
		return e.user.sync->is_visibility_public() || e.user.sync->get_visibility_for(peer);
	}

	bool spawn_visible(uint64_t key, int32_t peer) {
		MultiplayerSynchronizer *s = sync_from_objid(key);
		return s && (s->is_visibility_public() || s->get_visibility_for(peer));
	}

	void encode(goldnet::Writer &w, const CoreServer::Entity &e, int slot, const Variant *base, uint32_t now) {
		const SlotCoding &c = e.user.coding;
		put_slot(w, c.kinds[slot], c.grids[slot], c.quant[slot], e.value(slot), base, now);
	}

	// GOLDNET_LOSS drops snapshots to exercise self-heal: the peer's ack stalls, so the next frame
	// keeps diffing against the same acked baseline and re-carries whatever changed since — no
	// desync, no retransmit. Surviving snapshots route through the send-side sim (latency/spike).
	void send(int32_t peer, const goldnet::Writer &w) {
		PackedByteArray bytes;
		bytes.resize((int64_t)w.size());
		memcpy(bytes.ptrw(), w.data(), w.size());
		if (!gn->sim.loss_roll()) {
			gn->_sim_send(peer, link, "_gn_recv", Array::make(bytes), true, 4 + 8 + (int)bytes.size());
		}
		gn->dbg_bytes += bytes.size();
		if (gn->rpc_stats) {
			// The snapshot rides an RPC too: its PackedByteArray arg costs 8 bytes of Variant header.
			gn->rpc_stats_snap_bytes += bytes.size() + 8 + RPC_HEADER_EST;
			gn->rpc_stats_snap_sends++;
		}
	}
};

// Hand the core each owned synchronizer on its first live tick (in the tree, and ours), as
// ServerHost::live would first say yes: by then the game has set the metas its layout is made from.
// They're read once — changing gn_quant / gn_priority / gn_push afterwards has no effect; a game
// needing a runtime importance change should express it through visibility (set_visibility_for),
// consulted every tick.
void GoldNetMultiplayer::_add_entities() {
	Vector<uint64_t> done;
	for (const uint64_t &objid : entities_to_add) {
		MultiplayerSynchronizer *sync = sync_from_objid(objid);
		if (!sync) {
			done.push_back(objid); // freed before it went live
			continue;
		}
		if (!sync->is_inside_tree() || !sync->is_multiplayer_authority()) {
			continue;
		}
		SyncData d;
		d.sync = sync;
		goldnet::EntityLayout layout;
		_read_push(sync, layout.push);
		layout.priority = gn_read_priority(sync);
		cache_slot_plan(sync, d.slot_target_id, d.slot_prop);
		layout.slots = d.slot_prop.size();
		_make_slot_coding(sync, d.slot_target_id, d.slot_prop, d.coding);
		for (uint8_t kind : d.coding.kinds) {
			layout.stepped.push_back(slot_kind_stepped(kind) ? 1 : 0);
		}
		layout.kinds_check = d.coding.check;
		server.add_entity(objid, owned[objid], std::move(layout)).user = std::move(d);
		done.push_back(objid);
	}
	for (int i = 0; i < done.size(); i++) {
		entities_to_add.erase(done[i]);
	}
}

void GoldNetMultiplayer::_server_tick() {
	GoldNetLink *l = _ensure_link();
	if (!l) {
		return;
	}
	_add_entities();
	uint32_t now = (uint32_t)Time::get_singleton()->get_ticks_msec();
	ServerHost host{ this, l };
	server.tick(now, host);

	if (dbg && now - dbg_last_ms > 2000) {
		double kbps = (double)dbg_bytes / (double)(now - dbg_last_ms); // bytes/ms == KB/s
		UtilityFunctions::print("[goldnet] send owned=", (int)server.entities().size(),
				" peers=", (int)server.peers().size(), " ~", kbps, " KB/s (all peers)");
		dbg_last_ms = now;
		dbg_bytes = 0;
	}
}

// The Godot side of goldnet::Client::apply: net_ids resolve to registered synchronizers, slots to
// their properties, and spawns / despawns / leaves to the spawners and signals the game listens to.
struct GoldNetMultiplayer::ClientHost {
	GoldNetMultiplayer *gn;

	void protocol_mismatch() {
		UtilityFunctions::push_error("goldnet: snapshot protocol mismatch — the server and client are "
				"running different goldnet builds. Rebuild/redeploy both to the same version.");
	}

	// Every snapshot's header clock, as an always-on server-time feed (see ServerClock).
	void server_time(uint32_t ms) { gn->emit_signal("server_time_received", (int64_t)ms); }

	void message(const uint8_t *data, size_t size, bool) { gn->_deliver_message(1, data, size); }

	bool spawn(uint32_t net_id, uint32_t spawner_net_id, const uint8_t *data, size_t size) {
		return gn->_apply_spawn(net_id, spawner_net_id, gn_bytes_to_var(data, size));
	}

	void despawn(uint32_t net_id) { gn->_apply_despawn(net_id); }

	void relevance_lost(uint32_t net_id) {
		if (const uint64_t *objid = gn->netid_to_objid.getptr(net_id)) {
			if (MultiplayerSynchronizer *s = sync_from_objid(*objid)) {
				gn->emit_signal("entity_relevance_lost", s);
			}
		}
	}

	// One synchronizer's replicated properties: decoded by their slot coding, set on the nodes they
	// name, and then the synchronizer's `synchronized` signal, which the game reacts to replicated
	// state in (only when something actually changed).
	goldnet::EntityResult entity(goldnet::EntityUpdate<Variant> &u) {
		const uint32_t net_id = u.net_id();
		const uint64_t *objid = gn->netid_to_objid.getptr(net_id);
		MultiplayerSynchronizer *sync = objid ? sync_from_objid(*objid) : nullptr;
		if (!sync) {
			return goldnet::ENTITY_SKIP; // freed or unknown: nothing to wait for, so it can't stall the stream
		}
		if (!sync->is_inside_tree()) {
			// Registered and still alive, but not in the tree yet: a warmup miss we should retry
			// rather than silently swallow.
			return goldnet::ENTITY_NOT_READY;
		}
		Vector<NodePath> slots;
		get_sync_slots(sync, slots);
		const SlotCoding *coding = gn->client_slot_codings.getptr(*objid);
		if (!coding) {
			Vector<uint64_t> target_ids;
			Vector<NodePath> props;
			cache_slot_plan(sync, target_ids, props);
			SlotCoding made;
			_make_slot_coding(sync, target_ids, props, made);
			gn->client_slot_codings[*objid] = made;
			coding = gn->client_slot_codings.getptr(*objid);
		}
		if (u.whole() && u.kinds_check() != coding->check) {
			if (!gn->warned_kinds.has(net_id)) {
				gn->warned_kinds.insert(net_id);
				UtilityFunctions::push_error("goldnet: ", sync->get_path(), "'s replicated properties are typed or "
						"hinted differently here than on the server; its state is ignored. Rebuild both from the same project.");
			}
			return goldnet::ENTITY_SKIP;
		}
		Node *root = sync->get_node_or_null(sync->get_root_path());
		bool applied = false;
		for (int s : u.changed()) {
			if (s >= slots.size() || s >= (int)coding->kinds.size()) {
				break; // a slot this end doesn't have: left unset, so the client doesn't ack the snapshot
			}
			const Variant v = get_slot(u.reader(), coding->kinds[s], coding->grids[s], u.base(s), u.server_time());
			if (u.reader().failed()) {
				break;
			}
			u.set(s, v);
			NodePath prop;
			Node *target = root ? resolve_property(root, slots[s], prop) : nullptr;
			if (target) {
				target->set_indexed(prop, v);
				applied = true;
			}
		}
		if (applied) {
			sync->emit_signal("synchronized");
		}
		return goldnet::ENTITY_APPLIED;
	}
};

void GoldNetMultiplayer::apply_snapshot(const PackedByteArray &p_bytes) {
	ClientHost host{ this };
	goldnet::Client<Variant>::ApplyInfo info = client.apply(p_bytes.ptr(), (size_t)p_bytes.size(), host,
			(uint32_t)Time::get_singleton()->get_ticks_msec());
	const int granted = client.stats().update_rate_granted;
	if (granted != update_rate_granted) {
		update_rate_granted = granted;
		emit_signal("update_rate_granted", (int64_t)update_rate_granted);
	}
	if (dbg && info.committed) {
		uint64_t now = Time::get_singleton()->get_ticks_msec();
		if (now - dbg_last_ms > 2000) {
			dbg_last_ms = now;
			UtilityFunctions::print("[goldnet] recv seq=", info.seq, " base=", info.base_seq,
					" spawn=", info.spawns, " despawn=", info.despawns,
					" changed=", info.changed, " bytes=", (int)p_bytes.size(),
					" registered=", (int)owned.size());
		}
	}
}

// The Godot side of goldnet::Server::receive_uplink.
struct GoldNetMultiplayer::UplinkHost {
	GoldNetMultiplayer *gn;

	void protocol_mismatch(int32_t peer) {
		UtilityFunctions::push_error("goldnet: uplink protocol mismatch from peer ", peer, " — the server "
				"and client are running different goldnet builds. Rebuild/redeploy both to the same version.");
	}
	const goldnet::Schema *command_schema(int32_t, uint32_t stream) {
		const CommandTarget *t = gn->_command_target(stream);
		return t ? &t->sig.kinds : nullptr;
	}
	void command(int32_t peer, uint32_t stream, const goldnet::Command &cmd) { gn->_deliver_command(peer, stream, cmd); }
	void message(int32_t peer, const uint8_t *data, size_t size, bool) { gn->_deliver_message(peer, data, size); }
};

void GoldNetMultiplayer::on_uplink(int32_t p_peer, const PackedByteArray &p_bytes) {
	if (inner->get_unique_id() != 1) {
		return;
	}
	UplinkHost host{ this };
	server.receive_uplink(p_peer, p_bytes.ptr(), (size_t)p_bytes.size(), host,
			(uint32_t)Time::get_singleton()->get_ticks_msec());
}

// How a command field maps back to its argument: the field kind, and for bytes whether the argument
// was a PackedByteArray or any other Variant (put_arg's encoding, coded bytewise).
enum CommandArgForm : uint8_t {
	CMD_ARG_INT,
	CMD_ARG_FLOAT,
	CMD_ARG_BOOL,
	CMD_ARG_BYTES,
	CMD_ARG_VARIANT,
};

static uint8_t command_arg_form(int p_type) {
	switch (p_type) {
		case Variant::INT:
			return CMD_ARG_INT;
		case Variant::FLOAT:
			return CMD_ARG_FLOAT;
		case Variant::BOOL:
			return CMD_ARG_BOOL;
		case Variant::PACKED_BYTE_ARRAY:
			return CMD_ARG_BYTES;
		default:
			return CMD_ARG_VARIANT;
	}
}

// A command stream's fields are its method's arguments, typed by their declarations: an int, float
// or bool argument is a field of that kind (a float travels as f32), a PackedByteArray one is bytes,
// and anything else, typed or not, is its Variant encoding as bytes. Or, when CommandStream.mark gave
// a field table (the node's gn_command_fields meta, SyncSpec's shape: name -> {"type": ...}), the
// method takes one Dictionary and each entry the table names is a field of its given type, in the
// table's order. Both ends read the same script, so they agree with no handshake; the signature is
// folded into the stream id, so two builds that declare it differently see different streams rather
// than misreading each other's fields.
static bool command_signature(Object *p_object, const StringName &p_method, std::vector<uint8_t> &r_forms,
		std::vector<StringName> &r_names, bool &r_table) {
	Node *n = Object::cast_to<Node>(p_object);
	const Dictionary declared = n && n->has_meta("gn_command_fields") ? Dictionary(n->get_meta("gn_command_fields")) : Dictionary();
	r_table = declared.has(p_method);
	r_forms.clear();
	r_names.clear();
	if (r_table) {
		const Dictionary table = declared[p_method];
		if (table.size() > goldnet::MAX_COMMAND_FIELDS) {
			UtilityFunctions::push_error("goldnet: command stream '", p_method, "' declares more than ",
					goldnet::MAX_COMMAND_FIELDS, " fields.");
			return false;
		}
		const Array keys = table.keys();
		for (int64_t i = 0; i < keys.size(); i++) {
			const Dictionary opts = table[keys[i]];
			r_names.push_back(StringName(keys[i]));
			r_forms.push_back(command_arg_form((int)opts.get("type", (int)Variant::NIL)));
		}
		return true;
	}
	const TypedArray<Dictionary> methods = p_object->get_method_list();
	for (int64_t i = 0; i < methods.size(); i++) {
		const Dictionary m = methods[i];
		if (StringName(m.get("name", "")) != p_method) {
			continue;
		}
		const Array args = m.get("args", Array());
		if (args.size() > goldnet::MAX_COMMAND_FIELDS) {
			UtilityFunctions::push_error("goldnet: command stream '", p_method, "' takes more than ",
					goldnet::MAX_COMMAND_FIELDS, " arguments.");
			return false;
		}
		for (int64_t a = 0; a < args.size(); a++) {
			const Dictionary arg = args[a];
			r_names.push_back(StringName(arg.get("name", "")));
			r_forms.push_back(command_arg_form((int)arg.get("type", (int)Variant::NIL)));
		}
		return true;
	}
	return false;
}

static goldnet::FieldKind command_field_kind(uint8_t p_form) {
	switch (p_form) {
		case CMD_ARG_INT:
			return goldnet::FIELD_INT;
		case CMD_ARG_FLOAT:
			return goldnet::FIELD_FLOAT;
		case CMD_ARG_BOOL:
			return goldnet::FIELD_BOOL;
		default:
			return goldnet::FIELD_BYTES;
	}
}

// The signature for p_node's p_method, id 0 if it isn't a command stream or can't be one.
static void make_command_signature(Node *p_node, const StringName &p_method, uint32_t &r_id, goldnet::Schema &r_kinds,
		std::vector<uint8_t> &r_forms, std::vector<StringName> &r_names, bool &r_table) {
	r_id = 0;
	if (!command_signature(p_node, p_method, r_forms, r_names, r_table)) {
		return;
	}
	String sig = r_table ? "{" : "(";
	r_kinds.clear();
	for (size_t k = 0; k < r_forms.size(); k++) {
		r_kinds.push_back(command_field_kind(r_forms[k]));
		sig += String(r_names[k]) + ":" + String::chr('a' + r_forms[k]) + ",";
	}
	r_id = (String(p_node->get_path()) + "::" + String(p_method) + sig).hash();
	r_id = r_id ? r_id : 1;
}

// The signature for an RPC, or null if it isn't a command stream: the node lists the method in its
// gn_commands meta, and the method is an @rpc (an unmarked one fails here exactly as it would on stock).
const GoldNetMultiplayer::CommandSignature *GoldNetMultiplayer::_command_stream(Object *p_object, const StringName &p_method) {
	const uint64_t oid = p_object->get_instance_id();
	HashMap<StringName, CommandSignature> &methods = command_stream_cache[oid];
	if (const CommandSignature *cached = methods.getptr(p_method)) {
		return cached->id ? cached : nullptr;
	}
	CommandSignature sig;
	Node *n = Object::cast_to<Node>(p_object);
	if (n && n->has_meta("gn_commands") && Array(n->get_meta("gn_commands")).has(p_method)) {
		Dictionary cfg;
		if (rpc_method_config(p_object, p_method, cfg)) {
			make_command_signature(n, p_method, sig.id, sig.kinds, sig.forms, sig.names, sig.table);
		} else {
			UtilityFunctions::push_error("goldnet: '", p_method, "' is in gn_commands but isn't an @rpc; "
					"calling it as an ordinary RPC.");
		}
	}
	methods[p_method] = sig;
	const CommandSignature *stored = methods.getptr(p_method);
	return stored->id ? stored : nullptr;
}

// A call's arguments as its command's fields. False, with an error, if they don't fit the signature.
static bool command_from_args(const std::vector<uint8_t> &p_forms, const std::vector<StringName> &p_names, bool p_table,
		const StringName &p_method, const Array &p_call_args, goldnet::Command &r_cmd) {
	Array args = p_call_args;
	if (p_table) {
		if (p_call_args.size() != 1 || p_call_args[0].get_type() != Variant::DICTIONARY) {
			UtilityFunctions::push_error("goldnet: command stream '", p_method, "' takes one Dictionary, its fields; "
					"the call is dropped.");
			return false;
		}
		const Dictionary d = p_call_args[0];
		args = Array();
		for (const StringName &name : p_names) {
			if (!d.has(name)) {
				UtilityFunctions::push_error("goldnet: command stream '", p_method, "' is missing its field '", name,
						"'; the call is dropped.");
				return false;
			}
			args.push_back(d[name]);
		}
		if ((size_t)d.size() != p_names.size()) {
			UtilityFunctions::push_error("goldnet: command stream '", p_method, "' was given a field its table "
					"doesn't declare; the call is dropped.");
			return false;
		}
	} else if ((size_t)args.size() != p_forms.size()) {
		UtilityFunctions::push_error("goldnet: command stream '", p_method, "' takes ", (int64_t)p_forms.size(),
				" arguments but was called with ", args.size(), "; the call is dropped.");
		return false;
	}
	r_cmd.assign(p_forms.size(), goldnet::Field());
	for (size_t k = 0; k < p_forms.size(); k++) {
		const Variant &v = args[(int64_t)k];
		const Variant::Type t = v.get_type();
		const bool number = t == Variant::INT || t == Variant::FLOAT || t == Variant::BOOL;
		bool ok = true;
		switch (p_forms[k]) {
			case CMD_ARG_INT:
				ok = number;
				r_cmd[k].i = ok ? (int64_t)v : 0;
				break;
			case CMD_ARG_FLOAT:
				ok = number;
				r_cmd[k].f = ok ? (float)(double)v : 0.0f;
				break;
			case CMD_ARG_BOOL:
				ok = number;
				r_cmd[k].i = ok && (bool)v ? 1 : 0;
				break;
			case CMD_ARG_BYTES: {
				ok = t == Variant::PACKED_BYTE_ARRAY;
				if (ok) {
					const PackedByteArray b = v;
					r_cmd[k].bytes.assign(b.ptr(), b.ptr() + b.size());
				}
			} break;
			default: {
				goldnet::Writer w;
				put_arg(w, v);
				r_cmd[k].bytes.assign(w.data(), w.data() + w.size());
			} break;
		}
		if (!ok) {
			UtilityFunctions::push_error("goldnet: command stream '", p_method, "' argument ", (int64_t)k,
					" is a ", Variant::get_type_name(t), ", which its declared type can't take; the call is dropped.");
			return false;
		}
	}
	return true;
}

void GoldNetMultiplayer::_scan_command_targets(Node *p_node) {
	if (p_node->has_meta("gn_commands")) {
		Array methods = p_node->get_meta("gn_commands");
		for (int i = 0; i < methods.size(); i++) {
			CommandTarget t;
			t.object_id = p_node->get_instance_id();
			t.method = methods[i];
			make_command_signature(p_node, t.method, t.sig.id, t.sig.kinds, t.sig.forms, t.sig.names, t.sig.table);
			if (t.sig.id) {
				command_targets[t.sig.id] = t;
			}
		}
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		_scan_command_targets(p_node->get_child(i));
	}
}

// The method a stream id calls, or null. A node marked since the last scan, or one freed and
// re-added, takes a rescan; that is a tree walk, so a client sending ids nothing matches can't make
// the server do it more than once a second.
const GoldNetMultiplayer::CommandTarget *GoldNetMultiplayer::_command_target(uint32_t p_stream) {
	const CommandTarget *t = command_targets.getptr(p_stream);
	if (t && ObjectDB::get_instance(t->object_id)) {
		return t;
	}
	const uint64_t now = Time::get_singleton()->get_ticks_msec();
	if (command_scan_ms != 0 && now - command_scan_ms < 1000) {
		return nullptr;
	}
	command_scan_ms = now;
	command_targets.clear();
	SceneTree *tree = Object::cast_to<SceneTree>(Engine::get_singleton()->get_main_loop());
	if (tree && tree->get_root()) {
		_scan_command_targets(tree->get_root());
	}
	t = command_targets.getptr(p_stream);
	return t && ObjectDB::get_instance(t->object_id) ? t : nullptr;
}

// Call the method a command stands for, as the inner would have for the RPC: the same permission
// rule, and get_remote_sender_id() answering the client that sent it.
void GoldNetMultiplayer::_deliver_command(int32_t p_peer, uint32_t p_stream, const goldnet::Command &p_cmd) {
	const CommandTarget *t = _command_target(p_stream);
	if (!t || p_cmd.size() != t->sig.forms.size()) {
		return;
	}
	Object *o = ObjectDB::get_instance(t->object_id);
	Array args;
	for (size_t k = 0; k < p_cmd.size(); k++) {
		const goldnet::Field &f = p_cmd[k];
		switch (t->sig.forms[k]) {
			case CMD_ARG_INT:
				args.push_back(f.i);
				break;
			case CMD_ARG_FLOAT:
				args.push_back((double)f.f);
				break;
			case CMD_ARG_BOOL:
				args.push_back(f.i != 0);
				break;
			case CMD_ARG_BYTES: {
				PackedByteArray b;
				b.resize((int64_t)f.bytes.size());
				if (!f.bytes.empty()) {
					memcpy(b.ptrw(), f.bytes.data(), f.bytes.size());
				}
				args.push_back(b);
			} break;
			default: {
				goldnet::Reader r(f.bytes.data(), f.bytes.size());
				const Variant v = get_arg(r);
				if (r.failed()) {
					return;
				}
				args.push_back(v);
			} break;
		}
	}
	Dictionary cfg;
	Node *n = Object::cast_to<Node>(o);
	if (!n || !rpc_method_config(o, t->method, cfg)) {
		return;
	}
	const int mode = (int)cfg.get("rpc_mode", (int)MultiplayerAPI::RPC_MODE_AUTHORITY);
	if (mode == MultiplayerAPI::RPC_MODE_DISABLED ||
			(mode == MultiplayerAPI::RPC_MODE_AUTHORITY && n->get_multiplayer_authority() != p_peer)) {
		return;
	}
	const StringName method = t->method;
	const int32_t outer_sender = command_sender;
	command_sender = p_peer;
	if (t->sig.table) {
		Dictionary fields;
		for (size_t k = 0; k < t->sig.names.size(); k++) {
			fields[t->sig.names[k]] = args[(int64_t)k];
		}
		args = Array::make(fields);
	}
	o->callv(method, args);
	command_sender = outer_sender;
}

// Queue one call to p_peer on the netchan: reliable, or in the datagram. False if it should go through
// the inner instead (an unreliable call too big for a datagram).
bool GoldNetMultiplayer::_send_message(int32_t p_peer, Node *p_node, const StringName &p_method, const Array &p_args,
		bool p_reliable) {
	const bool on_server = inner->get_unique_id() == 1;
	const String path = String(p_node->get_path());
	const String key = path + String("::") + String(p_method);
	uint32_t id;
	if (const uint32_t *known_id = msg_ids.getptr(key)) {
		id = *known_id;
	} else {
		id = msg_next_id++;
		msg_ids[key] = id;
	}
	goldnet::Writer reg;
	goldnet::put_uvarint(&reg, 0);
	goldnet::put_uvarint(&reg, id);
	put_str(reg, path);
	put_str(reg, String(p_method));
	goldnet::Writer args;
	goldnet::put_uvarint(&args, (uint32_t)p_args.size());
	for (int i = 0; i < p_args.size(); i++) {
		put_arg(args, p_args[i]);
	}

	HashMap<uint32_t, uint64_t> &registered = msg_registered[p_peer];
	const uint64_t *reg_at = registered.getptr(id);
	uint64_t queued;
	uint64_t delivered;
	if (on_server) {
		const CoreServer::PeerStats st = server.peer_stats(p_peer);
		queued = st.messages_queued;
		delivered = st.messages_delivered;
	} else {
		const goldnet::Client<Variant>::Stats st = client.stats();
		queued = st.messages_queued;
		delivered = st.messages_delivered;
	}
	const bool known = reg_at && delivered > *reg_at;
	// A call is its target's id and its arguments. An unreliable one to a target the peer may not know
	// yet carries the registration in front instead, so it can go in the datagram with the rest.
	goldnet::Writer call;
	if (p_reliable || known) {
		goldnet::put_uvarint(&call, id);
	} else {
		call.put_data(reg);
	}
	call.put_data(args);
	if (!p_reliable && (int)call.size() > MAX_UNRELIABLE_MESSAGE) {
		return false;
	}
	auto queue_reliable = [&](const goldnet::Writer &w) {
		if (on_server) {
			server.queue_reliable(p_peer, w.data(), w.size());
		} else {
			client.queue_reliable(w.data(), w.size());
		}
	};
	if (!reg_at) {
		registered[id] = queued;
		queue_reliable(reg);
	}
	if (p_reliable) {
		queue_reliable(call);
	} else if (on_server) {
		server.queue_unreliable(p_peer, call.data(), call.size()); // a full datagram drops it, as GoldSrc's did
	} else {
		client.queue_unreliable(call.data(), call.size());
	}
	return true;
}

// GoldSrc dropped a client whose reliable channel overflowed: it isn't taking what it's sent.
void GoldNetMultiplayer::_check_overflow(int32_t p_peer) {
	Ref<MultiplayerPeer> mp = _get_multiplayer_peer();
	if (mp.is_null()) {
		return;
	}
	if (inner->get_unique_id() == 1) {
		if (server.peer_stats(p_peer).overflowed) {
			UtilityFunctions::push_error("goldnet: reliable channel overflowed for peer ", p_peer, "; dropping it.");
			server.remove_peer(p_peer);
			mp->disconnect_peer(p_peer);
		}
	} else if (client.stats().overflowed) {
		UtilityFunctions::push_error("goldnet: reliable channel to the server overflowed; disconnecting.");
		client.reset();
		mp->close();
	}
}

// One message from p_from: a target registration, or a call, made the way the inner would have made it.
void GoldNetMultiplayer::_deliver_message(int32_t p_from, const uint8_t *p_data, size_t p_size) {
	goldnet::Reader r(p_data, p_size);
	uint32_t id = goldnet::get_uvarint(&r);
	HashMap<uint32_t, MsgTarget> &targets = msg_targets[p_from];
	if (id == 0) {
		// A registration, and a call to it if anything follows (see _send_message).
		const uint32_t new_id = goldnet::get_uvarint(&r);
		const NodePath path = NodePath(get_str(r));
		const StringName method = StringName(get_str(r));
		if (r.failed() || new_id == 0) {
			return;
		}
		MsgTarget *known = targets.getptr(new_id);
		if (!known || known->path != path || known->method != method) {
			MsgTarget t;
			t.path = path;
			t.method = method;
			targets[new_id] = t;
		}
		if (r.remaining() == 0) {
			return;
		}
		id = new_id;
	}
	MsgTarget *t = targets.getptr(id);
	if (!t) {
		return;
	}
	Node *n = nullptr;
	if (Object *o = t->object_id ? ObjectDB::get_instance(t->object_id) : nullptr) {
		n = Object::cast_to<Node>(o);
		if (n && !n->is_inside_tree()) {
			n = nullptr;
		}
	}
	if (!n) {
		SceneTree *tree = Object::cast_to<SceneTree>(Engine::get_singleton()->get_main_loop());
		n = tree && tree->get_root() ? tree->get_root()->get_node_or_null(t->path) : nullptr;
		t->object_id = n ? n->get_instance_id() : 0;
	}
	if (!n) {
		if (!t->warned) {
			t->warned = true;
			UtilityFunctions::push_warning("goldnet: RPC '", t->method, "' for ", t->path, ", which isn't here.");
		}
		return;
	}
	const uint32_t argc = goldnet::get_uvarint(&r);
	Array args;
	for (uint32_t i = 0; i < argc && !r.failed(); i++) {
		args.push_back(get_arg(r));
	}
	Dictionary cfg;
	if (r.failed() || !rpc_method_config(n, t->method, cfg)) {
		return;
	}
	const int mode = (int)cfg.get("rpc_mode", (int)MultiplayerAPI::RPC_MODE_AUTHORITY);
	if (mode == MultiplayerAPI::RPC_MODE_DISABLED ||
			(mode == MultiplayerAPI::RPC_MODE_AUTHORITY && n->get_multiplayer_authority() != p_from)) {
		return;
	}
	const StringName method = t->method; // the call may register more targets, moving t
	const int32_t outer_sender = command_sender;
	command_sender = p_from;
	n->callv(method, args);
	command_sender = outer_sender;
}

// The RPC on the netchan, if it can go there: every call between the server and a client on channel 0.
// A call declared on another channel goes through the inner, unordered against the netchan, as stock
// orders each channel apart; so does a client's call to another client (the inner relays those).
bool GoldNetMultiplayer::_netchan_rpc(int32_t p_peer, Object *p_object, const StringName &p_method, const Array &p_args) {
	Ref<MultiplayerPeer> mp = inner->get_multiplayer_peer();
	if (mp.is_null() || mp->get_connection_status() != MultiplayerPeer::CONNECTION_CONNECTED) {
		return false;
	}
	Node *n = Object::cast_to<Node>(p_object);
	if (!n || !n->is_inside_tree()) {
		return false;
	}
	Dictionary cfg;
	if (!rpc_method_config(p_object, p_method, cfg)) {
		return false; // not an @rpc: the inner says so
	}
	if ((int)cfg.get("channel", 0) != 0) {
		return false;
	}
	const bool reliable = (int)cfg.get("transfer_mode", (int)MultiplayerPeer::TRANSFER_MODE_RELIABLE) ==
			MultiplayerPeer::TRANSFER_MODE_RELIABLE;
	if (inner->get_unique_id() != 1) {
		if (p_peer != 1 || !_send_message(1, n, p_method, p_args, reliable)) {
			return false;
		}
		_check_overflow(1);
		return true;
	}
	if (p_peer == 1) {
		return false; // only ourselves
	}
	const PackedInt32Array peers = inner->get_peers();
	std::vector<int32_t> targets;
	if (p_peer > 0) {
		if (!peers.has(p_peer)) {
			return false; // the inner reports the unknown peer
		}
		targets.push_back(p_peer);
	} else {
		for (int i = 0; i < peers.size(); i++) {
			if (peers[i] != -p_peer) {
				targets.push_back(peers[i]);
			}
		}
	}
	for (int32_t t : targets) {
		if (_send_message(t, n, p_method, p_args, reliable)) {
			_check_overflow(t);
		} else {
			_charge_rpc(t, p_args);
			_sim_send(t, p_object, p_method, p_args, !reliable);
		}
	}
	if ((bool)cfg.get("call_local", false) && (p_peer == 0 || (p_peer < 0 && p_peer != -1))) {
		inner->rpc(1, p_object, p_method, p_args); // the server's own copy, as the inner makes it
	}
	return true;
}

// Client: one uplink packet, whenever the core says one is due (goldnet::Client::uplink_due).
void GoldNetMultiplayer::_send_uplink() {
	Ref<MultiplayerPeer> peer = inner->get_multiplayer_peer();
	if (peer.is_null() || peer->get_connection_status() != MultiplayerPeer::CONNECTION_CONNECTED ||
			inner->get_unique_id() == 1) {
		return;
	}
	const uint32_t now = (uint32_t)Time::get_singleton()->get_ticks_msec();
	if (!client.uplink_due(now)) {
		return;
	}
	GoldNetLink *l = _ensure_link();
	if (!l) {
		return;
	}
	goldnet::Writer w;
	client.write_uplink(now, w);
	if (sim.loss_percent() > 0 && sim.loss_roll()) {
		return; // simulated drop
	}
	PackedByteArray bytes;
	bytes.resize((int64_t)w.size());
	memcpy(bytes.ptrw(), w.data(), w.size());
	_sim_send(1, l, "_gn_up", Array::make(bytes), true, (int)w.size() + 8);
}

void GoldNetMultiplayer::set_command_rate(double p_hz) {
	client.set_command_rate(p_hz);
}

double GoldNetMultiplayer::get_command_rate() const {
	return client.command_rate();
}

// Automatic, up to the rate commands are made: one a physics tick.
void GoldNetMultiplayer::set_auto_command_rate(bool p_enabled) {
	client.set_auto_command_rate(p_enabled, (double)Engine::get_singleton()->get_physics_ticks_per_second());
}

bool GoldNetMultiplayer::get_auto_command_rate() const {
	return client.auto_command_rate();
}

Error GoldNetMultiplayer::_poll() {
	// Before the inner polls, since that is when its peer flushes: a command made this frame goes out
	// this frame, as the RPC it replaces would have.
	_send_uplink();
	Error e = inner->poll();
	_ensure_link();
	uint64_t now = Time::get_singleton()->get_ticks_msec();
	// Pump the send-side sim every poll on BOTH ends: a client delays its input RPCs here (the
	// client→server leg), a server its snapshots + RPCs (server→client). Also advances the spike timer.
	sim.pump(now, [this](PendingSend &ps) {
		Object *o = ObjectDB::get_instance(ps.object_id);
		if (o) {
			inner->rpc(ps.peer, o, ps.method, ps.args);
		}
	});
	// A peerless MultiplayerAPI has no unique id to report, and asking for one logs an error.
	// That is a normal state, not a broken one — the API sits peerless before the first session
	// and again after one ends (including a join the server refused because it was full) — and
	// because this runs every poll, one error becomes a continuous stream for as long as the
	// game is open. Nothing below applies without a peer: the server branch needs one anyway. A
	// client whose server went away still holds its peer, closed, which logs the same way.
	Ref<MultiplayerPeer> mp = inner->get_multiplayer_peer();
	if (mp.is_null() || mp->get_connection_status() == MultiplayerPeer::CONNECTION_DISCONNECTED) {
		return e;
	}
	if (rpc_stats) {
		_rpc_stats_print(now);
	}
	if (inner->get_unique_id() == 1) {
		_drain_pending_spawns(); // every poll — pick up spawns promptly
		_detect_despawns();
		if (!manual_send && inner->get_peers().size() > 0) {
			// Advance the deadline by the interval rather than resetting it to the clock we happened
			// to read. `last = now` makes every period the interval rounded UP to the next poll, so
			// the cadence inherits the poll granularity and loses the remainder every time: a server
			// polling at ~160 Hz sent a nominal 60 Hz stream at ~50 Hz, with gaps spread 12-36 ms
			// instead of a steady 16.7. Clients size their interpolation buffer from the nominal
			// rate, so that spread is what pushes an entity's buffer dry between updates.
			uint64_t now_us = Time::get_singleton()->get_ticks_usec();
			uint64_t iv_us = cached_min_interval_us > 0 ? cached_min_interval_us : 1ULL;
			if (last_send_us == 0) {
				last_send_us = now_us;
			}
			if (now_us - last_send_us >= iv_us) {
				last_send_us += iv_us;
				// More than a whole interval behind means a real stall (a load hitch, a debugger
				// break), not poll granularity. Resync instead of bursting to catch up.
				if (now_us - last_send_us >= iv_us) {
					last_send_us = now_us;
				}
				_server_tick();
			}
		}
	}
	return e;
}

void GoldNetMultiplayer::_set_multiplayer_peer(const Ref<MultiplayerPeer> &p_peer) {
	// Second arming point: the moment a session starts, covering a game that spawns after setting the
	// peer but before our first poll. Idempotent.
	capture_spawners();
	_end_server_session();
	// A client's peer goes in behind the receive sim; a server (id 1) or no peer goes in as is.
	recv_peer.unref();
	if (p_peer.is_valid() && p_peer->get_unique_id() != 1) {
		recv_peer.instantiate();
		recv_peer->wrap(p_peer);
		apply_sim_config(recv_peer->get_sim(), read_sim_config(recv_sim), "recv_sim_config");
		inner->set_multiplayer_peer(recv_peer);
		return;
	}
	inner->set_multiplayer_peer(p_peer);
}

// The game's own peer, never the receive sim's wrapper around it.
Ref<MultiplayerPeer> GoldNetMultiplayer::_get_multiplayer_peer() {
	Ref<MultiplayerPeer> p = inner->get_multiplayer_peer();
	if (recv_peer.is_valid() && p.ptr() == recv_peer.ptr()) {
		return recv_peer->get_wrapped();
	}
	return p;
}

int32_t GoldNetMultiplayer::_get_unique_id() const {
	return inner->get_unique_id();
}

PackedInt32Array GoldNetMultiplayer::_get_peer_ids() const {
	return inner->get_peers();
}

Error GoldNetMultiplayer::_rpc(int32_t p_peer, Object *p_object, const StringName &p_method, const Array &p_args) {
	// The single chokepoint every game @rpc passes through — goldnet's other send funnel. Apply
	// send-side loss (unreliable only) + latency/spike here; the game can't observe the few-ms
	// deferral (same contract the old net_latency_sim.gd send wrappers had). Loss drops only
	// UNRELIABLE RPCs — dropping a reliable one would stop ENet retransmitting it and desync state
	// A command stream's calls skip this: they ride the uplink, whose own send
	// (_send_uplink) the sim applies to.
	if (rpc_stats) {
		_rpc_stats_count(p_peer, p_object, p_method, p_args);
	}
	// A command stream's call rides the uplink instead (see _command_stream).
	if (p_peer == 1 && inner->get_multiplayer_peer().is_valid() && inner->get_unique_id() != 1) {
		if (const CommandSignature *sig = _command_stream(p_object, p_method)) {
			goldnet::Command cmd;
			if (!command_from_args(sig->forms, sig->names, sig->table, p_method, p_args, cmd)) {
				return ERR_INVALID_PARAMETER;
			}
			if (client.command_schema(sig->id) != sig->kinds) {
				client.set_command_schema(sig->id, sig->kinds);
			}
			client.push_command(sig->id, std::move(cmd), (uint32_t)Time::get_singleton()->get_ticks_msec());
			return OK;
		}
	}
	if (_netchan_rpc(p_peer, p_object, p_method, p_args)) {
		return OK;
	}
	const bool simulating = sim.loss_percent() > 0 || sim.bandwidth_bps() > 0;
	const bool droppable = simulating && _rpc_is_unreliable(p_object, p_method);
	if (droppable && sim.loss_percent() > 0 && sim.loss_roll()) {
		return OK; // simulated drop
	}
	_charge_rpc(p_peer, p_args);
	return _sim_send(p_peer, p_object, p_method, p_args, droppable);
}

void GoldNetMultiplayer::_rpc_stats_count(int32_t p_peer, Object *p_object, const StringName &p_method, const Array &p_args) {
	if (p_peer == inner->get_unique_id()) {
		return; // a call to ourselves never reaches the wire
	}
	int recipients = 1;
	if (p_peer <= 0) {
		recipients = inner->get_peers().size();
		if (p_peer < 0 && recipients > 0) {
			recipients -= 1;
		}
	}
	if (recipients <= 0) {
		return;
	}
	int bytes = RPC_HEADER_EST;
	for (int i = 0; i < p_args.size(); i++) {
		bytes += UtilityFunctions::var_to_bytes(p_args[i]).size();
	}
	RpcStat &s = rpc_stats_methods[p_method];
	if (s.calls == 0) {
		s.unreliable = _rpc_is_unreliable(p_object, p_method);
		Ref<Script> scr = p_object->get_script();
		s.owner = scr.is_valid() ? scr->get_path().get_file() : p_object->get_class();
	}
	s.calls++;
	s.sends += recipients;
	s.bytes += (uint64_t)bytes * recipients;
}

void GoldNetMultiplayer::_rpc_stats_print(uint64_t p_now) {
	if (rpc_stats_last_ms == 0) {
		rpc_stats_last_ms = p_now;
		return;
	}
	uint64_t span = p_now - rpc_stats_last_ms;
	if (span < 5000) {
		return;
	}
	double secs = (double)span / 1000.0;
	std::vector<std::pair<StringName, RpcStat>> rows;
	uint64_t rpc_bytes = 0;
	uint64_t rpc_sends = 0;
	for (const KeyValue<StringName, RpcStat> &kv : rpc_stats_methods) {
		rpc_bytes += kv.value.bytes;
		rpc_sends += kv.value.sends;
		rows.push_back({ kv.key, kv.value });
	}
	std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) { return a.second.bytes > b.second.bytes; });
	if (inner->get_unique_id() == 1) {
		for (int32_t peer : server.peers()) {
			const CoreServer::PeerStats st = server.peer_stats(peer);
			UtilityFunctions::print("[goldnet rpc]   netchan peer ", peer, ": reliable ",
					(int64_t)st.messages_delivered, "/", (int64_t)st.messages_queued,
					" delivered, ", (int64_t)st.datagrams_dropped, " datagrams dropped");
		}
	}
	UtilityFunctions::print("[goldnet rpc] ", String::num(secs, 1), "s peers=", inner->get_peers().size(),
			" snapshot=", String::num(rpc_stats_snap_bytes / 1024.0 / secs, 2), " KB/s (",
			String::num(rpc_stats_snap_sends / secs, 0), " sends/s) rpc=",
			String::num(rpc_bytes / 1024.0 / secs, 2), " KB/s (", String::num(rpc_sends / secs, 0), " sends/s)");
	for (const auto &row : rows) {
		const RpcStat &s = row.second;
		UtilityFunctions::print("[goldnet rpc]   ", String::num(s.bytes / 1024.0 / secs, 2), " KB/s ",
				s.unreliable ? "unrel" : "rel  ", " ", String::num(s.sends / secs, 1), " sends/s avg ",
				(int64_t)(s.sends ? s.bytes / s.sends : 0), " B  ", String(row.first), " (", s.owner, ")");
	}
	rpc_stats_methods.clear();
	rpc_stats_snap_bytes = 0;
	rpc_stats_snap_sends = 0;
	rpc_stats_last_ms = p_now;
}

int32_t GoldNetMultiplayer::_get_remote_sender_id() const {
	return command_sender != 0 ? command_sender : inner->get_remote_sender_id();
}

Error GoldNetMultiplayer::_object_configuration_add(Object *p_object, const Variant &p_config) {
	// A spawn is reported to the MultiplayerAPI as object_configuration_add(spawned_node,
	// spawner). We already learned the reconstruction data from the wrapped spawn_function
	// (see _wrap_spawner / _spawn_trampoline), so nothing to do here but claim it — do not
	// forward to the inner.
	if (MultiplayerSpawner *sp = Object::cast_to<MultiplayerSpawner>(p_config)) {
		// Unless we never wrapped it. Then this spawn's spawn_function ran unintercepted and its
		// recipe is gone for good: the recipe is the ARGUMENT to spawn(), consumed on the way in,
		// and only the finished node survives — it cannot be read back out. So this one node will
		// not replicate. What we can do is wrap the spawner right now, so every LATER spawn through
		// it does, and say plainly what was lost.
		//
		// This is the one point the engine always tells us about: it is driven by MultiplayerSpawner
		// itself, not by our trampoline, so detection never depends on the thing that failed.
		// Server-only — a client rebuilds through _apply_spawn, which calls orig_fn and add_child
		// directly and never routes back through here.
		if (p_object != nullptr && inner->get_unique_id() == 1 && !spawners.has(sp->get_instance_id())) {
			_wrap_spawner(sp);
			ERR_PRINT(String("goldnet: spawner '") + String(sp->get_name()) +
					"' spawned before it was captured - that one node will not replicate "
					"(its spawn data is unrecoverable). Later spawns through it will. This means "
					"the spawner was used before goldnet's first poll and before a peer was set.");
		}
		return OK;
	}

	MultiplayerSynchronizer *sync = Object::cast_to<MultiplayerSynchronizer>(p_config);
	if (sync && _should_intercept(sync)) {
		uint64_t objid = sync->get_instance_id();
		uint32_t net_id = net_id_for(sync);
		owned[objid] = net_id;
		netid_to_objid[net_id] = objid;
		entities_to_add.insert(objid); // the server's core takes it once it's live (_add_entities)
		_refresh_interval(); // intervals are set before the sync enters the tree
		return OK; // we own it — do not forward to the inner
	}
	return inner->object_configuration_add(p_object, p_config);
}

Error GoldNetMultiplayer::_object_configuration_remove(Object *p_object, const Variant &p_config) {
	// Despawn is reported as object_configuration_remove(node, spawner); we detect the freed
	// node by polling instead (a node can be freed without this firing), so just claim it.
	if (Object::cast_to<MultiplayerSpawner>(p_config)) {
		return OK;
	}

	MultiplayerSynchronizer *sync = Object::cast_to<MultiplayerSynchronizer>(p_config);
	if (sync) {
		uint64_t objid = sync->get_instance_id();
		client_slot_codings.erase(objid);
		if (const uint32_t *net_id = owned.getptr(objid)) {
			netid_to_objid.erase(*net_id);
			client.forget(*net_id); // freed with no despawn to say so (a map change, say)
			owned.erase(objid);
			entities_to_add.erase(objid);
			server.remove_entity(objid);
			_refresh_interval();
			return OK;
		}
	}
	return inner->object_configuration_remove(p_object, p_config);
}
