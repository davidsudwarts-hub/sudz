#!/bin/bash
# Signs and notarises the Audio Unit and wraps it in an installer, so it installs and runs on other Macs.
#
# You need an Apple Developer Program membership, and once:
#   1. Two certificates in your keychain (Xcode > Settings > Accounts > Manage Certificates > +):
#        "Developer ID Application" and "Developer ID Installer"
#   2. An app-specific password from account.apple.com (Sign-In and Security > App-Specific Passwords)
#   3. A saved notarisation profile:
#        xcrun notarytool store-credentials sudz-notary \
#          --apple-id you@example.com --team-id YOURTEAMID --password xxxx-xxxx-xxxx-xxxx
#
# Then, after ./build-mac.sh:
#   APP_ID="Developer ID Application: Your Name (YOURTEAMID)" \
#   INSTALLER_ID="Developer ID Installer: Your Name (YOURTEAMID)" \
#   ./sign-mac.sh
#
# `security find-identity -v` lists the exact certificate names on your Mac.
set -euo pipefail
cd "$(dirname "$0")"

: "${APP_ID:?Set APP_ID to your Developer ID Application certificate name (see: security find-identity -v)}"
: "${INSTALLER_ID:?Set INSTALLER_ID to your Developer ID Installer certificate name}"
PROFILE="${NOTARY_PROFILE:-sudz-notary}"

COMP="build/Sudz_artefacts/Release/AU/Sudz.component"
[ -d "$COMP" ] || { echo "No built plug-in found. Run ./build-mac.sh first."; exit 1; }
VERSION=$(/usr/libexec/PlistBuddy -c "Print CFBundleShortVersionString" "$COMP/Contents/Info.plist" 2>/dev/null || echo "0.9.0")

OUT="dist"
rm -rf "$OUT"; mkdir -p "$OUT/root"
ditto "$COMP" "$OUT/root/Sudz.component"

echo "== Signing the plug-in (hardened runtime, secure timestamp)"
codesign --force --options runtime --timestamp --sign "$APP_ID" "$OUT/root/Sudz.component"
codesign --verify --strict --verbose=2 "$OUT/root/Sudz.component"

PKG="$OUT/Sudz-$VERSION.pkg"
echo "== Building the installer (installs to /Library/Audio/Plug-Ins/Components)"
pkgbuild --root "$OUT/root" \
         --identifier com.sudz.vocal.au \
         --version "$VERSION" \
         --install-location "/Library/Audio/Plug-Ins/Components" \
         --sign "$INSTALLER_ID" \
         "$PKG"

echo "== Notarising (this usually takes a few minutes)"
xcrun notarytool submit "$PKG" --keychain-profile "$PROFILE" --wait

echo "== Stapling the notarisation ticket, so it installs offline too"
xcrun stapler staple "$PKG"
spctl --assess --type install -vv "$PKG"

echo
echo "Ready to share: $PKG"
echo "On the other Mac: open the installer, then in GarageBand tick Settings > Audio/MIDI > Enable Audio Units."
