#!/usr/bin/env bash
# Builds dist/Samplite-Explorer-<version>-macOS.pkg. Run from the project root after building.
set -euo pipefail
version=$(cat build/version.txt)
app="build/SampliteExplorer_artefacts/Release/Samplite Explorer.app"
stage=build/pkgroot
rm -rf "$stage" && mkdir -p "$stage" dist
ditto "$app" "$stage/Samplite Explorer.app"
mkdir -p "$stage/Samplite Explorer.app/Contents/Resources/licenses"
cp build/licenses/* "$stage/Samplite Explorer.app/Contents/Resources/licenses/"
codesign --force --deep --sign - "$stage/Samplite Explorer.app" # ad-hoc, not a developer ID

# Always install to /Applications, even if another copy of the app exists elsewhere.
pkgbuild --analyze --root "$stage" build/components.plist
/usr/libexec/PlistBuddy -c "Set :0:BundleIsRelocatable false" build/components.plist
pkgbuild --root "$stage" --component-plist build/components.plist \
  --identifier com.samplite.explorer --version "$version" --install-location /Applications \
  "dist/Samplite-Explorer-$version-macOS.pkg"
