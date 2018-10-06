#!/bin/bash

# Set up a stable environment to run performance benchmarks on cloudlab machines.

# Install useful tools
sudo apt update
sudo apt install -y linux-tools-common linux-tools-$(uname -r) cpuset cpufrequtils i7z tuned

# TODO: Hmmm, maybe one "tuned" to rule them all? What does this profile do actually? Looks like it's not turning off HT or TurboBoost.
sudo systemctl start tuned
sudo tuned-adm profile latency-performance

# Disable address space randomization.
# Suggested by https://llvm.org/docs/Benchmarking.html
echo 0 | sudo tee -a /proc/sys/kernel/randomize_va_space

# Disable dynamic frequency scaling.
# TODO: can't do this anymore after using tuned-adm?
# sudo cpupower frequency-set -g performance

# Disable hyper-threading.
maxCPU=$(($(lscpu | grep "^CPU(s)" | awk '{print $2}') - 1))
for n in `seq 0 $maxCPU`
do
    cpu=/sys/devices/system/cpu/cpu$n
    hyperthread=$(cut -d, -f2 $cpu/topology/thread_siblings_list)
    if [[ "$n" = "$hyperthread" ]]
    then
        echo 0 | sudo tee -a $cpu/online
    fi
done

# Disable turbo-boost.
cores=$(cat /proc/cpuinfo | grep processor | awk '{print $3}')
for core in $cores; do
    sudo wrmsr -p${core} 0x1a0 0x4000850089
    state=$(sudo rdmsr -p${core} 0x1a0 -f 38:38)
    if [[ $state -eq 1 ]]; then
        echo "core ${core}: disabled"
    else
        echo "core ${core}: enabled"
    fi
done

# Allow us to collect performance metrics via `perf` without sudo.
echo -1 | sudo tee -a /proc/sys/kernel/perf_event_paranoid

# Disable deep C-states without changing BIOS setting or kernel boot parameter.
# See https://lore.kernel.org/patchwork/patch/824326/
# TODO: the following only keeps after "sudo su"; once you exit from root; the
# fd will be closed and C-states reenabled...
# TODO: other solutions using Perl (https://stackoverflow.com/questions/22482252/how-to-set-intel-idle-max-cstate-0-to-disable-c-states) or C (https://access.redhat.com/articles/65410).
#exec 3<> /dev/cpu_dma_latency; echo 0 >&3

# Reserve a number of cores (on the same NUMA node) to run benchmark programs.
# cset shield -c N1,N2 -k on
