#!/usr/bin/env bash
# Build, install, and enable selsync for the current user on any Linux machine.
set -euo pipefail
cd "$(dirname "$0")"

./build.sh

install -Dm755 selsync "$HOME/.local/bin/selsync"
install -Dm644 selsync.service "$HOME/.config/systemd/user/selsync.service"

systemctl --user daemon-reload
systemctl --user enable --now selsync.service
echo
systemctl --user --no-pager status selsync.service | head -n 5 || true
echo
echo "Installed. Highlight text anywhere -> Ctrl+V pastes it."
