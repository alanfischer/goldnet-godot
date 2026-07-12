#include "goldnet_link.h"

#include "goldnet_multiplayer.h"

#include <godot_cpp/classes/multiplayer_api.hpp>
#include <godot_cpp/core/class_db.hpp>

using namespace godot;

void GoldNetLink::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_gn_recv", "bytes"), &GoldNetLink::_gn_recv);
}

void GoldNetLink::_gn_recv(const PackedByteArray &p_bytes) {
	Ref<MultiplayerAPI> mp = get_multiplayer();
	GoldNetMultiplayer *gn = Object::cast_to<GoldNetMultiplayer>(mp.ptr());
	if (gn) {
		gn->apply_snapshot(p_bytes);
	}
}
