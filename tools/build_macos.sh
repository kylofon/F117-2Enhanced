#!/bin/bash
# Build F-117A Enhanced on macOS with Homebrew (tested on Apple Silicon; Intel untested).
#   tools/build_macos.sh [GAME_DIR]
# GAME_DIR: your own F-117A 2.0 files (default: ./Game). Writes play.command at the repo root.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GAME="$(cd "${1:-$ROOT/Game}" && pwd)"
command -v brew >/dev/null || { echo "Install Homebrew first: https://brew.sh"; exit 1; }
brew list sdl3 >/dev/null 2>&1 || brew install sdl3
for t in cmake ninja; do command -v $t >/dev/null || brew install $t; done
cmake -S "$ROOT/f117port" -B "$ROOT/f117port/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DF117_LAUNCHER=OFF -DCMAKE_PREFIX_PATH="$(brew --prefix)"
cmake --build "$ROOT/f117port/build"
"$ROOT/f117port/build/f117-enhanced" --check --game-dir "$GAME"
cat > "$ROOT/play.command" <<P
#!/bin/bash
exec "$ROOT/f117port/build/f117-enhanced" --game-dir "$GAME" "\$@"
P
chmod +x "$ROOT/play.command"
echo "Built. Double-click play.command, or: ./play.command --fullscreen --render-scale 4"
