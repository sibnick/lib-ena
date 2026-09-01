// Module socket.d - High-Performance Low-Latency UDP Socket Wrapper for Dlang betterC
module socket;

import core.stdc.stdio : fprintf, stderr;
import core.stdc.string : memset;
import core.sys.posix.fcntl : fcntl, F_GETFL, F_SETFL, O_NONBLOCK;
import core.sys.posix.netinet.in_ : sockaddr_in, in_addr, AF_INET, IPPROTO_UDP, IPPROTO_IP, htons, inet_addr;
import core.sys.posix.sys.socket : socket, bind, sendto, recvfrom, setsockopt, SOL_SOCKET, SO_REUSEADDR, SO_SNDBUF, SO_RCVBUF, sockaddr, socklen_t;

@nogc:
nothrow:

enum int SO_REUSEPORT = 15;
enum int SO_BUSY_POLL = 50;
enum int IP_TOS = 1;
enum int IPTOS_LOWDELAY = 0x10;
enum uint MAX_FANOUT_DESTS = 16;

struct UdpSocket {
    int fd = -1;
    sockaddr_in local_addr;
    sockaddr_in dest_addr;
    sockaddr_in[MAX_FANOUT_DESTS] fanout_addrs;
    uint num_fanout_dests = 0;

    bool initSender(const(char)* dest_ip, ushort dest_port) @nogc nothrow {
        fd = socket(AF_INET, 2, IPPROTO_UDP); // SOCK_DGRAM = 2
        if (fd < 0) {
            fprintf(stderr, "[socket] creation failed\n");
            return false;
        }

        int flag = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &flag, flag.sizeof);
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &flag, flag.sizeof);

        // Try 16MB Send Buffer, fallback to 2MB
        int buf_size = 16 * 1024 * 1024;
        if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf_size, buf_size.sizeof) != 0) {
            buf_size = 2 * 1024 * 1024;
            setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf_size, buf_size.sizeof);
        }

        // HFT Low-Delay TOS / DSCP
        int tos = IPTOS_LOWDELAY;
        setsockopt(fd, IPPROTO_IP, IP_TOS, &tos, tos.sizeof);

        // Non-blocking mode
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        memset(&dest_addr, 0, dest_addr.sizeof);
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(dest_port);
        dest_addr.sin_addr.s_addr = inet_addr(dest_ip);

        num_fanout_dests = 0;
        addFanoutDest(dest_ip, dest_port);

        return true;
    }

    void addFanoutDest(const(char)* ip, ushort port) @nogc nothrow {
        if (num_fanout_dests < MAX_FANOUT_DESTS) {
            memset(&fanout_addrs[num_fanout_dests], 0, sockaddr_in.sizeof);
            fanout_addrs[num_fanout_dests].sin_family = AF_INET;
            fanout_addrs[num_fanout_dests].sin_port = htons(port);
            fanout_addrs[num_fanout_dests].sin_addr.s_addr = inet_addr(ip);
            num_fanout_dests++;
        }
    }

    void clearFanoutDests() @nogc nothrow {
        num_fanout_dests = 0;
    }

    bool initReceiver(ushort listen_port) @nogc nothrow {
        fd = socket(AF_INET, 2, IPPROTO_UDP); // SOCK_DGRAM = 2
        if (fd < 0) {
            fprintf(stderr, "[socket] creation failed\n");
            return false;
        }

        int flag = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &flag, flag.sizeof);
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &flag, flag.sizeof);

        // Try 16MB Receive Buffer, fallback to 2MB
        int buf_size = 16 * 1024 * 1024;
        if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf_size, buf_size.sizeof) != 0) {
            buf_size = 2 * 1024 * 1024;
            setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf_size, buf_size.sizeof);
        }

        // Busy poll mode (poll kernel socket queues in low latency)
        int busy_poll_us = 50;
        setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL, &busy_poll_us, busy_poll_us.sizeof);

        // Non-blocking mode
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        memset(&local_addr, 0, local_addr.sizeof);
        local_addr.sin_family = AF_INET;
        local_addr.sin_port = htons(listen_port);
        local_addr.sin_addr.s_addr = 0; // INADDR_ANY

        if (bind(fd, cast(sockaddr*)&local_addr, local_addr.sizeof) < 0) {
            fprintf(stderr, "[socket] bind to port %d failed\n", listen_port);
            return false;
        }

        return true;
    }

    long send(const(void)* data, uint len) @nogc nothrow {
        if (num_fanout_dests <= 1) {
            return sendto(fd, data, len, 0, cast(const(sockaddr)*)&dest_addr, dest_addr.sizeof);
        }
        long last_res = 0;
        for (uint i = 0; i < num_fanout_dests; ++i) {
            last_res = sendto(fd, data, len, 0, cast(const(sockaddr)*)&fanout_addrs[i], sockaddr_in.sizeof);
        }
        return last_res;
    }

    long sendTo(const(void)* data, uint len, const(sockaddr_in)* target) @nogc nothrow {
        return sendto(fd, data, len, 0, cast(const(sockaddr)*)target, target.sizeof);
    }

    long recv(void* buf, uint max_len, sockaddr_in* src_addr = null) @nogc nothrow {
        socklen_t addr_len = sockaddr_in.sizeof;
        return recvfrom(fd, buf, max_len, 0, cast(sockaddr*)src_addr, src_addr ? &addr_len : null);
    }

    void closeSocket() @nogc nothrow {
        if (fd >= 0) {
            import core.sys.posix.unistd : close;
            close(fd);
            fd = -1;
        }
    }
}
