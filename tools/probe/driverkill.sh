#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
#
# The GPU driver service killed halfway through a move, at the moment the
# GPU has no driver, for its rescue unit to be seen binding one (DESIGN
# *Another kernel driver, with the window open*). Run as root on the bare
# machine, then ask for a move off <driver> -- drivermove, or the GPU box:
#
#   tools/probe/driverkill.sh 0000:01:00.0 nouveau
#
# It waits for the GPU to be on <driver>, then for a move's service to
# start and the GPU's driver link to go, freezes the service's cgroup there
# -- inside rmmod nouveau, or NVIDIA's unbind -- so that nothing after the
# let-go runs, and kills it with SIGKILL. The rescue's lines and the driver
# after follow. On schenker the rescue bound NVIDIA's module 0.57 s after
# the kill.
set -u
pci=${1:?usage: driverkill.sh <pci address> <driver to arm on>}
arm=${2:?usage: driverkill.sh <pci address> <driver to arm on>}
link=/sys/bus/pci/devices/$pci/driver
slice='/sys/fs/cgroup/system.slice/system-fenestal*'
units="$slice/fenestal-gpu-driver@*.service"

bound() { l=$(readlink "$link" 2>/dev/null) && basename "$l" || echo none; }

until [ "$(bound)" = "$arm" ]; do sleep 0.05; done
# A service still ending from the move onto it is not the one to kill.
while ls -d $units >/dev/null 2>&1; do sleep 0.01; done
echo "armed: $pci on $arm"
cg=
until [ -n "$cg" ]; do cg=$(ls -d $units 2>/dev/null | head -1); done
while [ -e "$link" ]; do :; done
echo 1 > "$cg/cgroup.freeze"
echo "frozen with $pci on $(bound): $(cat "$cg/cgroup.procs" | tr '\n' ' ')"
systemctl kill -s KILL "$(basename "$cg")"
sleep 3
echo "after the rescue: $pci on $(bound)"
journalctl --since "-15 s" -u 'fenestal-gpu-driver*' --no-pager -o cat
systemctl reset-failed 'fenestal-gpu-driver@*'
