// Module sender_main.d - Sender Process Entry Point for Low-Latency Transport
module sender_main;

import core.stdc.stdio : printf, fprintf, stderr, fflush;
import core.stdc.stdlib : exit, atoi, atof, rand;
import core.stdc.string : strcmp, memcpy;
import core.sys.posix.unistd : usleep;
import core.sys.posix.sched : sched_yield;
import core.sys.posix.netinet.in_ : sockaddr_in, htons, inet_addr;
import core.sys.posix.arpa.inet : inet_ntoa;
import core.sys.posix.pthread : pthread_create, pthread_t;
import core.atomic : atomicLoad, atomicStore, MemoryOrder;
import core.sys.posix.time : clock_gettime, CLOCK_REALTIME, timespec;
import wire_protocol;
import shm_client;
import fec;
import socket;
import sha256;
import preflight_check;

@nogc:
nothrow:

struct DestEndpoint {
    char[64] ip;
    ushort port;
}

struct Config {
    const(char)* shm_name = "/fanout_ring";
    uint slots = 65536;
    DestEndpoint[16] dests;
    uint num_dests = 0;
    ushort default_port = 9000;
    ushort nak_port = 9002;
    bool enable_nak = true;
    ushort fec_k = 16;
    double drop_rate = 0.0;
    ushort echo_listen_port = 9001; // Default to port 9001 for zero-config multi-host RTT testing
    ulong count = 0; // 0 = infinite / until stopped
    bool enable_sha256 = false;
}

ulong nowNs() @nogc nothrow {
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return cast(ulong)ts.tv_sec * 1_000_000_000UL + cast(ulong)ts.tv_nsec;
}

void addDest(Config* cfg, const(char)* str, ushort default_port) @nogc nothrow {
    if (cfg.num_dests >= 16) return;
    import core.stdc.string : strchr, strncpy;
    const(char)* colon = strchr(str, ':');
    if (colon) {
        size_t ip_len = cast(size_t)(colon - str);
        if (ip_len >= 63) ip_len = 63;
        strncpy(cfg.dests[cfg.num_dests].ip.ptr, str, ip_len);
        cfg.dests[cfg.num_dests].ip[ip_len] = '\0';
        cfg.dests[cfg.num_dests].port = cast(ushort)atoi(colon + 1);
    } else {
        strncpy(cfg.dests[cfg.num_dests].ip.ptr, str, 63);
        cfg.dests[cfg.num_dests].ip[63] = '\0';
        cfg.dests[cfg.num_dests].port = default_port;
    }
    cfg.num_dests++;
}

void parseDestArg(Config* cfg, const(char)* arg) @nogc nothrow {
    import core.stdc.string : strchr, strncpy;
    const(char)* cur = arg;
    while (*cur) {
        const(char)* comma = strchr(cur, ',');
        if (comma) {
            char[128] token;
            size_t tlen = cast(size_t)(comma - cur);
            if (tlen >= 127) tlen = 127;
            strncpy(token.ptr, cur, tlen);
            token[tlen] = '\0';
            addDest(cfg, token.ptr, cfg.default_port);
            cur = comma + 1;
        } else {
            addDest(cfg, cur, cfg.default_port);
            break;
        }
    }
}

void parseArgs(int argc, char** argv, Config* cfg) @nogc nothrow {
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--shm") == 0 && i + 1 < argc) {
            cfg.shm_name = argv[++i];
        } else if (strcmp(argv[i], "--slots") == 0 && i + 1 < argc) {
            cfg.slots = cast(uint)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            cfg.default_port = cast(ushort)atoi(argv[++i]);
            for (uint d = 0; d < cfg.num_dests; ++d) {
                if (cfg.dests[d].port == 9000) cfg.dests[d].port = cfg.default_port;
            }
        } else if (strcmp(argv[i], "--dest") == 0 && i + 1 < argc) {
            parseDestArg(cfg, argv[++i]);
        } else if (strcmp(argv[i], "--nak-port") == 0 && i + 1 < argc) {
            cfg.nak_port = cast(ushort)atoi(argv[++i]);
            if (cfg.nak_port == 0) cfg.enable_nak = false;
        } else if (strcmp(argv[i], "--no-nak") == 0) {
            cfg.enable_nak = false;
            cfg.nak_port = 0;
        } else if (strcmp(argv[i], "--fec") == 0 && i + 1 < argc) {
            cfg.fec_k = cast(ushort)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--no-fec") == 0) {
            cfg.fec_k = 0;
        } else if (strcmp(argv[i], "--drop-rate") == 0 && i + 1 < argc) {
            cfg.drop_rate = atof(argv[++i]);
        } else if (strcmp(argv[i], "--echo-listen") == 0 && i + 1 < argc) {
            cfg.echo_listen_port = cast(ushort)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--count") == 0 && i + 1 < argc) {
            cfg.count = cast(ulong)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--sha256") == 0) {
            cfg.enable_sha256 = true;
        }
    }
    if (cfg.num_dests == 0) {
        addDest(cfg, "127.0.0.1", cfg.default_port);
    }
}

