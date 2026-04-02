#!/usr/bin/env bash
#
# scripts/macos-waypipe-client.sh
#
# macOS side of the waypipe forwarding setup.
#
# Counterpart to the container-side VS Code tasks:
#   "waypipe: 1. socat tunnel"  -- container socat connects to BIND_IP:PORT
#   "waypipe: 2. server"        -- container waypipe server
#
# Flow:
#   1. waypipe client connects to the local Wayland compositor and creates
#      WAYPIPE_SOCK for the container-side waypipe server to connect through.
#   2. socat listens on BIND_IP:PORT and forwards each connection to WAYPIPE_SOCK.
#
# All parameters can be overridden via environment variables:
#   BIND_IP          macOS host IP as seen from the container
#                    (must match tasks.json: "socat ... TCP:<IP>:12345")
#   PORT             TCP port number (default: 12345)
#   WAYLAND_DISPLAY  local Wayland socket name  (default: wayland-0)
#   XDG_RUNTIME_DIR  local XDG runtime dir      (default: /tmp/xdg)
#   WAYPIPE_SOCK     waypipe Unix socket path    (default: /tmp/waypipe-client.sock)

set -euo pipefail

BIND_IP="${BIND_IP:-192.168.64.1}"
PORT="${PORT:-12345}"
WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}"
XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp/xdg}"
WAYPIPE_SOCK="${WAYPIPE_SOCK:-/tmp/waypipe-client.sock}"

mkdir -p "$XDG_RUNTIME_DIR"

# Remove any leftover socket from a previous (crashed) run so socat does not
# connect to a dead socket on the first attempt.
rm -f "$WAYPIPE_SOCK"

# Start waypipe client in the background.  It connects to the local compositor
# via WAYLAND_DISPLAY and exposes WAYPIPE_SOCK for the container to reach it.
WAYLAND_DISPLAY="$WAYLAND_DISPLAY" XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" \
    waypipe-c --socket "$WAYPIPE_SOCK" --debug client &
WAYPIPE_PID=$!

cleanup() {
    kill "$WAYPIPE_PID" 2>/dev/null || true
    wait "$WAYPIPE_PID" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

# Wait for waypipe to create the socket before socat tries to connect to it.
until [ -S "$WAYPIPE_SOCK" ]; do sleep 0.1; done

# Bridge TCP → waypipe Unix socket.  Runs in the foreground; the trap above
# tears down waypipe when socat exits or the script is interrupted.
socat \
    "TCP4-LISTEN:${PORT},bind=${BIND_IP},reuseaddr,fork" \
    "UNIX-CONNECT:${WAYPIPE_SOCK}"
