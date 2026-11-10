#!/bin/bash
# Wrap the macOS build in "F-117A Enhanced.app" (SDL3 bundled, icon, ad-hoc signed).
#   tools/make_macos_app.sh [OUT_DIR]      (run tools/build_macos.sh first; default OUT_DIR: release/)
# The game files are not bundled: on first launch the app asks for your F-117A folder and remembers it in
# ~/Library/Application Support/F-117A Enhanced/gamedir. Hold Option while opening to choose again.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/release}"; mkdir -p "$OUT"
BIN="$ROOT/f117port/build/f117-enhanced"
[ -x "$BIN" ] || { echo "Build first: tools/build_macos.sh"; exit 1; }
VER="$("$BIN" --version | awk '{print $2}')"
APP="$OUT/F-117A Enhanced.app"; C="$APP/Contents"
rm -rf "$APP"; mkdir -p "$C/MacOS" "$C/Resources" "$C/Frameworks"

# Engine + SDL3, rewired to load SDL3 from inside the bundle.
cp "$BIN" "$C/MacOS/f117-enhanced"
SDL="$(otool -L "$BIN" | awk '/libSDL3/ {print $1}')"
cp -L "$SDL" "$C/Frameworks/"; chmod u+w "$C/Frameworks/$(basename "$SDL")"
install_name_tool -id "@rpath/$(basename "$SDL")" "$C/Frameworks/$(basename "$SDL")"
install_name_tool -change "$SDL" "@rpath/$(basename "$SDL")" "$C/MacOS/f117-enhanced"
install_name_tool -add_rpath "@executable_path/../Frameworks" "$C/MacOS/f117-enhanced"

# Front: find the game folder (remembered, or ask), then start the engine.
cat > "$C/MacOS/F-117A Enhanced" <<'S'
#!/bin/bash
HERE="$(cd "$(dirname "$0")" && pwd)"
CFG="$HOME/Library/Application Support/F-117A Enhanced"; mkdir -p "$CFG"
GAME="$(cat "$CFG/gamedir" 2>/dev/null || true)"
OPT="$(osascript -l JavaScript -e 'ObjC.import("AppKit"); ($.NSEvent.modifierFlags & (1<<19)) ? "1" : "0"' 2>/dev/null)"
if [ "$OPT" = "1" ] || [ ! -f "$GAME/VGAME.EXE" ]; then
  GAME="$(osascript -e 'POSIX path of (choose folder with prompt "Choose your F-117A Nighthawk 2.0 game folder (the one with VGAME.EXE):")' 2>/dev/null)" || exit 0
  GAME="${GAME%/}"; echo "$GAME" > "$CFG/gamedir"
fi
EXTRA=(); [ -f "$CFG/options" ] && read -r -a EXTRA < "$CFG/options"
exec "$HERE/f117-enhanced" --game-dir "$GAME" "${EXTRA[@]}"
S
chmod +x "$C/MacOS/F-117A Enhanced"

# Icon from the launcher's .ico (256 px).
python3 - "$ROOT/f117port/launcher/app.ico" "$OUT/icon.iconset" <<'P'
import sys, os
from PIL import Image
im = Image.open(sys.argv[1]); im.size = max(im.info.get("sizes", [im.size])); im = im.convert("RGBA")
d = sys.argv[2]; os.makedirs(d, exist_ok=True)
for s in (16, 32, 128, 256, 512):
    im.resize((s, s), Image.LANCZOS).save(f"{d}/icon_{s}x{s}.png")
    im.resize((s*2, s*2), Image.LANCZOS).save(f"{d}/icon_{s}x{s}@2x.png")
P
iconutil -c icns "$OUT/icon.iconset" -o "$C/Resources/AppIcon.icns"; rm -rf "$OUT/icon.iconset"

cat > "$C/Info.plist" <<P
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>F-117A Enhanced</string>
  <key>CFBundleDisplayName</key><string>F-117A Enhanced</string>
  <key>CFBundleIdentifier</key><string>com.github.f117-2enhanced</string>
  <key>CFBundleVersion</key><string>$VER</string>
  <key>CFBundleShortVersionString</key><string>$VER</string>
  <key>CFBundleExecutable</key><string>F-117A Enhanced</string>
  <key>CFBundleIconFile</key><string>AppIcon</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.simulation-games</string>
  <key>LSMinimumSystemVersion</key><string>12.0</string>
  <key>NSHighResolutionCapable</key><true/>
</dict></plist>
P

codesign --force --deep --sign - "$APP"
echo "Made: $APP"
