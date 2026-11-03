#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
#
# An NVIDIA GPU moved between nouveau and NVIDIA's own module while the
# machine runs, with no reboot (DESIGN *Another kernel driver, with the
# window open*). Installed as /usr/local/sbin/fenestal-gpu-driver by
# `bare_driver.py --setup`, which puts systemd's socket in front of it --
# /run/fenestal-gpu-driver.sock, for the group fenestal-gpu -- and a
# sudoers rule beside it; called by the library's KMS backend
# (FNL_GPU_DRIVER) through the socket, or through sudo where there is
# none, and by the harness's KMS rounds (`kms-vulkan-bare@machine:nouveau`)
# as root.
#
#   fenestal-gpu-driver list                every NVIDIA GPU, one a line:
#                                           its address, the driver bound
#                                           ("none") and the drivers this
#                                           kernel has a module for
#   fenestal-gpu-driver <driver> [address]  that driver bound, as root
#   fenestal-gpu-driver serve               a move, read as a line from
#                                           stdin, and `status: <n>`
#                                           written after its lines: the
#                                           socket's service, whose client
#                                           sees no exit status
#   fenestal-gpu-driver rescue              an NVIDIA GPU left with no
#                                           driver bound to one: the
#                                           service's OnFailure
#
# The two drivers let the GPU go differently, each the way it survives:
#
#   NVIDIA's module stays loaded and the GPU is unbound from it through
#   sysfs. Loading it was 1.7 of the 1.9 s a move onto it took, and bound
#   again it takes 0.4 s; with nothing bound it is cheap for its userspace,
#   which without it spent 2.5 s a load trying to load it. Held open, its
#   unbind waits for the holder to close and then completes -- a stall and
#   not damage -- so the holders are looked for first, and named while it
#   waits.
#
#   nouveau is unloaded, never unbound: an unbind with a Vulkan device open
#   on it panicked the kernel (7.2.11, in nouveau_ttm_fini), where unloading
#   refuses, atomically, while anything holds the module -- an open node, a
#   descriptor in flight, a buffer another driver imported. By rmmod, which
#   unloads nouveau alone: modprobe -r also unloads what it alone used,
#   which the next move onto it loads again. It is loaded again ahead of
#   that move, while NVIDIA's module drives the GPU (preload()).
#
# A move once begun is finished. From the lock on, hangup, interrupt,
# terminate and a closed pipe are ignored, here and in what this runs: a
# helper stopped between the one driver letting go and the other binding
# would leave the GPU with no driver. SIGKILL is left, and the service
# answers it with `rescue`.
#
# Only an NVIDIA display device bound to one of the two, or to nothing, is
# moved, named by an address this machine lists (gpus()), whatever route
# the request came by: the sudo rule lets the account pass any words, and
# a path of its own would have root write through its symlinks. Two
# callers take turns (LOCK). Exit status: 0 bound, 2 the GPU is held open
# -- each holder printed as `held: <pid> <name>` -- and 1 anything else.
set -u

DRIVERS="nouveau nvidia"
# In /run and not /run/lock, which is world-writable on Debian, and made
# root's alone: a lock any account may open is one it may hold, and every
# move and rescue would wait it out.
LOCK=/run/fenestal-gpu-driver.lock

say() { echo "fenestal-gpu-driver: $*"; }
fail() { say "$*" >&2; exit 1; }
now_ms() { echo $(($(date +%s%N) / 1000000)); }

is_gpu() {
    [ "$(cat "/sys/bus/pci/devices/$1/vendor")" = 0x10de ] || return 1
    case $(cat "/sys/bus/pci/devices/$1/class") in
    0x03*) return 0 ;;
    *) return 1 ;;
    esac
}

gpus() {
    for d in /sys/bus/pci/devices/*; do
        is_gpu "${d##*/}" && echo "${d##*/}"
    done
}

bound() {
    drv=$(readlink "/sys/bus/pci/devices/$1/driver") || drv=none
    echo "${drv##*/}"
}

# A module this kernel has: nouveau is the kernel's own, and NVIDIA's is
# built for it by the package's DKMS, or not.
have() { modinfo -n "$1" >/dev/null 2>&1; }

