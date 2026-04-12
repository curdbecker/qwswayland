#!/bin/sh
# Waits for the Wayland compositor socket, then starts gdbserver on the target
# in --multi --once (extended-remote, single-session) mode for qwswayland.
# --once makes gdbserver kill the inferior and exit when the GDB client disconnects,
# ensuring a clean state for the next debug session.
# Kills any existing process on port 2345 first, then pre-creates the binary
# with the execute bit set — remote put overwrites the content but does not set +x.

# Wait until the compositor (qtshell) has created its Wayland socket before
# launching qwswayland — the proxy cannot connect to a compositor that isn't up yet.

exec ssh microscope '
    sleep 2
    until test -S /tmp/runtime-root/wayland-0; do
        sleep 0.5
    done
    fuser -k 2345/tcp >/dev/null 2>&1 || true
    touch /mnt/sda1/qwswayland && chmod +x /mnt/sda1/qwswayland
    /qwswayland.sh gdbserver --multi --once 0.0.0.0:2345
'
