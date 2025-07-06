#!/bin/bash
# save as setup_perf_fedora.sh

# Install required packages
sudo dnf install -y util-linux schedutils

# Stop unnecessary services
sudo systemctl stop irqbalance
sudo systemctl stop thermald 2>/dev/null || true

# Set CPU governor to performance
echo performance | sudo tee /sys/devices/system/cpu/cpu7/cpufreq/scaling_governor

# Disable CPU frequency scaling for core 7
MAXFREQ=$(cat /sys/devices/system/cpu/cpu7/cpufreq/scaling_max_freq)
echo $MAXFREQ | sudo tee /sys/devices/system/cpu/cpu7/cpufreq/scaling_min_freq

# Move IRQs away from core 7 (bind to cores 0-6)
for irq in /proc/irq/*/smp_affinity; do
    echo "7f" | sudo tee $irq 2>/dev/null || true  # 0x7f = cores 0-6
done

# Set kernel parameters for performance
echo 0 | sudo tee /proc/sys/kernel/hung_task_timeout_secs
echo 0 | sudo tee /sys/kernel/debug/sched_features

# Disable swap for better performance
sudo swapoff -a

echo "Core 7 isolated and optimized for performance testing on Fedora"
