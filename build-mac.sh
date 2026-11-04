#!/bin/bash
# DevRev — macOS build: VST3 + AudioUnit (AUv2), universal (Apple Silicon + Intel).
# Requirements: full Xcode + CMake.
#   ./build-mac.sh              build, check, sign, install for this user
#   NO_INSTALL=1 ./build-mac.sh build + check + package only
#   CI=1 ./build-mac.sh         (GitHub Actions) also installs on the runner and runs validator + auval
#   ARCHS=arm64 ./build-mac.sh  single architecture
set -euo pipefail
VST3_SDK_TAG="v3.8.1_build_84"
AU_SDK_TAG="AudioUnitSDK-1.4.0"
ARCHS="${ARCHS:-arm64;x86_64}"
ROOT="$(cd "$(dirname "$0")" && pwd)"
DEPS="$ROOT/deps"; BUILD="$ROOT/build-mac"; DIST="$ROOT/dist-mac"
say()  { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
fail() { printf '\n\033[31mHATA: %s\033[0m\n' "$*" >&2; exit 1; }

say "Araclar"
command -v cmake >/dev/null || fail "CMake yok (brew install cmake)"
xcodebuild -version >/dev/null 2>&1 || fail "Tam Xcode gerekli"
xcodebuild -version | head -1; cmake --version | head -1

mkdir -p "$DEPS"
[ -d "$DEPS/vst3sdk/pluginterfaces" ] || git clone --depth 1 --branch "$VST3_SDK_TAG" --recursive --shallow-submodules \
    https://github.com/steinbergmedia/vst3sdk.git "$DEPS/vst3sdk"
[ -f "$DEPS/AudioUnitSDK/AudioUnitSDK.xcodeproj/project.pbxproj" ] || git clone --depth 1 --branch "$AU_SDK_TAG" \
    https://github.com/apple/AudioUnitSDK.git "$DEPS/AudioUnitSDK"

say "Derleniyor ($ARCHS)"
cmake -S "$ROOT" -B "$BUILD" -G Xcode \
  -DVST3_SDK_ROOT="$DEPS/vst3sdk" \
  -DSMTG_AUDIOUNIT_SDK_PATH="$DEPS/AudioUnitSDK" \
  -DCMAKE_OSX_ARCHITECTURES="$ARCHS" \
  -DSMTG_DISABLE_CODE_SIGNING=ON
cmake --build "$BUILD" --config Release --target DevRev -- -quiet
cmake --build "$BUILD" --config Release --target DevRev-AU -- -quiet
cmake --build "$BUILD" --config Release --target devrev_cli -- -quiet

VST3_SRC="$BUILD/VST3/Release/DevRev.vst3"
AU_SRC="$BUILD/VST3/Release/DevRev.component"
[ -d "$VST3_SRC" ] || fail "VST3 ciktisi yok: $VST3_SRC"
[ -d "$AU_SRC" ] || AU_SRC="$(find "$BUILD" -name 'DevRev.component' -maxdepth 4 | head -1)"
[ -d "$AU_SRC" ] || fail "AU ciktisi yok"

say "DSP kontrolu: Mac ciktisi Linux/Windows ciktisiyla ayni mi?"
CLI="$(find "$BUILD" -name devrev_cli -type f -perm -u+x | head -1)"
"$CLI" "$ROOT/test/echo_in.wav" "$BUILD/echo_mac.wav"
python3 "$ROOT/tools/compare.py" "$BUILD/echo_mac.wav" "$ROOT/test/echo_expected_linux.wav" || fail "DSP ciktisi referanstan farkli"

say "Paketleniyor ve imzalaniyor (ad-hoc)"
rm -rf "$DIST"; mkdir -p "$DIST"
cp -R "$VST3_SRC" "$DIST/DevRev.vst3"
cp -RL "$AU_SRC" "$DIST/DevRev.component"
codesign --force --deep --sign - "$DIST/DevRev.vst3"
codesign --force --deep --sign - "$DIST/DevRev.component"
codesign --verify --deep --strict "$DIST/DevRev.vst3"
codesign --verify --deep --strict "$DIST/DevRev.component"
echo "VST3 mimari: $(lipo -archs "$DIST/DevRev.vst3/Contents/MacOS/DevRev")"
echo "AU   mimari: $(lipo -archs "$DIST/DevRev.component/Contents/MacOS/DevRev")"
cp "$ROOT/MAC-KURULUM.txt" "$DIST/"
(cd "$DIST" && ditto -c -k --keepParent DevRev.vst3 DevRev-mac-vst3.zip \
            && ditto -c -k --keepParent DevRev.component DevRev-mac-au.zip \
            && zip -qry DevRev-1.0-mac.zip DevRev.vst3 DevRev.component MAC-KURULUM.txt)

[ "${NO_INSTALL:-0}" = "1" ] && { say "Bitti: $DIST"; exit 0; }

say "Kuruluyor: ~/Library/Audio/Plug-Ins"
mkdir -p "$HOME/Library/Audio/Plug-Ins/VST3" "$HOME/Library/Audio/Plug-Ins/Components"
rm -rf "$HOME/Library/Audio/Plug-Ins/VST3/DevRev.vst3" "$HOME/Library/Audio/Plug-Ins/Components/DevRev.component"
cp -R "$DIST/DevRev.vst3" "$HOME/Library/Audio/Plug-Ins/VST3/"
cp -R "$DIST/DevRev.component" "$HOME/Library/Audio/Plug-Ins/Components/"
xattr -dr com.apple.quarantine "$HOME/Library/Audio/Plug-Ins/VST3/DevRev.vst3" 2>/dev/null || true
xattr -dr com.apple.quarantine "$HOME/Library/Audio/Plug-Ins/Components/DevRev.component" 2>/dev/null || true
killall -9 AudioComponentRegistrar 2>/dev/null || true

if [ "${CI:-0}" != "0" ]; then
  say "Steinberg validator"
  cmake --build "$BUILD" --config Release --target validator -- -quiet || echo "validator derlenemedi"
  VAL="$(find "$BUILD" -name validator -type f -perm -u+x | head -1)"
  if [ -n "$VAL" ]; then "$VAL" "$DIST/DevRev.vst3" > "$DIST/validator.log" 2>&1 || true; grep "Result:" "$DIST/validator.log" | tail -1; fi
  say "auval"
  sleep 3
  auval -v aufx dvrv DvRv > "$DIST/auval.log" 2>&1 && echo "AU VALIDATION SUCCEEDED" || { tail -40 "$DIST/auval.log"; fail "auval basarisiz"; }
  grep -E "AU VALIDATION|PASS" "$DIST/auval.log" | tail -3
fi
say "Bitti"
