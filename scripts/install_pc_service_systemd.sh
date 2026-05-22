#!/usr/bin/env bash
set -euo pipefail

SERVICE_NAME="${SERVICE_NAME:-esp32-system-monitor-pc}"
SERVICE_USER="${SERVICE_USER:-${SUDO_USER:-$(id -un)}}"
SERVICE_GROUP="${SERVICE_GROUP:-$(id -gn "$SERVICE_USER")}"
SERVICE_HOST="${SERVICE_HOST:-0.0.0.0}"
SERVICE_PORT="${SERVICE_PORT:-8080}"
SERVICE_TARGET="${SERVICE_TARGET:-}"
SERVICE_DISCOVERY="${SERVICE_DISCOVERY:-auto}"
INSTALL_SCOPE="${INSTALL_SCOPE:-system}"

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
PC_SERVICE_DIR="$REPO_ROOT/pc_service"
VENV_DIR="${VENV_DIR:-$PC_SERVICE_DIR/.venv}"
if [[ "$INSTALL_SCOPE" == "user" ]]; then
  UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
  UNIT_PATH="$UNIT_DIR/${SERVICE_NAME}.service"
else
  UNIT_DIR="/etc/systemd/system"
  UNIT_PATH="$UNIT_DIR/${SERVICE_NAME}.service"
fi

if ! command -v python3 >/dev/null 2>&1; then
  echo "python3 is required" >&2
  exit 1
fi

if ! command -v systemctl >/dev/null 2>&1; then
  echo "systemctl is required" >&2
  exit 1
fi

echo "Preparing Python virtualenv: $VENV_DIR"
python3 -m venv "$VENV_DIR"
"$VENV_DIR/bin/python" -m pip install --no-cache-dir -r "$PC_SERVICE_DIR/requirements.txt"

echo "Installing systemd unit: $UNIT_PATH"
if [[ "$INSTALL_SCOPE" == "user" ]]; then
  mkdir -p "$UNIT_DIR"
  UNIT_WRITER=(tee "$UNIT_PATH")
else
  if ! sudo -n true 2>/dev/null; then
    echo "sudo is required for INSTALL_SCOPE=system. Run this script from a terminal with sudo access, or use:" >&2
    echo "  INSTALL_SCOPE=user $0" >&2
    exit 1
  fi
  UNIT_WRITER=(sudo tee "$UNIT_PATH")
fi

"${UNIT_WRITER[@]}" >/dev/null <<UNIT
[Unit]
Description=ESP32 System Monitor PC telemetry service
$(if [[ "$INSTALL_SCOPE" == "system" ]]; then printf 'Wants=network-online.target\nAfter=network-online.target\n'; else printf 'After=default.target\n'; fi)

[Service]
Type=simple
$(if [[ "$INSTALL_SCOPE" == "system" ]]; then printf 'User=%s\nGroup=%s\n' "$SERVICE_USER" "$SERVICE_GROUP"; fi)
WorkingDirectory=$PC_SERVICE_DIR
Environment=PYTHONUNBUFFERED=1
ExecStart=$VENV_DIR/bin/python $PC_SERVICE_DIR/monitor_service.py --host $SERVICE_HOST --port $SERVICE_PORT --allow-manual-targets$(if [[ -n "$SERVICE_TARGET" ]]; then printf ' --target %s' "$SERVICE_TARGET"; fi)$(if [[ "$SERVICE_DISCOVERY" == "0" || "$SERVICE_DISCOVERY" == "false" || ( "$SERVICE_DISCOVERY" == "auto" && -n "$SERVICE_TARGET" ) ]]; then printf ' --no-discovery'; fi)
Restart=always
RestartSec=3

# Keep the monitor lightweight without hiding host metrics from psutil.
Nice=5
CPUWeight=20
IOWeight=20
MemoryHigh=128M
MemoryMax=256M
TasksMax=64

# Basic hardening that still allows LAN discovery and HTTP telemetry.
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=full
RestrictAddressFamilies=AF_INET AF_INET6 AF_UNIX AF_NETLINK AF_PACKET
SystemCallArchitectures=native
LockPersonality=true

[Install]
WantedBy=$(if [[ "$INSTALL_SCOPE" == "system" ]]; then printf 'multi-user.target'; else printf 'default.target'; fi)
UNIT

echo "Reloading systemd and starting $SERVICE_NAME"
if [[ "$INSTALL_SCOPE" == "user" ]]; then
  systemctl --user daemon-reload
  systemctl --user enable --now "$SERVICE_NAME"
  systemctl --user restart "$SERVICE_NAME"
else
  sudo systemctl daemon-reload
  sudo systemctl enable --now "$SERVICE_NAME"
  sudo systemctl restart "$SERVICE_NAME"
fi

echo
if [[ "$INSTALL_SCOPE" == "user" ]]; then
  systemctl --user --no-pager --full status "$SERVICE_NAME"
else
  sudo systemctl --no-pager --full status "$SERVICE_NAME"
fi