// In-Memory Replay Buffer for Targeted Unicast NAK Retransmissions
enum uint REPLAY_RING_SIZE = 65536;
enum uint REPLAY_RING_MASK = REPLAY_RING_SIZE - 1;

align(64) struct ReplaySlot {
    ubyte[256] payload;
    uint len;
    ulong seq_id;
}

__gshared ReplaySlot[REPLAY_RING_SIZE] g_replay_ring;
__gshared UdpSocket g_nak_sock;
__gshared ulong g_total_naks_serviced = 0;

extern (C) void* nakWorkerThread(void* arg) @nogc nothrow {
    ubyte[256] nak_buf;
    while (true) {
        sockaddr_in requester_addr;
        long nbytes = g_nak_sock.recv(nak_buf.ptr, nak_buf.sizeof, &requester_addr);
        if (nbytes >= NakReqWire.sizeof) {
            auto req = cast(const NakReqWire*)nak_buf.ptr;
            if (req.header.type == TYPE_NAK_REQ) {
                for (ulong s = req.start_seq; s <= req.end_seq; ++s) {
                    ReplaySlot* slot = &g_replay_ring[s & REPLAY_RING_MASK];
                    if (slot.seq_id == s && slot.len > 0) {
                        // Resend directly to requester via Targeted Unicast UDP
                        g_nak_sock.sendTo(slot.payload.ptr, slot.len, &requester_addr);
                        g_total_naks_serviced++;
                    }
                }
            }
        } else {
            sched_yield();
        }
    }
    return null;
}

void processEchoPacket(const ubyte* buf, ulong* rtt_count, ulong* rtt_sum, ulong* rtt_min, ulong* rtt_max) @nogc nothrow {
    ulong now = nowNs();
    auto echo_hdr = cast(const CompactWireHeader*)buf;
    ulong rtt_ns = now >= echo_hdr.send_ts_ns ? (now - echo_hdr.send_ts_ns) : 0;
    ulong one_way_lat = rtt_ns / 2;

    (*rtt_count)++;
    *rtt_sum += one_way_lat;
    if (one_way_lat < *rtt_min) *rtt_min = one_way_lat;
    if (one_way_lat > *rtt_max) *rtt_max = one_way_lat;

    if (*rtt_count % 10000 == 0) {
        printf("[RTT Eval] echoed %llu msgs: 1-way lat (ns): min=%llu mean=%llu max=%llu\n",
               *rtt_count, *rtt_min, *rtt_sum / *rtt_count, *rtt_max);
        fflush(null);
    }
}

