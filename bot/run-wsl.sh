#!/usr/bin/env bash
#
# Runs the bot in WSL, over Daily: daily-python, which the Daily transport
# needs, is for Linux and macOS only. The game, on Windows, starts it at
# http://localhost:7860/start (WSL forwards the port), and the two meet in a
# Daily room the runner makes with DAILY_API_KEY, from .env.
#
# From PowerShell, in this directory:
#
#   wsl bash ./run-wsl.sh
#
set -euo pipefail
cd "$(dirname "$0")"

# uv, if WSL doesn't have it yet.
export PATH="$HOME/.local/bin:$PATH"
if ! command -v uv >/dev/null 2>&1; then
    echo "Installing uv in WSL..."
    curl -LsSf https://astral.sh/uv/install.sh | sh
fi

# Linux's own environment, apart from Windows' .venv in this directory.
export UV_PROJECT_ENVIRONMENT="$HOME/.cache/pipecat-room/venv"
exec uv run bot.py -t daily --host 0.0.0.0 "$@"
