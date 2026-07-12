#include "goldnet_link.h"

#include "goldnet_multiplayer.h"

#include <godot_cpp/classes/multiplayer_api.hpp>
#include <godot_cpp/core/class_db.hpp>

using namespace godot;

void GoldNetLink::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_gn_recv", "bytes"), &GoldNetLink::_gn_recv);
	ClassDB::bind_method(D_METHOD("_gn_ack", "seq"), &GoldNetLink::_gn_ack);
}

void GoldNetLink::_gn_recv(const PackedByteArray &p_bytes) {
	Ref<MultiplayerAPI> mp = get_multiplayer();
	GoldNetMultiplayer *gn = Object::cast_to<GoldNetMultiplayer>(mp.ptr());
	if (gn) {
		gn->apply_snapshot(p_bytes);
	}
}

void GoldNetLink::_gn_ack(int p_seq) {
	Ref<MultiplayerAPI> mp = get_multiplayer();
	GoldNetMultiplayer *gn = Object::cast_to<GoldNetMultiplayer>(mp.ptr());
	if (gn) {
		gn->on_ack(gn->get_remote_sender_id(), p_seq);
	}
}