extern (C) int main(int argc, char** argv) @nogc nothrow {
    Config cfg;
    parseArgs(argc, argv, &cfg);

    runPreflightChecks("sender");

    printf("[sender] attaching to shm segment '%s' (%u slots)...\n", cfg.shm_name, cfg.slots);
    fflush(null);
    ShmRingClient ring;
    int retries = 0;
    while (!ring.attach(cfg.shm_name, cfg.slots, true)) {
        usleep(10_000);
        retries++;
        if (retries > 500) {
            fprintf(stderr, "[sender] timeout waiting for shm ring '%s'\n", cfg.shm_name);
            fflush(null);
            return 1;
        }
    }

    printf("[sender] target fan-out (%u destinations):\n", cfg.num_dests);
    for (uint d = 0; d < cfg.num_dests; ++d) {
        printf("         [%u] %s:%u\n", d + 1, cfg.dests[d].ip.ptr, cfg.dests[d].port);
    }
    printf("         fec group = %u, simulated drop = %.2f%%\n", cfg.fec_k, cfg.drop_rate * 100.0);
    if (cfg.echo_listen_port > 0) {
        printf("[sender] RTT Evaluation active -> listening for echoes on port %u\n", cfg.echo_listen_port);
    } else {
        printf("[sender] RTT Evaluation disabled (echo_listen_port = 0)\n");
    }
    printf("[sender] Opportunistic Adaptive Batching active (MTU 1350 B)\n");
    if (cfg.enable_sha256) {
        printf("[sender] SHA-256 Checksum calculation ENABLED\n");
    }
    fflush(null);

    // Init NAK Repair Service socket
    if (cfg.enable_nak && cfg.nak_port > 0) {
        if (g_nak_sock.initReceiver(cfg.nak_port)) {
            printf("[sender] Unicast NAK Repair service listening on UDP port %u...\n", cfg.nak_port);
            pthread_t nak_tid;
            pthread_create(&nak_tid, null, &nakWorkerThread, null);
        } else {
            fprintf(stderr, "[sender] WARNING: NAK service failed to bind to port %u\n", cfg.nak_port);
        }
    } else {
        printf("[sender] Unicast NAK Repair service disabled (nak_port = 0)\n");
    }
    fflush(null);

    UdpSocket sock;
    if (!sock.initSender(cfg.dests[0].ip.ptr, cfg.dests[0].port)) {
        fprintf(stderr, "[sender] failed to init udp sender socket\n");
        fflush(null);
        return 1;
    }
    for (uint d = 1; d < cfg.num_dests; ++d) {
        sock.addFanoutDest(cfg.dests[d].ip.ptr, cfg.dests[d].port);
    }

    UdpSocket echo_sock;
    bool enable_rtt = (cfg.echo_listen_port > 0);
    if (enable_rtt) {
        if (!echo_sock.initReceiver(cfg.echo_listen_port)) {
            fprintf(stderr, "[sender] ERROR: failed to init echo receiver socket on port %u\n", cfg.echo_listen_port);
            enable_rtt = false;
        }
    }

    FecEncoder fec_enc;
    fec_enc.reset(1);
    Sha256 sender_sha;

    ubyte[576] raw_frame;
    ubyte[256] wire_buf;
    ubyte[256] echo_buf;
    ubyte[1400] batch_buf;
    uint batch_len = 0;

    ulong read_index = ring.liveEdge();
    ulong total_sent = 0;
    ulong total_fec_sent = 0;
    ulong total_simulated_drops = 0;

    ulong rtt_count = 0;
    ulong rtt_min = ulong.max;
    ulong rtt_max = 0;
    ulong rtt_sum = 0;

    printf("[sender] entering high-frequency send loop (live edge at %llu)...\n", read_index);
    fflush(null);

    bool running = true;
    while (running) {
        // Poll RTT echo responses if active
        if (enable_rtt) {
            long echo_bytes = echo_sock.recv(echo_buf.ptr, echo_buf.sizeof);
            while (echo_bytes > 0) {
                uint offset = 0;
                while (offset + CompactWireHeader.sizeof <= cast(uint)echo_bytes) {
                    auto echo_hdr = cast(const CompactWireHeader*)(echo_buf.ptr + offset);
                    uint frame_len = 0;
                    if (echo_hdr.type == TYPE_TRADE) frame_len = CompactTradeWire.sizeof;
                    else if (echo_hdr.type == TYPE_BBO) frame_len = CompactBboWire.sizeof;
                    else if (echo_hdr.type == TYPE_ORDERBOOK) frame_len = CompactOrderBookWire.sizeof;

                    if (frame_len == 0 || offset + frame_len > cast(uint)echo_bytes) break;

                    processEchoPacket(echo_buf.ptr + offset, &rtt_count, &rtt_sum, &rtt_min, &rtt_max);
                    offset += frame_len;
                }

                echo_bytes = echo_sock.recv(echo_buf.ptr, echo_buf.sizeof);
            }
        }

        uint len = 0;
        ulong resume_at = 0;
        FrameStatus st = ring.read(read_index, raw_frame.ptr, &len, &resume_at);

        if (st == FrameStatus.kOk) {
            auto hdr = cast(const HarnessHeader*)raw_frame.ptr;
            uint wire_len = 0;

            if (hdr.type == TYPE_TRADE) {
                wire_len = encodeTrade(cast(const HarnessTrade*)raw_frame.ptr, wire_buf.ptr);
            } else if (hdr.type == TYPE_BBO) {
                wire_len = encodeBbo(cast(const HarnessBbo*)raw_frame.ptr, wire_buf.ptr);
            } else if (hdr.type == TYPE_ORDERBOOK) {
                wire_len = encodeBook(cast(const HarnessOrderBook*)raw_frame.ptr, wire_buf.ptr);
            }

            if (wire_len > 0) {
                if (cfg.enable_sha256) {
                    sender_sha.update(wire_buf.ptr, wire_len);
                }

                // Store in Replay Buffer for Unicast NAK Service if enabled
                if (cfg.enable_nak) {
                    ReplaySlot* rslot = &g_replay_ring[hdr.seq_id & REPLAY_RING_MASK];
                    memcpy(rslot.payload.ptr, wire_buf.ptr, wire_len);
                    rslot.len = wire_len;
                    rslot.seq_id = hdr.seq_id;
                }

                bool drop_this = false;
                if (cfg.drop_rate > 0.0) {
                    double r = (rand() % 10000) / 10000.0;
                    if (r < cfg.drop_rate) {
                        drop_this = true;
                        total_simulated_drops++;
                    }
                }

                if (!drop_this) {
                    if (batch_len + wire_len > 1350 && batch_len > 0) {
                        sock.send(batch_buf.ptr, batch_len);
                        batch_len = 0;
                    }
                    memcpy(batch_buf.ptr + batch_len, wire_buf.ptr, wire_len);
                    batch_len += wire_len;
                }

                total_sent++;

                if (total_sent % 10000 == 0) {
                    printf("[sender] progress: sent %llu msgs (simulated drops = %llu)\n", total_sent, total_simulated_drops);
                    fflush(null);
                }

                if (cfg.fec_k > 0) {
                    FecParityWire parity;
                    if (fec_enc.addPacket(hdr.seq_id, wire_buf.ptr, wire_len, &parity)) {
                        if (batch_len > 0) {
                            sock.send(batch_buf.ptr, batch_len);
                            batch_len = 0;
                        }
                        sock.send(&parity, FecParityWire.sizeof);
                        total_fec_sent++;
                    }
                }

                bool has_more = (read_index + 1 < ring.liveEdge());
                if (!has_more || batch_len >= 1350) {
                    if (batch_len > 0) {
                        sock.send(batch_buf.ptr, batch_len);
                        batch_len = 0;
                    }
                }

                if (cfg.count > 0 && total_sent >= cfg.count) {
                    if (batch_len > 0) {
                        sock.send(batch_buf.ptr, batch_len);
                        batch_len = 0;
                    }
                    running = false;
                    break;
                }
            }

            read_index++;
        } else if (st == FrameStatus.kLapped) {
            read_index = resume_at;
        } else { // kEmpty
            if (batch_len > 0) {
                sock.send(batch_buf.ptr, batch_len);
                batch_len = 0;
            }
            if (cfg.count > 0 && total_sent >= cfg.count) {
                running = false;
                break;
            }
        }
    }

    // Drain remaining echoed packets: exit instantly if all echoes arrived, or after 500ms idle timeout
    if (enable_rtt) {
        printf("[sender] draining remaining echo packets (500ms idle timeout)...\n");
        fflush(null);

        int quiet_ms = 0;
        while (quiet_ms < 500 && rtt_count < total_sent) {
            long echo_bytes = echo_sock.recv(echo_buf.ptr, echo_buf.sizeof);
            if (echo_bytes > 0) {
                quiet_ms = 0;
                while (echo_bytes > 0) {
                    uint offset = 0;
                    while (offset + CompactWireHeader.sizeof <= cast(uint)echo_bytes) {
                        auto echo_hdr = cast(const CompactWireHeader*)(echo_buf.ptr + offset);
                        uint frame_len = 0;
                        if (echo_hdr.type == TYPE_TRADE) frame_len = CompactTradeWire.sizeof;
                        else if (echo_hdr.type == TYPE_BBO) frame_len = CompactBboWire.sizeof;
                        else if (echo_hdr.type == TYPE_ORDERBOOK) frame_len = CompactOrderBookWire.sizeof;

                        if (frame_len == 0 || offset + frame_len > cast(uint)echo_bytes) break;

                        processEchoPacket(echo_buf.ptr + offset, &rtt_count, &rtt_sum, &rtt_min, &rtt_max);
                        if (rtt_count >= total_sent) break;
                        offset += frame_len;
                    }
                    if (rtt_count >= total_sent) break;
                    echo_bytes = echo_sock.recv(echo_buf.ptr, echo_buf.sizeof);
                }
            } else {
                usleep(1_000);
                quiet_ms++;
            }
        }

        printf("\n---- RTT Delivery Metrics Summary ----\n");
        printf("sent         : %llu\n", total_sent);
        printf("echoed       : %llu\n", rtt_count);
        if (total_sent > 0) {
            double echo_rate = (cast(double)rtt_count / cast(double)total_sent) * 100.0;
            printf("echo_rate    : %.4f%%\n", echo_rate);
        }
        if (rtt_count > 0) {
            printf("1-way lat(ns): min=%llu mean=%llu max=%llu\n", rtt_min, rtt_sum / rtt_count, rtt_max);
        } else {
            printf("1-way lat(ns): NO ECHO PACKETS RECEIVED (Check ufw allow 9001/udp on sender host)\n");
        }
        printf("--------------------------------------\n");
        fflush(null);
    }

    if (cfg.enable_sha256) {
        char[65] hex;
        sender_sha.finalHex(&hex);
        printf("[sender] SHA-256 Checksum (%llu msgs): %s\n", total_sent, hex.ptr);
        fflush(null);
    }

    printf("[sender] completed sending %llu msgs (serviced %llu NAK requests). Exiting.\n", total_sent, g_total_naks_serviced);
    fflush(null);

    exit(0);
    return 0;
}
