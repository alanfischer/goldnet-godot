#include "goldnet_multiplayer.h"

#include <godot_cpp/classes/scene_multiplayer.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

GoldNetMultiplayer::GoldNetMultiplayer() {
	inner = MultiplayerAPI::create_default_interface(); // a SceneMultiplayer

	// The SceneMultiplayer replicator refuses to work with an empty root path
	// (it needs a base to resolve synchronizer/spawner node paths against). The
	// engine sets this to "/root" for the default multiplayer slot; we install
	// into that same slot in WizardWars, so mirror it here.
	SceneMultiplayer *sm = Object::cast_to<SceneMultiplayer>(inner.ptr());
	if (sm) {
		sm->set_root_path(NodePath("/root"));
	}

	// Relay the inner API's lifecycle signals up to us.
	inner->connect("peer_connected", callable_mp(this, &GoldNetMultiplayer::_relay_peer_connected));
	inner->connect("peer_disconnected", callable_mp(this, &GoldNetMultiplayer::_relay_peer_disconnected));
	inner->connect("connected_to_server", callable_mp(this, &GoldNetMultiplayer::_relay_connected_to_server));
	inner->connect("connection_failed", callable_mp(this, &GoldNetMultiplayer::_relay_connection_failed));
	inner->connect("server_disconnected", callable_mp(this, &GoldNetMultiplayer::_relay_server_disconnected));
}

GoldNetMultiplayer::~GoldNetMultiplayer() {}

void GoldNetMultiplayer::_relay_peer_connected(int64_t p_id) { emit_signal("peer_connected", p_id); }
void GoldNetMultiplayer::_relay_peer_disconnected(int64_t p_id) { emit_signal("peer_disconnected", p_id); }
void GoldNetMultiplayer::_relay_connected_to_server() { emit_signal("connected_to_server"); }
void GoldNetMultiplayer::_relay_connection_failed() { emit_signal("connection_failed"); }
void GoldNetMultiplayer::_relay_server_disconnected() { emit_signal("server_disconnected"); }

Error GoldNetMultiplayer::_poll() {
	return inner->poll();
}

void GoldNetMultiplayer::_set_multiplayer_peer(const Ref<MultiplayerPeer> &p_peer) {
	inner->set_multiplayer_peer(p_peer);
}

Ref<MultiplayerPeer> GoldNetMultiplayer::_get_multiplayer_peer() {
	return inner->get_multiplayer_peer();
}

int32_t GoldNetMultiplayer::_get_unique_id() const {
	return inner->get_unique_id();
}

PackedInt32Array GoldNetMultiplayer::_get_peer_ids() const {
	return inner->get_peers();
}

Error GoldNetMultiplayer::_rpc(int32_t p_peer, Object *p_object, const StringName &p_method, const Array &p_args) {
	return inner->rpc(p_peer, p_object, p_method, p_args);
}

int32_t GoldNetMultiplayer::_get_remote_sender_id() const {
	return inner->get_remote_sender_id();
}

Error GoldNetMultiplayer::_object_configuration_add(Object *p_object, const Variant &p_config) {
	return inner->object_configuration_add(p_object, p_config);
}

Error GoldNetMultiplayer::_object_configuration_remove(Object *p_object, const Variant &p_config) {
	return inner->object_configuration_remove(p_object, p_config);
}
