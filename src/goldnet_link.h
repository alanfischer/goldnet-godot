#pragma once

// GoldNetLink — a tiny persistent Node that carries goldnet's snapshot packets
// over the inner SceneMultiplayer's RPC transport. goldnet replaces the *state
// replication* layer but keeps delegating handshake/auth/peer-lifecycle/@rpc to
// the inner SceneMultiplayer (see GoldNetMultiplayer). Rather than fight the
// inner for raw packets off the MultiplayerPeer (SceneMultiplayer consumes every
// packet on every channel in its poll), we ship each snapshot as one unreliable
// RPC to this node. It lives at a fixed path ("/root/__GoldNetLink") on both
// server and client so the inner's path cache resolves it, and forwards received
// bytes back into the GoldNetMultiplayer to apply.

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

namespace godot {

class GoldNetLink : public Node {
	GDCLASS(GoldNetLink, Node)

protected:
	static void _bind_methods();

public:
	// Unreliable RPC target: server → client snapshot delivery.
	void _gn_recv(const PackedByteArray &p_bytes);
};

} // namespace godot
