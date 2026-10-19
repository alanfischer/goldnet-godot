#pragma once

// GoldNetSimPeer — the receive side of the network-condition sim. Wraps a client's
// MultiplayerPeer and holds what arrives on it in a goldnet::NetSim before the inner
// SceneMultiplayer sees it, so a client alone can play over a slow, lossy downlink: loss drops
// unreliable packets, latency and spikes delay everything, and a link cap queues. Its sends
// pass straight through; the send leg has its own sim in GoldNetMultiplayer.
//
// GoldNetMultiplayer installs one around every client peer and hands the original back from
// multiplayer_peer, so the game never sees it. With the sim off, packets pass through in order.

#include "goldnet/netsim.h"

#include <deque>

#include <godot_cpp/classes/multiplayer_peer.hpp>
#include <godot_cpp/classes/multiplayer_peer_extension.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

namespace godot {

class GoldNetSimPeer : public MultiplayerPeerExtension {
	GDCLASS(GoldNetSimPeer, MultiplayerPeerExtension)

public:
	struct Packet {
		PackedByteArray data;
		int32_t from = 0;
		int32_t channel = 0;
		MultiplayerPeer::TransferMode mode = MultiplayerPeer::TRANSFER_MODE_RELIABLE;
	};
	typedef goldnet::NetSim<Packet> Sim;

private:
	Ref<MultiplayerPeer> wrapped;
	Sim sim;
	std::deque<Packet> ready; // released by the sim, waiting for the inner to read them
	PackedByteArray current;  // the packet the inner last read; its bytes must outlive _get_packet
	int64_t delivered_bytes = 0;
	int64_t delivered_packets = 0;

	void _deliver(Packet &&p_packet);

	void _on_peer_connected(int32_t p_id);
	void _on_peer_disconnected(int32_t p_id);

protected:
	static void _bind_methods() {}

public:
	void wrap(const Ref<MultiplayerPeer> &p_peer);
	Ref<MultiplayerPeer> get_wrapped() const { return wrapped; }
	Sim &get_sim() { return sim; }
	const Sim &get_sim() const { return sim; }
	// What the sim has let through, in total: what a meter on the far side of a slow link would
	// see, where the transport's own counters see it all arrive.
	int64_t get_delivered_bytes() const { return delivered_bytes; }
	int64_t get_delivered_packets() const { return delivered_packets; }
	~GoldNetSimPeer();

	virtual Error _get_packet(const uint8_t **r_buffer, int32_t *r_buffer_size) override;
	virtual Error _put_packet(const uint8_t *p_buffer, int32_t p_buffer_size) override;
	virtual int32_t _get_available_packet_count() const override;
	virtual int32_t _get_max_packet_size() const override;
	virtual int32_t _get_packet_channel() const override;
	virtual MultiplayerPeer::TransferMode _get_packet_mode() const override;
	virtual void _set_transfer_channel(int32_t p_channel) override;
	virtual int32_t _get_transfer_channel() const override;
	virtual void _set_transfer_mode(MultiplayerPeer::TransferMode p_mode) override;
	virtual MultiplayerPeer::TransferMode _get_transfer_mode() const override;
	virtual void _set_target_peer(int32_t p_peer) override;
	virtual int32_t _get_packet_peer() const override;
	virtual bool _is_server() const override;
	virtual void _poll() override;
	virtual void _close() override;
	virtual void _disconnect_peer(int32_t p_peer, bool p_force) override;
	virtual int32_t _get_unique_id() const override;
	virtual void _set_refuse_new_connections(bool p_enable) override;
	virtual bool _is_refusing_new_connections() const override;
	virtual bool _is_server_relay_supported() const override;
	virtual MultiplayerPeer::ConnectionStatus _get_connection_status() const override;
};

} // namespace godot
