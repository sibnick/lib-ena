// Module receiver_main.d - Receiver Process Entry Point for Low-Latency Transport
module receiver_main;

import core.stdc.stdio : printf, fprintf, stderr, fflush;
import core.stdc.stdlib : exit, atoi;
import core.stdc.string : strcmp, memcpy;
import core.stdc.errno : errno;
import core.sys.posix.unistd : usleep;
import core.sys.posix.sched : sched_yield;
import core.sys.posix.netinet.in_ : sockaddr_in, htons, inet_addr;
import core.sys.posix.arpa.inet : inet_ntoa;
import core.sys.posix.pthread : pthread_create, pthread_t;
import core.atomic : atomicLoad, atomicStore, MemoryOrder;
import wire_protocol;
import shm_client;
import fec;
import socket;
import sha256;
import preflight_check;

@nogc:
nothrow:

struct Config {
    const(char)* shm_name = "/fanout_ring";
    uint slots = 65536;
    ushort port = 9000;
    const(char)* echo_ip = null;
    ushort echo_port = 9001; // Default to port 9001 for zero-config multi-host RTT testing
    const(char)* nak_ip = null;
    ushort nak_port = 9002;
    bool enable_nak = true;
    bool enable_sha256 = false;
}

void parseArgs(int argc, char** argv, Config* cfg) @nogc nothrow {
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--shm") == 0 && i + 1 < argc) {
            cfg.shm_name = argv[++i];
        } else if (strcmp(argv[i], "--slots") == 0 && i + 1 < argc) {
            cfg.slots = cast(uint)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            cfg.port = cast(ushort)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--echo-dest") == 0 && i + 1 < argc) {
            cfg.echo_ip = argv[++i];
        } else if (strcmp(argv[i], "--echo-port") == 0 && i + 1 < argc) {
            cfg.echo_port = cast(ushort)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--nak-dest") == 0 && i + 1 < argc) {
            cfg.nak_ip = argv[++i];
        } else if (strcmp(argv[i], "--nak-port") == 0 && i + 1 < argc) {
            cfg.nak_port = cast(ushort)atoi(argv[++i]);
            if (cfg.nak_port == 0) cfg.enable_nak = false;
        } else if (strcmp(argv[i], "--no-nak") == 0) {
            cfg.enable_nak = false;
            cfg.nak_port = 0;
        } else if (strcmp(argv[i], "--sha256") == 0) {
            cfg.enable_sha256 = true;
        }
    }
}

// Lock-Free Ring Buffer for Async Echo Thread
enum uint ECHO_RING_SIZE = 65536;
enum uint ECHO_RING_MASK = ECHO_RING_SIZE - 1;

align(64) struct EchoSlot {
    ubyte[256] payload;
    uint len;
    sockaddr_in target_addr;
    shared ulong ready;
}

__gshared EchoSlot[ECHO_RING_SIZE] g_echo_ring;
align(64) __gshared ulong g_echo_write_idx = 0;
align(64) __gshared ulong g_echo_read_idx = 0;
__gshared UdpSocket g_echo_sock;
__gshared bool g_echo_enabled = false;
__gshared ulong g_total_echoed = 0;
__gshared ulong g_total_echo_errs = 0;

__gshared UdpSocket g_nak_sock;
__gshared bool g_nak_enabled = false;
__gshared ulong g_total_naks_sent = 0;

extern (C) void* echoWorkerThread(void* arg) @nogc nothrow {
    ulong read_idx = 0;
    while (true) {
        EchoSlot* slot = &g_echo_ring[read_idx & ECHO_RING_MASK];
        ulong state = atomicLoad!(MemoryOrder.acq)(slot.ready);
        if (state == read_idx + 1) {
            g_echo_sock.dest_addr = slot.target_addr;
            
            long res = g_echo_sock.send(slot.payload.ptr, slot.len);
            int retries = 0;
            while (res < 0 && retries < 100) {
                sched_yield();
                res = g_echo_sock.send(slot.payload.ptr, slot.len);
                retries++;
            }

            if (res >= 0) {
                g_total_echoed++;
                if (g_total_echoed == 1) {
                    const(char)* src_ip_str = inet_ntoa(slot.target_addr.sin_addr);
                    printf("[receiver] [Async Echo] first packet sent to %s:%u (res=%ld)\n",
                           src_ip_str, ntohs(slot.target_addr.sin_port), res);
                    fflush(null);
                }
            } else {
                g_total_echo_errs++;
            }

            atomicStore!(MemoryOrder.rel)(slot.ready, 0);
            read_idx++;
        } else {
            sched_yield();
        }
    }
    return null;
}

