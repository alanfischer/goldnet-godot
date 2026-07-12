#pragma once

// GoldNetMultiplayer — a drop-in Godot MultiplayerAPI that will implement
// GoldSrc/Quake-style ack-delta snapshot replication behind the stock
// MultiplayerSynchronizer / MultiplayerSpawner API.
//
// PHASE 0 (this file): a 100% pass-through. It holds an inner SceneMultiplayer
// (the engine default) and forwards every MultiplayerAPI virtual to it, so a
// consuming game plays *identically* with our API installed. This proves the
// composition boundary before any custom protocol exists. Later phases intercept
// the synchronizer/spawner configs in _object_configuration_add and drive their
// replication ourselves in _poll, while still delegating handshake, auth, peer
// lifecycle, and all @rpc traffic to the inner SceneMultiplayer.

#include <godot_cpp/classes/multiplayer_api.hpp>
#include <godot_cpp/classes/multiplayer_api_extension.hpp>
#include <godot_cpp/classes/multiplayer_peer.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>

namespace godot {

class GoldNetMultiplayer : public MultiplayerAPIExtension {
	GDCLASS(GoldNetMultiplayer, MultiplayerAPIExtension)

	// The engine's stock SceneMultiplayer, created via
	// MultiplayerAPI::create_default_interface(). All Phase-0 work delegates here.
	Ref<MultiplayerAPI> inner;

	// Re-emit the inner API's lifecycle signals on ourselves. The game connects to
	// multiplayer.peer_connected / etc. on the OUTER object (us); without relays
	// those handlers would never fire, because the inner emits on itself.
	void _relay_peer_connected(int64_t p_id);
	void _relay_peer_disconnected(int64_t p_id);
	void _relay_connected_to_server();
	void _relay_connection_failed();
	void _relay_server_disconnected();

protected:
	static void _bind_methods() {}

public:
	GoldNetMultiplayer();
	~GoldNetMultiplayer();

	// MultiplayerAPIExtension virtuals — all forwarded to `inner` in Phase 0.
	virtual Error _poll() override;
	virtual void _set_multiplayer_peer(const Ref<MultiplayerPeer> &p_peer) override;
	virtual Ref<MultiplayerPeer> _get_multiplayer_peer() override;
	virtual int32_t _get_unique_id() const override;
	virtual PackedInt32Array _get_peer_ids() const override;
	virtual Error _rpc(int32_t p_peer, Object *p_object, const StringName &p_method, const Array &p_args) override;
	virtual int32_t _get_remote_sender_id() const override;
	virtual Error _object_configuration_add(Object *p_object, const Variant &p_config) override;
	virtual Error _object_configuration_remove(Object *p_object, const Variant &p_config) override;
};

} // namespace godot