# The processes holding GPU $1 open on driver $2: its DRM nodes, and for
# NVIDIA's also every /dev/nvidia*, one set for all of NVIDIA's GPUs -- not
# on nouveau, where a program with NVIDIA's userspace loaded holds
# /dev/nvidiactl and nothing of the GPU. One find, and not a readlink a
# descriptor: a desktop holds thousands. A buffer another driver imported
# and a descriptor in flight between two processes are in no table it
# reads; for nouveau the unload sees them, and for NVIDIA's they are an
# unbind that waits.
holders() {
    gpu=$1
    if [ "$2" = nvidia ]; then
        set -- -lname '/dev/nvidia*'
    else
        set -- -false
    fi
    for n in /sys/bus/pci/devices/"$gpu"/drm/*; do
        [ -e "$n" ] && set -- "$@" -o -lname "/dev/dri/${n##*/}"
    done
    find /proc/[0-9]*/fd -maxdepth 1 \( "$@" \) 2>/dev/null |
        cut -d/ -f3 | sort -un | while read -r pid; do
            echo "held: $pid $(cat "/proc/$pid/comm" 2>/dev/null)"
        done
}

# NVIDIA's persistence daemon holds the GPU open by design: set aside
# before the holders are looked for, and started again only once the GPU
# is on NVIDIA's module and ready -- where it was running, or is enabled.
persisted=
set_aside() {
    if systemctl is-active --quiet nvidia-persistenced 2>/dev/null; then
        systemctl stop nvidia-persistenced
        persisted=1
    fi
}
persist_back() {
    if [ "$(bound "$g")" = nvidia ] && { [ -n "$persisted" ] ||
            systemctl is-enabled --quiet nvidia-persistenced 2>/dev/null; }
    then
        systemctl start nvidia-persistenced
    fi
}

# The GPU let go of by driver $1 (see the top for why each its own way).
# NVIDIA's unbind is watched from beside it: a holder that came after the
# look is a write that sleeps until it closes, and its caller would see
# nothing for as long. The watcher ends itself once the GPU is let go, and
# is ended at once by USR1 -- TERM is ignored here, and so in it -- with
# its sleep's output not on the caller's pipe, which would hold it open.
# It is waited for with stderr shut, or bash reports the signal there.
# The lock is not passed to it, or a sleep it leaves holds the lock for
# its last half second.
leave() {
    case $1 in
    none) ;;
    nvidia)
        (
            i=0
            while sleep 0.5 >/dev/null 2>&1 &&
                    [ "$(bound "$g")" = nvidia ]; do
                i=$((i + 1))
                if [ $((i % 6)) = 0 ]; then
                    say "NVIDIA's module has waited $((i / 2)) s to let" \
                        "$g go, for:"
                    holders "$g" nvidia
                fi
            done
        ) 9>&- &
        watch=$!
        echo "$g" > /sys/bus/pci/drivers/nvidia/unbind
        r=$?
        { kill -USR1 "$watch"; wait "$watch"; } 2>/dev/null
        return "$r" ;;
    *) rmmod "$1" ;;
    esac
}

# The GPU bound to driver $1 through driver_override, so that no other
# driver takes it: by sysfs where the module is loaded, and otherwise by
# loading it, which binds the free GPU the override names it for. NVIDIA's
# companions and device nodes with it.
attach() {
    echo "$1" > "$P/driver_override"
    if [ -d "/sys/module/$1" ]; then
        echo "$g" > "/sys/bus/pci/drivers/$1/bind"
    elif [ "$1" = nvidia ]; then
        modprobe nvidia_drm modeset=1 && modprobe nvidia_uvm
    else
        modprobe "$1"
    fi
    r=$?
    echo > "$P/driver_override"
    [ "$r" = 0 ] || return 1
    # NVIDIA's nodes, which its module does not make: the GPU's and the
    # control node (minor 255), then UVM's and modeset's. A call takes one
    # branch -- with -u, -c names a minor of UVM's -- so each is a call of
    # its own. ready() waits for nvidiactl, which on a machine booted on
    # nouveau nothing else makes. Minor 0: a move is offered for one GPU.
    if [ "$1" = nvidia ] && command -v nvidia-modprobe >/dev/null; then
        nvidia-modprobe -c 0 -c 255
        nvidia-modprobe -u -c 0
        nvidia-modprobe -m
    fi
    return 0
}