ushort ntohs(ushort netshort) @nogc nothrow {
    return cast(ushort)((netshort >> 8) | (netshort << 8));
}

void sendNakRequest(ulong start_seq, ulong end_seq, const sockaddr_in* sender_addr, ushort nak_port) @nogc nothrow {
    if (!g_nak_enabled) return;
    NakReqWire req;
    req.header.seq_id = 0;
    req.header.send_ts_ns = 0;
    req.header.type = TYPE_NAK_REQ;
    req.header.version_ = 1;
    req.start_seq = start_seq;
    req.end_seq = end_seq;
    req.receiver_id = 1;

    sockaddr_in nak_target = *sender_addr;
    nak_target.sin_port = htons(nak_port);
    if (nak_target.sin_addr.s_addr == 0) {
        nak_target.sin_addr.s_addr = inet_addr("127.0.0.1");
    }

    g_nak_sock.sendTo(&req, NakReqWire.sizeof, &nak_target);
    g_total_naks_sent++;
}

void publishDecodedFrame(const ubyte* wire_buf, uint len, ShmRingClient* ring) @nogc nothrow {
    auto hdr = cast(const CompactWireHeader*)wire_buf;
    ubyte[1024] harness_buf;
    uint harness_len = 0;

    if (hdr.type == TYPE_TRADE) {
        harness_len = decodeTrade(wire_buf, cast(HarnessTrade*)harness_buf.ptr);
    } else if (hdr.type == TYPE_BBO) {
        harness_len = decodeBbo(wire_buf, cast(HarnessBbo*)harness_buf.ptr);
    } else if (hdr.type == TYPE_ORDERBOOK) {
        harness_len = decodeBook(wire_buf, cast(HarnessOrderBook*)harness_buf.ptr);
    }

    if (harness_len > 0) {
        ring.publish(harness_buf.ptr, harness_len);
    }
}

