#!/usr/bin/env bash
# Builds dist/samplite-explorer_<version>_<arch>.deb. Run from the project root after building.
set -euo pipefail
version=$(cat build/version.txt)
arch=$(dpkg --print-architecture)
root=build/debroot
rm -rf "$root"
mkdir -p "$root/DEBIAN" "$root/opt/samplite-explorer" "$root/usr/bin" \
         "$root/usr/share/applications" "$root/usr/share/doc/samplite-explorer" dist

install -m 755 "build/SampliteExplorer_artefacts/Release/Samplite Explorer" "$root/opt/samplite-explorer/samplite-explorer"
strip "$root/opt/samplite-explorer/samplite-explorer"
ln -s /opt/samplite-explorer/samplite-explorer "$root/usr/bin/samplite-explorer"
install -m 644 build/licenses/* "$root/usr/share/doc/samplite-explorer/"

cat > "$root/usr/share/applications/samplite-explorer.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=Samplite Explorer
Comment=Browse audio samples and synth presets
Exec=samplite-explorer
Terminal=false
Categories=AudioVideo;Audio;
DESKTOP

cat > "$root/DEBIAN/control" <<CONTROL
Package: samplite-explorer
Version: $version
Section: sound
Priority: optional
Architecture: $arch
Maintainer: Samplite <noreply@example.com>
Depends: libc6, libstdc++6, libasound2t64 | libasound2, libfreetype6, libfontconfig1, libx11-6, libxext6, libxinerama1, libxrandr2, libxcursor1
Description: Browser for audio samples and synth presets
 Preview, search and drag out samples, with tempo, key and tuning detection.
CONTROL

dpkg-deb --root-owner-group --build "$root" "dist/samplite-explorer_${version}_${arch}.deb"
