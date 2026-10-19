#include "goldnet_sim_peer.h"

#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>

using namespace godot;

void GoldNetSimPeer::wrap(const Ref<MultiplayerPeer> &p_peer) {
	wrapped = p_peer;
	wrapped->connect("peer_connected", callable_mp(this, &GoldNetSimPeer::_on_peer_connected));
	wrapped->connect("peer_disconnected", callable_mp(this, &GoldNetSimPeer::_on_peer_disconnected));
}

GoldNetSimPeer::~GoldNetSimPeer() {
	if (wrapped.is_valid()) {
		wrapped->disconnect("peer_connected", callable_mp(this, &GoldNetSimPeer::_on_peer_connected));
		wrapped->disconnect("peer_disconnected", callable_mp(this, &GoldNetSimPeer::_on_peer_disconnected));
	}
}

void GoldNetSimPeer::_on_peer_connected(int32_t p_id) {
	emit_signal("peer_connected", p_id);
}

// Whatever the peer had in flight goes with it, as it would on a real link.
void GoldNetSimPeer::_on_peer_disconnected(int32_t p_id) {
	sim.forget_link(p_id);
	for (auto it = ready.begin(); it != ready.end();) {
		it = it->from == p_id ? ready.erase(it) : it + 1;
	}
	emit_signal("peer_disconnected", p_id);
}

void GoldNetSimPeer::_poll() {
	wrapped->poll();
	const uint64_t now = Time::get_singleton()->get_ticks_msec();
	const bool active = sim.loss_percent() > 0 || sim.latency_max_ms() > 0 || sim.spike_ms() > 0 || sim.bandwidth_bps() > 0;
	while (wrapped->get_available_packet_count() > 0) {
		Packet p;
		p.from = wrapped->get_packet_peer();
		p.channel = wrapped->get_packet_channel();
		p.mode = wrapped->get_packet_mode();
		p.data = wrapped->get_packet();
		// Off, and nothing still held that this could overtake: straight through.
		if (!active && sim.pending() == 0) {
			_deliver(std::move(p));
			continue;
		}
		// The transport has already retransmitted a reliable packet into place, so only an
		// unreliable one may be lost; a reliable one queues however full the link is.
		const bool droppable = p.mode != MultiplayerPeer::TRANSFER_MODE_RELIABLE;
		if (droppable && sim.loss_roll()) {
			continue;
		}
		const int d = sim.schedule(now, p.from, (size_t)p.data.size(), droppable);
		if (d < 0) {
			continue;
		}
		sim.enqueue(now, p.from, d, std::move(p));
	}
	sim.pump(now, [this](Packet &p) { _deliver(std::move(p)); });
}

void GoldNetSimPeer::_deliver(Packet &&p_packet) {
	delivered_bytes += p_packet.data.size();
	delivered_packets++;
	ready.push_back(std::move(p_packet));
}

Error GoldNetSimPeer::_get_packet(const uint8_t **r_buffer, int32_t *r_buffer_size) {
	if (ready.empty()) {
		return ERR_UNAVAILABLE;
	}
	current = std::move(ready.front().data);
	ready.pop_front();
	*r_buffer = current.ptr();
	*r_buffer_size = (int32_t)current.size();
	return OK;
}

Error GoldNetSimPeer::_put_packet(const uint8_t *p_buffer, int32_t p_buffer_size) {
	PackedByteArray bytes;
	bytes.resize(p_buffer_size);
	memcpy(bytes.ptrw(), p_buffer, (size_t)p_buffer_size);
	return wrapped->put_packet(bytes);
}

int32_t GoldNetSimPeer::_get_available_packet_count() const {
	return (int32_t)ready.size();
}

// ENet's own ceiling; the wrapped peer doesn't expose its.
int32_t GoldNetSimPeer::_get_max_packet_size() const {
	return 1 << 25;
}

int32_t GoldNetSimPeer::_get_packet_channel() const {
	return ready.empty() ? 0 : ready.front().channel;
}

MultiplayerPeer::TransferMode GoldNetSimPeer::_get_packet_mode() const {
	return ready.empty() ? MultiplayerPeer::TRANSFER_MODE_RELIABLE : ready.front().mode;
}

int32_t GoldNetSimPeer::_get_packet_peer() const {
	return ready.empty() ? 0 : ready.front().from;
}

void GoldNetSimPeer::_set_transfer_channel(int32_t p_channel) {
	wrapped->set_transfer_channel(p_channel);
}

int32_t GoldNetSimPeer::_get_transfer_channel() const {
	return wrapped->get_transfer_channel();
}

void GoldNetSimPeer::_set_transfer_mode(MultiplayerPeer::TransferMode p_mode) {
	wrapped->set_transfer_mode(p_mode);
}

MultiplayerPeer::TransferMode GoldNetSimPeer::_get_transfer_mode() const {
	return wrapped->get_transfer_mode();
}

void GoldNetSimPeer::_set_target_peer(int32_t p_peer) {
	wrapped->set_target_peer(p_peer);
}

bool GoldNetSimPeer::_is_server() const {
	return wrapped->get_unique_id() == 1;
}

void GoldNetSimPeer::_close() {
	ready.clear();
	sim.forget_link(1);
	wrapped->close();
}

void GoldNetSimPeer::_disconnect_peer(int32_t p_peer, bool p_force) {
	wrapped->disconnect_peer(p_peer, p_force);
}

int32_t GoldNetSimPeer::_get_unique_id() const {
	return wrapped->get_unique_id();
}

void GoldNetSimPeer::_set_refuse_new_connections(bool p_enable) {
	wrapped->set_refuse_new_connections(p_enable);
}

bool GoldNetSimPeer::_is_refusing_new_connections() const {
	return wrapped->is_refusing_new_connections();
}

bool GoldNetSimPeer::_is_server_relay_supported() const {
	return wrapped->is_server_relay_supported();
}

MultiplayerPeer::ConnectionStatus GoldNetSimPeer::_get_connection_status() const {
	return wrapped->get_connection_status();
}