extern (C) int main(int argc, char** argv) @nogc nothrow {
    Config cfg;
    parseArgs(argc, argv, &cfg);

    runPreflightChecks("receiver");

    printf("[receiver] attaching to shm segment '%s' (%u slots)...\n", cfg.shm_name, cfg.slots);
    fflush(null);
    ShmRingClient ring;
    if (!ring.attach(cfg.shm_name, cfg.slots, true)) {
        fprintf(stderr, "[receiver] failed to attach/create shm ring '%s'\n", cfg.shm_name);
        fflush(null);
        return 1;
    }

    printf("[receiver] listening on udp port %u...\n", cfg.port);
    if (cfg.echo_port > 0) {
        const(char)* target_display = cfg.echo_ip !is null ? cfg.echo_ip : "auto/source IP";
        printf("[receiver] RTT Async Echo active -> echoing back to %s:%u\n", target_display, cfg.echo_port);
    } else {
        printf("[receiver] RTT Echo disabled (echo_port = 0)\n");
    }
    if (cfg.enable_sha256) {
        printf("[receiver] SHA-256 Checksum calculation ENABLED\n");
    }
    fflush(null);

    UdpSocket sock;
    if (!sock.initReceiver(cfg.port)) {
        fprintf(stderr, "[receiver] failed to init udp receiver socket on port %u\n", cfg.port);
        fflush(null);
        return 1;
    }

    g_echo_enabled = (cfg.echo_port > 0);
    if (g_echo_enabled) {
        const(char)* dest = cfg.echo_ip !is null ? cfg.echo_ip : "127.0.0.1";
        if (!g_echo_sock.initSender(dest, cfg.echo_port)) {
            fprintf(stderr, "[receiver] ERROR: failed to init echo sender socket to port %u\n", cfg.echo_port);
            g_echo_enabled = false;
        } else {
            // Launch dedicated async echo thread
            pthread_t thread_id;
            int err = pthread_create(&thread_id, null, &echoWorkerThread, null);
            if (err != 0) {
                fprintf(stderr, "[receiver] ERROR: pthread_create failed (err=%d)\n", err);
                g_echo_enabled = false;
            }
        }
    }

    // Init Unicast NAK Client socket
    if (cfg.enable_nak && cfg.nak_port > 0) {
        const(char)* nak_dest = cfg.nak_ip !is null ? cfg.nak_ip : "127.0.0.1";
        g_nak_enabled = g_nak_sock.initSender(nak_dest, cfg.nak_port);
        printf("[receiver] Targeted Unicast NAK Retransmission active -> target %s:%u\n", nak_dest, cfg.nak_port);
    } else {
        g_nak_enabled = false;
        printf("[receiver] Targeted Unicast NAK Retransmission disabled (nak_port = 0)\n");
    }

    FecDecoderGroup fec_dec;
    fec_dec.reset(1);
    Sha256 receiver_sha;

    ubyte[1500] rx_buf;
    ulong total_received = 0;
    ulong total_recovered = 0;
    ulong last_logged_received = 0;
    ulong max_seen_seq = 0;
    int idle_ms = 0;

    printf("[receiver] entering high-frequency receive loop (adaptive batch parsing)...\n");
    fflush(null);

    while (true) {
        sockaddr_in src_addr;
        long nbytes = sock.recv(rx_buf.ptr, rx_buf.sizeof, &src_addr);
        if (nbytes > 0) {
            idle_ms = 0;

            uint offset = 0;
            while (offset + CompactWireHeader.sizeof <= cast(uint)nbytes) {
                auto hdr = cast(const CompactWireHeader*)(rx_buf.ptr + offset);
                uint frame_len = 0;

                if (hdr.type == TYPE_TRADE) frame_len = CompactTradeWire.sizeof;
                else if (hdr.type == TYPE_BBO) frame_len = CompactBboWire.sizeof;
                else if (hdr.type == TYPE_ORDERBOOK) frame_len = CompactOrderBookWire.sizeof;
                else if (hdr.type == TYPE_FEC_PARITY) frame_len = FecParityWire.sizeof;

                if (frame_len == 0 || offset + frame_len > cast(uint)nbytes) break;

                const ubyte* frame_ptr = rx_buf.ptr + offset;

                if (g_echo_enabled && hdr.type != TYPE_FEC_PARITY) {
                    ulong w_idx = atomicLoad!(MemoryOrder.raw)(g_echo_write_idx);
                    EchoSlot* slot = &g_echo_ring[w_idx & ECHO_RING_MASK];
                    if (atomicLoad!(MemoryOrder.acq)(slot.ready) == 0) {
                        memcpy(slot.payload.ptr, frame_ptr, frame_len);
                        slot.len = frame_len;
                        slot.target_addr = src_addr;
                        slot.target_addr.sin_port = htons(cfg.echo_port);
                        if (slot.target_addr.sin_addr.s_addr == 0) {
                            slot.target_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
                        }
                        if (cfg.echo_ip !is null) {
                            slot.target_addr.sin_addr.s_addr = inet_addr(cfg.echo_ip);
                        }
                        atomicStore!(MemoryOrder.rel)(slot.ready, w_idx + 1);
                        atomicStore!(MemoryOrder.rel)(g_echo_write_idx, w_idx + 1);
                    }
                }

                if (hdr.type == TYPE_FEC_PARITY) {
                    auto parity = cast(const FecParityWire*)frame_ptr;
                    
                    ulong expected_base = parity.base_seq_id;
                    if (fec_dec.base_seq_id != expected_base && fec_dec.base_seq_id > 0) {
                        ulong rec_seq = 0;
                        ubyte[256] rec_payload;
                        uint rec_len = 0;
                        if (fec_dec.tryRecover(&rec_seq, rec_payload.ptr, &rec_len)) {
                            if (rec_seq > max_seen_seq) {
                                max_seen_seq = rec_seq;
                                if (cfg.enable_sha256) receiver_sha.update(rec_payload.ptr, rec_len);
                                publishDecodedFrame(rec_payload.ptr, rec_len, &ring);
                                total_recovered++;
                            }
                        } else {
                            // Unrecoverable FEC gap: Request Unicast NAK Retransmission directly from Sender if enabled
                            sendNakRequest(fec_dec.base_seq_id, expected_base - 1, &src_addr, cfg.nak_port);
                        }
                        fec_dec.reset(expected_base);
                    }

                    fec_dec.addParityPacket(parity);

                    ulong rec_seq = 0;
                    ubyte[256] rec_payload;
                    uint rec_len = 0;
                    if (fec_dec.tryRecover(&rec_seq, rec_payload.ptr, &rec_len)) {
                        if (rec_seq > max_seen_seq) {
                            max_seen_seq = rec_seq;
                            if (cfg.enable_sha256) receiver_sha.update(rec_payload.ptr, rec_len);
                            publishDecodedFrame(rec_payload.ptr, rec_len, &ring);
                            total_recovered++;
                        }
                    }
                } else if (hdr.type == TYPE_TRADE || hdr.type == TYPE_BBO || hdr.type == TYPE_ORDERBOOK) {
                    if (hdr.seq_id > 0) {
                        ulong expected_base = ((hdr.seq_id - 1) / FEC_GROUP_SIZE) * FEC_GROUP_SIZE + 1;
                        if (fec_dec.base_seq_id != expected_base && fec_dec.base_seq_id > 0) {
                            ulong rec_seq = 0;
                            ubyte[256] rec_payload;
                            uint rec_len = 0;
                            if (fec_dec.tryRecover(&rec_seq, rec_payload.ptr, &rec_len)) {
                                if (rec_seq > max_seen_seq) {
                                    max_seen_seq = rec_seq;
                                    if (cfg.enable_sha256) receiver_sha.update(rec_payload.ptr, rec_len);
                                    publishDecodedFrame(rec_payload.ptr, rec_len, &ring);
                                    total_recovered++;
                                }
                            } else {
                                // Unrecoverable FEC gap: Request Unicast NAK Retransmission directly from Sender if enabled
                                sendNakRequest(fec_dec.base_seq_id, expected_base - 1, &src_addr, cfg.nak_port);
                            }
                            fec_dec.reset(expected_base);
                        }
                    }

                    if (hdr.seq_id > max_seen_seq) {
                        max_seen_seq = hdr.seq_id;
                    }

                    fec_dec.addDataPacket(hdr.seq_id, frame_ptr, frame_len);
                    if (cfg.enable_sha256) receiver_sha.update(frame_ptr, frame_len);
                    publishDecodedFrame(frame_ptr, frame_len, &ring);
                    total_received++;

                    if (total_received % 10000 == 0) {
                        printf("[receiver] progress: received %llu msgs (seq_id = %llu, fec recovered = %llu, echoed = %llu, naks_sent = %llu)\n",
                               total_received, hdr.seq_id, total_recovered, g_total_echoed, g_total_naks_sent);
                        fflush(null);
                        last_logged_received = total_received;
                    }
                }

                offset += frame_len;
            }
        } else {
            if (total_received > 0) {
                usleep(1_000);
                idle_ms++;
                if (idle_ms == 500) {
                    if (total_received > last_logged_received) {
                        printf("\n[receiver] STREAM IDLE (500ms): total_received = %llu (last seq_id = %llu, fec recovered = %llu, total echoed = %llu, naks_sent = %llu)\n",
                               total_received, max_seen_seq, total_recovered, g_total_echoed, g_total_naks_sent);
                        last_logged_received = total_received;
                    }
                    if (cfg.enable_sha256) {
                        char[65] hex;
                        receiver_sha.finalHex(&hex);
                        printf("[receiver] SHA-256 Checksum (%llu msgs): %s\n", total_received, hex.ptr);
                    }
                    fflush(null);
                }
            }
        }
    }

    return 0;
}
