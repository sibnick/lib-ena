// Module preflight_check.d - Low-Latency Preflight Checks for CPU Contention & Environment Validation
module preflight_check;

import core.stdc.stdio : printf, fprintf, stderr, fflush, fopen, fclose, fgets, sscanf, FILE;
import core.stdc.string : strstr, memset;
import core.sys.posix.unistd : getpid, pid_t;

@nogc:
nothrow:

extern (C) {
    struct cpu_set_t {
        ulong[16] __bits;
    }
    int sched_getaffinity(pid_t pid, size_t cpusetsize, cpu_set_t* mask) @nogc nothrow;
}

pragma(inline, true)
bool isCpuSet(const(cpu_set_t)* set, uint cpu) @nogc nothrow {
    if (cpu >= 1024) return false;
    uint idx = cpu / 64;
    uint bit = cpu % 64;
    return (set.__bits[idx] & (1UL << bit)) != 0;
}

struct PreflightResult {
    bool passed;
    int pinned_core;
    int num_pinned;
    double load1;
    double load5;
    double load15;
}

PreflightResult runPreflightChecks(const(char)* process_name) @nogc nothrow {
    PreflightResult res;
    res.passed = true;
    res.pinned_core = -1;
    res.num_pinned = 0;

    printf("[%s] [Preflight] Running hardware & host sanity checks...\n", process_name);

    // 1. Check CPU Affinity
    cpu_set_t cpuset;
    memset(&cpuset, 0, cpuset.sizeof);
    pid_t pid = getpid();
    if (sched_getaffinity(pid, cpuset.sizeof, &cpuset) == 0) {
        int count = 0;
        int first_core = -1;
        for (uint c = 0; c < 256; ++c) {
            if (isCpuSet(&cpuset, c)) {
                count++;
                if (first_core < 0) first_core = cast(int)c;
            }
        }
        res.num_pinned = count;
        res.pinned_core = first_core;

        if (count == 1) {
            printf("[%s] [Preflight]  Thread pinned to dedicated Core %d (Zero Context-Switch Policy)\n",
                   process_name, first_core);
        } else {
            printf("[%s] [Preflight] ⚠️ WARNING: Thread NOT pinned to a single core (Affinity mask covers %d cores). Run with 'taskset -c <core>' for minimal tail latency!\n",
                   process_name, count);
        }
    }

    // 2. Read System Load Average from /proc/loadavg
    FILE* fp = fopen("/proc/loadavg", "r");
    if (fp) {
        char[128] buf;
        if (fgets(buf.ptr, buf.sizeof, fp)) {
            sscanf(buf.ptr, "%lf %lf %lf", &res.load1, &res.load5, &res.load15);
            printf("[%s] [Preflight]  System Load Average: %.2f (1m), %.2f (5m), %.2f (15m)\n",
                   process_name, res.load1, res.load5, res.load15);
        }
        fclose(fp);
    }

    fflush(null);
    return res;
}