# Bound to driver $1, and its nodes there for a program to open: udev
# makes a DRM node after the bind returns and gives it its group after
# that, and a node that is not yet the group's is not the user's to open
# (DESIGN *KMS: no window server*). A render node at least, which both drivers
# make in the bind. Five seconds for the bind and the nodes; one more for
# the group, past which it goes on -- a system whose rules give none
# would otherwise wait it out at every move. Not udevadm settle, which
# waits for every event queued on the machine.
ready() {
    i=0
    while [ "$(bound "$g")" != "$1" ]; do
        [ "$i" -ge 500 ] && return 1
        sleep 0.01
        i=$((i + 1))
    done
    i=0
    while :; do
        ok=1
        render=0
        for n in "$P"/drm/card* "$P"/drm/renderD*; do
            [ -e "$n" ] || continue
            case ${n##*/} in renderD*) render=1 ;; esac
            [ -e "/dev/dri/${n##*/}" ] || ok=0
        done
        [ "$render" = 1 ] || ok=0
        [ "$1" = nvidia ] && [ ! -e /dev/nvidiactl ] && ok=0
        [ "$ok" = 1 ] && break
        [ "$i" -ge 500 ] && return 1
        sleep 0.01
        i=$((i + 1))
    done
    i=0
    while [ "$i" -lt 100 ]; do
        ok=1
        for n in "$P"/drm/card* "$P"/drm/renderD*; do
            [ -e "$n" ] &&
                [ "$(stat -c %g "/dev/dri/${n##*/}" 2>/dev/null)" = 0 ] &&
                ok=0
        done
        [ "$ok" = 1 ] && return 0
        sleep 0.01
        i=$((i + 1))
    done
    return 0
}

# GPU $g ($P), on no driver, given one: NVIDIA's module first where it is
# loaded, which binds in 0.4 s, and the other where that one will not,
# each loaded first where it is not. $rebound names the driver that bound
# with its nodes. One that binds and makes none: NVIDIA's lets go safely,
# so the other is tried, and it is bound again where that one fails too
# -- a driver with no nodes is more than none; nouveau is never unbound,
# and is kept.
rebind() {
    rebound=
    if [ -d /sys/module/nvidia ]; then
        set -- nvidia nouveau
    else
        set -- nouveau nvidia
    fi
    for d; do
        [ "$(bound "$g")" = none ] || break
        have "$d" || continue
        if attach "$d" && ready "$d"; then
            rebound=$d
            return 0
        fi
        [ "$(bound "$g")" = nvidia ] && leave nvidia
    done
    [ "$(bound "$g")" = none ] && have nvidia && attach nvidia
    [ "$(bound "$g")" != none ]
}

list() {
    for g in $(gpus); do
        line="$g $(bound "$g")"
        for m in $DRIVERS; do
            have "$m" && line="$line $m"
        done
        echo "$line"
    done
}

# The lock, which every caller takes in turn: a move, a preload, a rescue.
lock() {
    m=$(umask)
    umask 077
    exec 9> "$LOCK" || fail "cannot open $LOCK"
    umask "$m"
    flock -w "$1" 9 || fail "another move held $LOCK for $1 s"
}

move() {
    want=$1
    case " $DRIVERS " in
    *" $want "*) ;;
    *) fail "no driver $want here: one of $DRIVERS" ;;
    esac
    g=${2:-$(gpus | head -n 1)}
    [ -n "$g" ] || fail "no NVIDIA GPU on this machine"
    case " $(gpus | tr '\n' ' ') " in
    *" $g "*) ;;
    *) fail "$g is not an NVIDIA display device here: one of" \
            "$(gpus | tr '\n' ' ')" ;;
    esac
    P=/sys/bus/pci/devices/$g
    have "$want" || fail "this kernel ($(uname -r)) has no $want module"
    [ "$(id -u)" = 0 ] || fail "binding a driver wants root"

    lock 30
    trap '' HUP INT TERM PIPE

    now=$(bound "$g")
    case " none $DRIVERS " in
    *" $now "*) ;;
    *) fail "$g is on $now, which this moves nothing from" ;;
    esac
    if [ "$now" = "$want" ]; then
        say "$g is on $want already"
        return 0
    fi
    [ "$now" = nvidia ] && set_aside
    held=$(holders "$g" "$now")
    if [ -n "$held" ]; then
        persist_back
        say "$g is held open, so $now stays bound:"
        echo "$held"
        return 2
    fi
    t0=$(now_ms)
    if ! leave "$now"; then
        persist_back
        say "$now would not let $g go"
        holders "$g" "$now"
        return 2
    fi
    t1=$(now_ms)
    if ! attach "$want"; then
        attached=
    else
        attached=1
        t2=$(now_ms)
    fi
    if [ -z "$attached" ] || ! ready "$want"; then
        [ "$(bound "$g")" = "$want" ] && leave "$want"
        if [ "$now" != none ] && [ "$(bound "$g")" = none ]; then
            attach "$now" && ready "$now"
        fi
        # A GPU on no driver is one nothing can draw on, a move from none
        # included, and nothing else would give it one: the service ends
        # well, so its OnFailure does not run, and the caller may be gone.
        # Where that binds the driver asked for, with its nodes, the move
        # is made.
        rebound=
        [ "$(bound "$g")" = none ] && rebind
        persist_back
        if [ "$rebound" = "$want" ]; then
            say "$g: $now to $want at the second try"
            return 0
        fi
        fail "$want did not bind $g; back on $(bound "$g")"
    fi
    t3=$(now_ms)
    persist_back
    say "$g: $now to $want in $((t3 - t0)) ms (let go $((t1 - t0))," \
        "bound $((t2 - t1)), nodes $((t3 - t2)))"
    return 0
}

