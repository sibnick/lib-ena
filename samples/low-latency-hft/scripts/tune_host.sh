#!/usr/bin/env bash
# ==============================================================================
# Low-Latency Host & ENA NIC IRQ Tuning Script
# ==============================================================================
# Applies Linux kernel network buffer sizing, busy-poll configuration,
# and pins NIC interrupts away from isolated worker CPU cores onto housekeeping cores.
# ==============================================================================

if [ "$EUID" -ne 0 ]; then
    echo "Please run as root / sudo to apply kernel & IRQ configuration:"
    echo "  sudo bash scripts/tune_host.sh"
    exit 1
fi

echo "=== Applying Linux Low-Latency Kernel Network Parameters ==="
sysctl -w net.core.rmem_max=67108864
sysctl -w net.core.wmem_max=67108864
sysctl -w net.core.rmem_default=33554432
sysctl -w net.core.wmem_default=33554432
sysctl -w net.core.busy_poll=50
sysctl -w net.core.busy_read=50
sysctl -w net.ipv4.udp_rmem_min=16384
sysctl -w net.ipv4.udp_wmem_min=16384

echo "=== Configuring CPU Governor to Performance ==="
if command -v cpupower &>/dev/null; then
    cpupower frequency-set -g performance 2>/dev/null || true
fi

echo "=== Pinning NIC IRQs to Housekeeping Cores (Cores 0-1) ==="
# Steering all network interrupts away from isolated benchmark cores (e.g. Cores 2+)
for irq in $(grep -E "eth|ena|ens|enp" /proc/interrupts | awk '{print $1}' | sed 's/://'); do
    if [ -d "/proc/irq/$irq" ]; then
        echo "3" > "/proc/irq/$irq/smp_affinity" 2>/dev/null || true # 3 = 0x3 (Cores 0 & 1)
        echo "  Pinned IRQ $irq to CPU mask 0x3 (Cores 0-1)"
    fi
done

echo "=== Host Low-Latency Tuning Completed Successfully ==="
