#!/bin/bash
# Builds the Sudz Audio Unit for GarageBand, installs it, and validates it.
# Works with full Xcode, or with only Apple's Command Line Tools (for Macs that can't run the latest Xcode).
set -euo pipefail
cd "$(dirname "$0")"

if ! command -v cmake >/dev/null; then
  if [ -x "/Applications/CMake.app/Contents/bin/cmake" ]; then export PATH="/Applications/CMake.app/Contents/bin:$PATH"
  else echo "CMake is missing. Install it with: brew install cmake   (or the macOS installer from https://cmake.org/download/)"; exit 1; fi
fi

ARCH="$(uname -m)"                                           # build for this Mac only (arm64 or x86_64)
OSV="$(sw_vers -productVersion | cut -d. -f1-2)"             # and for the macOS it runs
if xcodebuild -version >/dev/null 2>&1; then
  echo "Building with Xcode ($(xcodebuild -version | head -1)) for macOS $OSV on $ARCH"
  GEN=(-G Xcode); CFG=(--config Release)
else
  if ! xcode-select -p >/dev/null 2>&1; then
    echo "Apple's Command Line Tools are needed (a free download, much smaller than Xcode)."
    echo "A window will ask to install them: click Install, wait for it to finish, then run this script again."
    xcode-select --install || true
    exit 1
  fi
  echo "Building with the Command Line Tools ($(pkgutil --pkg-info=com.apple.pkg.CLTools_Executables 2>/dev/null | grep version || echo 'version unknown')) for macOS $OSV on $ARCH"
  GEN=(-G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release); CFG=()
fi

cmake -B build "${GEN[@]}" -DCMAKE_OSX_ARCHITECTURES="$ARCH" -DCMAKE_OSX_DEPLOYMENT_TARGET="$OSV"
cmake --build build ${CFG[@]+"${CFG[@]}"} --target Sudz_AU Sudz_Standalone -j "$(sysctl -n hw.ncpu)"

# Install (replacing any older build)
mkdir -p "$HOME/Library/Audio/Plug-Ins/Components"
COMPONENT="$HOME/Library/Audio/Plug-Ins/Components/Sudz.component"
rm -rf "$COMPONENT"
cp -R "build/Sudz_artefacts/Release/AU/Sudz.component" "$COMPONENT"

# The native AI engine (ONNX Runtime 1.19.2, the newest that runs on macOS 11), shared by the plug-in and the app
LIB="$HOME/Library/Application Support/Sudz/lib"
if [ ! -f "$LIB/libonnxruntime.dylib" ]; then
  echo "Getting the AI engine (ONNX Runtime)…"
  T="$(mktemp -d)"
  if curl -fsSL -o "$T/ort.tgz" https://github.com/microsoft/onnxruntime/releases/download/v1.19.2/onnxruntime-osx-universal2-1.19.2.tgz && tar xzf "$T/ort.tgz" -C "$T"; then
    mkdir -p "$LIB"; cp "$T"/onnxruntime-osx-universal2-1.19.2/lib/libonnxruntime.1.19.2.dylib "$LIB/libonnxruntime.dylib"
    lipo "$LIB/libonnxruntime.dylib" -thin "$ARCH" -output "$LIB/libonnxruntime.thin" 2>/dev/null && mv "$LIB/libonnxruntime.thin" "$LIB/libonnxruntime.dylib"
    codesign --force --sign - "$LIB/libonnxruntime.dylib" 2>/dev/null || true
  else echo "Couldn't download the AI engine; Sudz will use its slower built-in one."; fi
  rm -rf "$T"
fi

# Make macOS rescan Audio Units, then validate (GarageBand only shows plug-ins that pass).
killall -9 AudioComponentRegistrar >/dev/null 2>&1 || true
auval -v aufx Sudz Sdzv

echo
echo "Installed: $COMPONENT"
echo "In GarageBand: Settings > Audio/MIDI > Enable Audio Units, then Smart Controls > Plug-ins > Audio Units > Sudz."