# nouveau loaded ahead, while NVIDIA's module drives the GPU, so that the
# move onto it binds and does not load. The override, left in place, keeps
# nouveau off the GPU, which it would otherwise take whenever NVIDIA's
# module let go of it. Only for one NVIDIA GPU, as the library moves one;
# never where nouveau is loaded already, or the kernel has none. Run after
# the move it follows has been answered, never in its way.
preload() {
    lock 30
    set -- $(gpus)
    [ $# = 1 ] && [ "$(bound "$1")" = nvidia ] || return 0
    [ ! -d /sys/module/nouveau ] && have nouveau || return 0
    echo nvidia > "/sys/bus/pci/devices/$1/driver_override" &&
        modprobe nouveau
}

# The service killed in the middle of a move -- by SIGKILL, which nothing
# here can ignore -- leaves the GPU with no driver, and this is its
# OnFailure: given one as a move that missed both ways is (rebind()).
rescue() {
    lock 60
    trap '' HUP INT TERM PIPE
    for g in $(gpus); do
        P=/sys/bus/pci/devices/$g
        [ "$(bound "$g")" = none ] || continue
        if rebind; then
            say "$g had no driver; bound to $(bound "$g")"
        else
            say "$g had no driver, and neither driver bound it" >&2
        fi
    done
}

# One request a connection -- a driver's name and an address, as a move's
# arguments -- its words checked here before anything runs as root on
# them: the client is anyone in the socket's group. The status is written
# as a line, as the last thing the client is sent; a preload owed then
# runs with the connection closed. The list is not asked for here: what it
# reads is sysfs's, which the library reads itself.
serve() {
    trap '' HUP INT TERM PIPE
    w1= w2= w3=
    IFS=' ' read -r w1 w2 w3 || w1=
    ok=1
    case $w1 in
    nouveau | nvidia) ;;
    *) ok= ;;
    esac
    case $w2 in
    '') ;;
    [0-9a-f][0-9a-f][0-9a-f][0-9a-f]:[0-9a-f][0-9a-f]:[0-9a-f][0-9a-f].[0-7]) ;;
    *) ok= ;;
    esac
    [ -z "$w3" ] || ok=
    if [ -z "$ok" ]; then
        say "not a request: $w1 $w2 $w3"
        echo "status: 1"
        return
    fi
    (move "$w1" ${w2:+"$w2"}) 2>&1
    r=$?
    echo "status: $r"
    exec < /dev/null > /dev/null 2>&1
    [ "$r" = 0 ] && preload
    # The move's status is the client's; the service fails only where it
    # was killed, which is what its OnFailure answers.
    return 0
}

case ${1:-} in
list) list ;;
serve) serve ;;
rescue) rescue ;;
'') fail "usage: fenestal-gpu-driver list | <driver> [address] | serve" \
         "| rescue" ;;
*)
    move "$@"
    r=$?
    # The lock let go of first: a job started while this shell holds it is
    # a fork that holds it too, and preload, waiting for it, waited out its
    # 30 s and gave up. And the job's own output sent away, or the caller's
    # pipe stays open until preload ends.
    exec 9>&-
    if [ "$r" = 0 ]; then
        preload < /dev/null > /dev/null 2>&1 &
    fi
    exit "$r" ;;
esac
