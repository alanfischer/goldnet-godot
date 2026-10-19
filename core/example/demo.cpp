// goldnet's core over plain UDP, no Godot: a server bounces balls around a box, spawning and
// despawning them, and any number of clients draw what the snapshot stream tells them. Each client
// also sends the server a command every 16 ms over the uplink, the way Quake sends usercmds, and
// the server reports whether every one arrived in order.
//
//   ./goldnet_demo server [--port 27500] [--loss PCT] [--latency MS] [--rate BYTES_PER_SEC]
//   ./goldnet_demo client [--host 127.0.0.1] [--port 27500] [--loss PCT] [--latency MS]
//
// --loss and --latency apply to whichever end they're given to: the server's snapshots, or the
// client's uplink. Client -> server datagrams are 'H' (hello: start a session) or 'U' + an uplink
// packet (snapshot ack and commands). Server -> client datagrams are goldnet snapshots, unframed.

#include "goldnet/client.h"
#include "goldnet/netsim.h"
#include "goldnet/server.h"
#include "goldnet/uplink.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <variant>

using namespace goldnet;

typedef std::variant<int64_t, float> Value;

static const int W = 60;
static const int H = 18;
static const uint32_t TICK_MS = 33;
static const uint32_t TIMEOUT_MS = 3000;
static const uint32_t SCOREBOARD_ID = 1; // a map-static entity: both ends know it, no spawn needed
static const uint32_t CMD_MS = 16;
static const uint32_t CMD_STREAM = 1;

// Ball slots: x, y, heading, bounces.
static const int BALL_SLOTS = 4;
static const std::vector<uint8_t> BALL_QUANT = { TAG_HALF, TAG_HALF, TAG_ANGLE16, QUANT_AUTO };

static uint32_t now_ms() {
	static const auto start = std::chrono::steady_clock::now();
	return 1 + (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
					   std::chrono::steady_clock::now() - start)
					   .count();
}

static float as_float(const Value &v) {
	return std::holds_alternative<float>(v) ? std::get<float>(v) : (float)std::get<int64_t>(v);
}

static int64_t as_int(const Value &v) {
	return std::holds_alternative<int64_t>(v) ? std::get<int64_t>(v) : (int64_t)std::get<float>(v);
}

static void encode_value(Writer &w, const Value &v, uint8_t quant, uint32_t) {
	if (std::holds_alternative<int64_t>(v)) {
		w.put_u8(TAG_INT);
		put_varint(&w, std::get<int64_t>(v));
		return;
	}
	const float f = std::get<float>(v);
	switch (quant) {
		case TAG_HALF:
			w.put_u8(TAG_HALF);
			w.put_half(f);
			break;
		case TAG_ANGLE16:
			w.put_u8(TAG_ANGLE16);
			put_angle16(&w, f);
			break;
		default:
			w.put_u8(TAG_FLOAT);
			w.put_float(f);
	}
}

static Value decode_value(Reader &r, uint32_t) {
	switch (r.get_u8()) {
		case TAG_INT:
			return get_varint(&r);
		case TAG_FLOAT:
			return r.get_float();
		case TAG_HALF:
			return r.get_half();
		case TAG_ANGLE16:
			return get_angle16(&r);
		default:
			r.get_data((size_t)-1); // a tag this demo never sends: fail the packet
			return Value();
	}
}

