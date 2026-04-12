#!/bin/sh
# Starts gdbserver on the target in --multi --once (extended-remote, single-session)
# mode for qtshell.
# --once makes gdbserver kill the inferior and exit when the GDB client disconnects,
# ensuring a clean state for the next debug session.
# Kills any existing process on port 2346 first, then pre-creates the binary
# with the execute bit set — remote put overwrites the content but does not set +x.
#
# QV4_NO_JIT=1: V4 JIT-compiled code bypasses QML debugger breakpoint hooks.
# Setting it here (in gdbserver's own environment) is the only reliable method —
# GDB's "set environment" relies on the QEnviron packet which minimal embedded
# gdbserver builds may not support.
exec ssh microscope '
    fuser -k 2346/tcp >/dev/null 2>&1 || true
    rm /tmp/runtime-root/wayland-0 >/dev/null 2>&1 || true
    touch /mnt/sda1/qtshell && chmod +x /mnt/sda1/qtshell
    QV4_NO_JIT=1 /qtshell.sh gdbserver --multi --once 0.0.0.0:2346
'
