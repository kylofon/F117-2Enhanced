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
# options: shell words, so a path with spaces can be quoted ('/my dir/x'); it is the user's own file.
EXTRA=(); [ -f "$CFG/options" ] && eval "EXTRA=( $(cat "$CFG/options") )"
exec "$HERE/f117-enhanced" --game-dir "$GAME" "${EXTRA[@]}"
S
chmod +x "$C/MacOS/F-117A Enhanced"

# Icon from the launcher's .ico: its largest entry (a 256 px PNG), sized with sips (no Pillow needed).
ICS="$OUT/icon.iconset"; mkdir -p "$ICS"
python3 - "$ROOT/f117port/launcher/app.ico" "$ICS/src.png" <<'P'
import struct, sys
d = open(sys.argv[1], "rb").read()
n = struct.unpack_from("<H", d, 4)[0]
e = max((struct.unpack_from("<BBBBHHII", d, 6 + 16 * i) for i in range(n)), key=lambda t: t[0] or 256)
open(sys.argv[2], "wb").write(d[e[7]:e[7] + e[6]])
P
for s in 16 32 128 256 512; do
  sips -z $s $s "$ICS/src.png" --out "$ICS/icon_${s}x${s}.png" >/dev/null
  sips -z $((s*2)) $((s*2)) "$ICS/src.png" --out "$ICS/icon_${s}x${s}@2x.png" >/dev/null
done
rm "$ICS/src.png"
iconutil -c icns "$ICS" -o "$C/Resources/AppIcon.icns"; rm -rf "$ICS"

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