static int open_socket(uint16_t port) {
	const int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		perror("socket");
		exit(1);
	}
	sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(port);
	if (bind(fd, (sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("bind");
		exit(1);
	}
	return fd;
}

static void wait_readable(int fd, int ms) {
	pollfd p = { fd, POLLIN, 0 };
	poll(&p, 1, ms < 0 ? 0 : ms);
}

static std::string addr_key(const sockaddr_in &a) {
	char buf[64];
	snprintf(buf, sizeof(buf), "%s:%d", inet_ntoa(a.sin_addr), ntohs(a.sin_port));
	return buf;
}

// --- server ---

struct Ball {
	char letter;
	float x, y, vx, vy;
	int64_t bounces = 0;
};

struct Outgoing {
	sockaddr_in to;
	std::vector<uint8_t> bytes;
};

typedef Server<Value> DemoServer;

struct ServerApp {
	int fd = -1;
	DemoServer server;
	NetSim<Outgoing> sim;
	std::map<uint64_t, Ball> balls; // key == net_id
	std::vector<uint64_t> spawn_order;
	int64_t total_bounces = 0;
	uint32_t next_net_id = 100;
	char next_letter = 'A';
	uint32_t rate = 0;

	struct PeerInfo {
		sockaddr_in addr;
		uint32_t last_heard;
		uint32_t commands = 0;     // since the last report
		uint32_t out_of_order = 0; // ever: should stay 0
		uint32_t expect = 0;
	};
	CommandReceiver commands;
	std::map<int32_t, PeerInfo> peers;
	std::map<std::string, int32_t> by_addr;
	int32_t next_peer = 1;
	size_t bytes_out = 0;
	int packets_out = 0;

	// The host interface Server::tick() calls back into.
	bool live(DemoServer::Entity &e) { return e.key == SCOREBOARD_ID || balls.count(e.key); }
	void prepare(DemoServer::Entity &e) {
		if (e.key == SCOREBOARD_ID) {
			e.slots = 2; // live balls, total bounces
		} else {
			e.slots = BALL_SLOTS;
			e.quant = BALL_QUANT;
		}
	}
	void read(DemoServer::Entity &e, bool) {
		if (e.key == SCOREBOARD_ID) {
			server.stamp(e, 0, Value((int64_t)balls.size()));
			server.stamp(e, 1, Value(total_bounces));
			return;
		}
		const Ball &b = balls[e.key];
		server.stamp(e, 0, Value(b.x));
		server.stamp(e, 1, Value(b.y));
		server.stamp(e, 2, Value(atan2f(b.vy, b.vx)));
		server.stamp(e, 3, Value(b.bounces));
	}
	bool visible(const DemoServer::Entity &, int32_t) { return true; }
	bool spawn_visible(uint64_t, int32_t) { return true; }
	void encode(Writer &w, const Value &v, uint8_t quant, uint32_t now) { encode_value(w, v, quant, now); }
	void send(int32_t peer, const Writer &packet) {
		Outgoing o{ peers[peer].addr, std::vector<uint8_t>(packet.data(), packet.data() + packet.size()) };
		bytes_out += o.bytes.size();
		packets_out++;
		if (sim.loss_roll()) {
			return;
		}
		const uint32_t now = now_ms();
		const int delay = sim.schedule(now, peer, o.bytes.size(), true);
		if (delay < 0) {
			return;
		} else if (delay == 0) {
			transmit(o);
		} else {
			sim.enqueue(now, peer, delay, std::move(o));
		}
	}

	// The host interface CommandReceiver::receive() calls back into.
	void protocol_mismatch(int32_t peer) { fprintf(stderr, "[server] peer %d speaks a different version\n", peer); }
	void snapshot_ack(int32_t peer, uint16_t seq) { server.on_ack(peer, seq, now_ms()); }
	void channel_ack(int32_t peer, const ChannelAck &ack) { server.on_channel_ack(peer, ack); }
	void message(int32_t, const uint8_t *, size_t, bool) {}
	void command(int32_t peer, uint32_t, const uint8_t *data, size_t size) {
		PeerInfo &p = peers[peer];
		const uint32_t n = size >= 4 ? data[0] | (data[1] << 8) | (data[2] << 16) | ((uint32_t)data[3] << 24) : 0;
		if (p.commands + p.out_of_order > 0 && n != p.expect) {
			p.out_of_order++;
		}
		p.expect = n + 1;
		p.commands++;
	}

	void transmit(const Outgoing &o) {
		sendto(fd, o.bytes.data(), o.bytes.size(), 0, (const sockaddr *)&o.to, sizeof(o.to));
	}

	void spawn_ball() {
		const uint32_t id = next_net_id++;
		const float angle = (float)(rand() % 628) / 100.0f;
		const float speed = 8.0f + (float)(rand() % 80) / 10.0f;
		Ball b{ next_letter, (float)(W / 2), (float)(H / 2), cosf(angle) * speed, sinf(angle) * speed * 0.5f };
		next_letter = next_letter == 'Z' ? 'A' : next_letter + 1;
		balls[id] = b;
		spawn_order.push_back(id);

		DemoServer::SpawnRecord rec;
		rec.node_key = id;
		rec.data = { (uint8_t)b.letter };
		server.add_spawn(id, rec);
		server.add_entity(id, id);
	}

	void despawn_oldest() {
		const uint64_t id = spawn_order.front();
		spawn_order.erase(spawn_order.begin());
		balls.erase(id);
		server.remove_entity(id);
		server.despawn((uint32_t)id);
	}

	void simulate(float dt) {
		for (auto &kv : balls) {
			Ball &b = kv.second;
			b.x += b.vx * dt;
			b.y += b.vy * dt;
			if (b.x < 0 || b.x >= W) {
				b.vx = -b.vx;
				b.x = b.x < 0 ? -b.x : 2 * (W - 0.01f) - b.x;
				b.bounces++;
				total_bounces++;
			}
			if (b.y < 0 || b.y >= H) {
				b.vy = -b.vy;
				b.y = b.y < 0 ? -b.y : 2 * (H - 0.01f) - b.y;
				b.bounces++;
				total_bounces++;
			}
		}
	}

	void receive(uint32_t now) {
		uint8_t buf[1500];
		sockaddr_in from;
		socklen_t len = sizeof(from);
		ssize_t n;
		while ((n = recvfrom(fd, buf, sizeof(buf), MSG_DONTWAIT, (sockaddr *)&from, &len)) > 0) {
			const std::string key = addr_key(from);
			auto it = by_addr.find(key);
			if (buf[0] == 'H') {
				if (it != by_addr.end()) {
					drop_peer(it->second, "rejoined");
				}
				const int32_t id = next_peer++;
				peers[id] = PeerInfo{ from, now };
				by_addr[key] = id;
				if (rate) {
					server.set_peer_rate(id, rate);
				}
				printf("[server] peer %d joined from %s\n", id, key.c_str());
			} else if (buf[0] == 'U' && it != by_addr.end()) {
				peers[it->second].last_heard = now;
				commands.receive(it->second, buf + 1, (size_t)n - 1, *this, now_ms());
				server.set_command_ack(it->second, commands.ack(it->second), commands.take_news(it->second));
				server.set_uplink_ack(it->second, commands.channel_ack(it->second));
			}
			len = sizeof(from);
		}
	}

	void drop_peer(int32_t id, const char *why) {
		printf("[server] peer %d %s\n", id, why);
		by_addr.erase(addr_key(peers[id].addr));
		peers.erase(id);
		server.remove_peer(id);
		commands.remove_peer(id);
		sim.forget_link(id);
	}

	int run(uint16_t port) {
		fd = open_socket(port);
		server.set_packet_overhead(28); // IPv4 + UDP headers
		server.add_entity(SCOREBOARD_ID, SCOREBOARD_ID);
		printf("[server] listening on udp %d (loss %d%%, latency %d ms, rate %u B/s)\n", port,
				sim.loss_percent(), sim.latency_max_ms(), rate);

		uint32_t next_tick = now_ms();
		uint32_t next_spawn = next_tick;
		uint32_t next_report = next_tick + 1000;
		for (;;) {
			uint32_t now = now_ms();
			receive(now);
			sim.pump(now, [&](Outgoing &o) { transmit(o); });

			if ((int32_t)(now - next_tick) >= 0) {
				simulate(TICK_MS / 1000.0f);
				if ((int32_t)(now - next_spawn) >= 0) {
					if (balls.size() >= 6) {
						despawn_oldest();
					}
					spawn_ball();
					next_spawn += 2000;
				}
				std::vector<int32_t> ids;
				for (auto it = peers.begin(); it != peers.end();) {
					const int32_t id = it->first;
					++it;
					if (now - peers[id].last_heard > TIMEOUT_MS) {
						drop_peer(id, "timed out");
					} else {
						ids.push_back(id);
					}
				}
				server.tick(now, ids, *this);
				next_tick += TICK_MS;
			}

			if ((int32_t)(now - next_report) >= 0) {
				printf("[server] %zu peers, %zu balls, %d snapshots/s, %.1f KB/s out\n", peers.size(),
						balls.size(), packets_out, bytes_out / 1024.0);
				for (auto &kv : peers) {
					printf("[server]   peer %d: %u commands/s, %u out of order, %u given up on", kv.first,
							kv.second.commands, kv.second.out_of_order, commands.skipped(kv.first));
					if (rate) {
						printf(", choked %u times", server.chokes(kv.first));
					}
					printf("\n");
					kv.second.commands = 0;
				}
				bytes_out = 0;
				packets_out = 0;
				next_report += 1000;
			}
			wait_readable(fd, (int)(int32_t)(next_tick - now_ms()));
		}
	}
};

// --- client ---

struct ClientApp {
	int fd = -1;
	sockaddr_in server_addr = {};
	Client<Value> client;
	CommandSender commands;
	NetSim<std::vector<uint8_t>> sim; // on the uplink
	uint16_t snapshot_ack = 0;
	uint32_t next_command = 0;

	struct Ent {
		char letter;
		std::vector<Value> vals;
	};
	std::map<uint32_t, Ent> ents;
	uint32_t cur = 0;
	uint32_t server_ms = 0;
	int spawns = 0, despawns = 0, snapshots = 0, committed = 0;
	size_t bytes_in = 0;

	// The host interface Client::apply() calls back into.
	void protocol_mismatch() { fprintf(stderr, "[client] server speaks a different snapshot version\n"); }
	void server_time(uint32_t ms) { server_ms = ms; }
	void command_ack(uint16_t seq) { commands.on_ack(seq); }
	void channel_ack(const ChannelAck &ack) { commands.on_channel_ack(ack, now_ms()); }
	void message(const uint8_t *, size_t, bool) {}
	void send_ack(uint16_t seq) { snapshot_ack = seq; } // rides the next uplink packet
	bool spawn(uint32_t net_id, uint32_t, const uint8_t *data, size_t size) {
		ents[net_id] = Ent{ size ? (char)data[0] : '?', std::vector<Value>(BALL_SLOTS) };
		spawns++;
		return true;
	}
	void despawn(uint32_t net_id) {
		ents.erase(net_id);
		despawns++;
	}
	void relevance_lost(uint32_t) {}
	bool registered(uint32_t net_id) { return ents.count(net_id) > 0; }
	int begin_entity(uint32_t net_id, uint32_t, bool &) {
		cur = net_id;
		auto it = ents.find(net_id);
		return it == ents.end() ? -1 : (int)it->second.vals.size();
	}
	bool set_slot(int slot, const Value &v) {
		ents[cur].vals[slot] = v;
		return true;
	}
	void end_entity(bool) {}
	Value decode(Reader &r, uint32_t ref_time_ms) { return decode_value(r, ref_time_ms); }

	void transmit(const std::vector<uint8_t> &pkt) {
		sendto(fd, pkt.data(), pkt.size(), 0, (sockaddr *)&server_addr, sizeof(server_addr));
	}

	// This tick's command: its number and a few bytes that rarely change, which the uplink's
	// delta coding makes nearly free.
	void make_command(uint32_t now) {
		const uint32_t n = next_command++;
		const uint8_t cmd[8] = { (uint8_t)n, (uint8_t)(n >> 8), (uint8_t)(n >> 16), (uint8_t)(n >> 24), 1, 2, 3, 4 };
		commands.push(CMD_STREAM, cmd, sizeof(cmd), now);
		if (!commands.due(now)) {
			return;
		}
		Writer w;
		w.put_u8('U');
		commands.write(now, snapshot_ack, client.channel_ack(), w);
		if (sim.loss_roll()) {
			return;
		}
		std::vector<uint8_t> pkt(w.data(), w.data() + w.size());
		const int delay = sim.schedule(now, 0, pkt.size(), true);
		if (delay == 0) {
			transmit(pkt);
		} else if (delay > 0) {
			sim.enqueue(now, 0, delay, std::move(pkt));
		}
	}

	void new_session() {
		client.reset();
		commands.reset();
		snapshot_ack = 0;
		next_command = 0;
		ents.clear();
		ents[SCOREBOARD_ID] = Ent{ '#', std::vector<Value>(2) };
		const uint8_t hello = 'H';
		sendto(fd, &hello, 1, 0, (sockaddr *)&server_addr, sizeof(server_addr));
	}

	void draw(bool tty) {
		std::vector<std::string> grid(H, std::string(W, ' '));
		for (auto &kv : ents) {
			if (kv.first == SCOREBOARD_ID) {
				continue;
			}
			const int x = (int)as_float(kv.second.vals[0]);
			const int y = (int)as_float(kv.second.vals[1]);
			if (x >= 0 && x < W && y >= 0 && y < H) {
				grid[y][x] = kv.second.letter;
			}
		}
		const Ent &score = ents[SCOREBOARD_ID];
		char cmds[96];
		snprintf(cmds, sizeof(cmds), "%u commands sent, %zu unacked, rtt %u ms", next_command, commands.pending(),
				commands.rtt_ms());
		if (!tty) {
			printf("[client] t=%.1fs %d snapshots (%d committed), %.1f KB/s, %d spawns, %d despawns, "
				   "%lld balls, %lld bounces; %s\n",
					server_ms / 1000.0, snapshots, committed, bytes_in / 1024.0, spawns, despawns,
					(long long)as_int(score.vals[0]), (long long)as_int(score.vals[1]), cmds);
			return;
		}
		std::string out = "\x1b[H\x1b[2J";
		out += "+" + std::string(W, '-') + "+\n";
		for (const std::string &row : grid) {
			out += "|" + row + "|\n";
		}
		out += "+" + std::string(W, '-') + "+\n";
		char line[160];
		snprintf(line, sizeof(line), "server t=%.1fs  balls=%lld  bounces=%lld  spawns=%d  despawns=%d\n",
				server_ms / 1000.0, (long long)as_int(score.vals[0]), (long long)as_int(score.vals[1]), spawns,
				despawns);
		out += line;
		out += std::string("uplink: ") + cmds + "\n";
		for (auto &kv : ents) {
			if (kv.first == SCOREBOARD_ID) {
				continue;
			}
			const std::vector<Value> &v = kv.second.vals;
			snprintf(line, sizeof(line), "  %c  net_id %u  x %6.2f  y %6.2f  heading %4.0f deg  bounces %lld\n",
					kv.second.letter, kv.first, as_float(v[0]), as_float(v[1]), as_float(v[2]) * 57.2958f,
					(long long)as_int(v[3]));
			out += line;
		}
		fputs(out.c_str(), stdout);
		fflush(stdout);
	}

	int run(const char *host, uint16_t port) {
		addrinfo hints = {}, *res = nullptr;
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_DGRAM;
		if (getaddrinfo(host, nullptr, &hints, &res) != 0 || !res) {
			fprintf(stderr, "[client] can't resolve %s\n", host);
			return 1;
		}
		server_addr = *(sockaddr_in *)res->ai_addr;
		server_addr.sin_port = htons(port);
		freeaddrinfo(res);
		fd = open_socket(0);

		const bool tty = isatty(STDOUT_FILENO);
		const uint32_t draw_ms = tty ? 100 : 1000;
		uint32_t last_heard = 0;
		uint32_t next_hello = now_ms() + 500;
		uint32_t next_draw = now_ms() + draw_ms;
		uint32_t next_cmd = now_ms();
		new_session();
		printf("[client] saying hello to %s:%d\n", host, port);

		uint8_t buf[1500];
		for (;;) {
			ssize_t n;
			while ((n = recvfrom(fd, buf, sizeof(buf), MSG_DONTWAIT, nullptr, nullptr)) > 0) {
				if (last_heard == 0 && !tty) {
					printf("[client] connected\n");
				}
				last_heard = now_ms();
				snapshots++;
				bytes_in += (size_t)n;
				if (client.apply(buf, (size_t)n, *this, now_ms()).committed) {
					committed++;
				}
			}
			const uint32_t now = now_ms();
			sim.pump(now, [&](std::vector<uint8_t> &pkt) { transmit(pkt); });
			if (last_heard != 0 && (int32_t)(now - next_cmd) >= 0) {
				make_command(now);
				next_cmd += CMD_MS;
				if ((int32_t)(now - next_cmd) >= 0) {
					next_cmd = now + CMD_MS; // stalled: don't burst to catch up
				}
			}
			if (last_heard == 0 && (int32_t)(now - next_hello) >= 0) {
				new_session(); // no server yet: keep knocking
				next_hello = now + 500;
			} else if (last_heard != 0 && now - last_heard > TIMEOUT_MS) {
				printf("[client] server went quiet; starting over\n");
				last_heard = 0;
			}
			if ((int32_t)(now - next_draw) >= 0) {
				draw(tty);
				snapshots = committed = 0;
				bytes_in = 0;
				next_draw += draw_ms;
			}
			const uint32_t wake = (int32_t)(next_cmd - next_draw) < 0 && last_heard != 0 ? next_cmd : next_draw;
			wait_readable(fd, (int)(int32_t)(wake - now_ms()));
		}
	}
};

static void usage() {
	fprintf(stderr,
			"usage: goldnet_demo server [--port N] [--loss PCT] [--latency MS] [--rate BYTES_PER_SEC]\n"
			"       goldnet_demo client [--host H] [--port N] [--loss PCT] [--latency MS]\n");
	exit(2);
}

int main(int argc, char **argv) {
	if (argc < 2) {
		usage();
	}
	setvbuf(stdout, nullptr, _IOLBF, 0);
	const std::string mode = argv[1];
	uint16_t port = 27500;
	const char *host = "127.0.0.1";
	static ServerApp srv;
	static ClientApp cl;
	for (int i = 2; i < argc; i++) {
		const std::string a = argv[i];
		if (i + 1 >= argc) {
			usage();
		}
		const char *v = argv[++i];
		if (a == "--port") {
			port = (uint16_t)atoi(v);
		} else if (a == "--host") {
			host = v;
		} else if (a == "--loss") {
			srv.sim.set_loss_percent(atoi(v));
			cl.sim.set_loss_percent(atoi(v));
		} else if (a == "--latency") {
			srv.sim.set_latency_min_ms(atoi(v));
			srv.sim.set_latency_max_ms(atoi(v));
			cl.sim.set_latency_min_ms(atoi(v));
			cl.sim.set_latency_max_ms(atoi(v));
		} else if (a == "--rate") {
			srv.rate = (uint32_t)atoi(v);
		} else {
			usage();
		}
	}
	if (mode == "server") {
		return srv.run(port);
	}
	if (mode == "client") {
		return cl.run(host, port);
	}
	usage();
}
