#!/bin/bash
# Installs the Sudz Audio Unit for GarageBand (and the Sudz app), then checks it.
cd "$(dirname "$0")"
set -e
DEST="$HOME/Library/Audio/Plug-Ins/Components"
mkdir -p "$DEST"
rm -rf "$DEST/Sudz.component"
cp -R Sudz.component "$DEST/"
xattr -dr com.apple.quarantine "$DEST/Sudz.component" 2>/dev/null || true
if [ -d Sudz.app ]; then
  APPS="/Applications"; [ -w "$APPS" ] || APPS="$HOME/Applications"; mkdir -p "$APPS"
  rm -rf "$APPS/Sudz.app"; cp -R Sudz.app "$APPS/"
  xattr -dr com.apple.quarantine "$APPS/Sudz.app" 2>/dev/null || true
  echo "Sudz app installed in $APPS"
fi
killall -9 AudioComponentRegistrar >/dev/null 2>&1 || true
if auval -v aufx Sudz Sdzv | grep -q "AU VALIDATION SUCCEEDED"; then
  echo; echo "Sudz is installed and passed Apple's check. Quit and reopen GarageBand, then remove and re-add Sudz on your track."
else
  echo; echo "Installed, but Apple's check didn't pass. Run: auval -v aufx Sudz Sdzv   and send the end of what it prints."
fi
