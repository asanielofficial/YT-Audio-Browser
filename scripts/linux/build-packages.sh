#!/usr/bin/env bash
# Builds dist/YtAudioBrowser-<ver>-linux-x86_64.tar.gz and dist/ytaudiobrowser_<ver>_amd64.deb
# from an already-built build/YtAudioBrowser.clap.  Usage: scripts/linux/build-packages.sh [build-dir]
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build="${1:-$root/build-linux}"
ver="$(sed -n 's/^project(YtAudioBrowser VERSION \([0-9.]*\).*/\1/p' "$root/CMakeLists.txt")"
clap="$build/YtAudioBrowser.clap"
[ -f "$clap" ] || { echo "Build the plugin first: $clap missing" >&2; exit 1; }
dist="$root/dist"; mkdir -p "$dist"
stage="$(mktemp -d)"; trap 'rm -rf "$stage"' EXIT
strip --strip-unneeded -o "$stage/YtAudioBrowser.clap" "$clap"

# --- tar.gz ------------------------------------------------------------------
name="YtAudioBrowser-$ver-linux-x86_64"
mkdir -p "$stage/$name"
cp "$stage/YtAudioBrowser.clap" "$root/scripts/linux/install.sh" "$stage/$name/"
cp "$root/README.md" "$stage/$name/"
chmod 755 "$stage/$name/install.sh"
tar -C "$stage" --owner=0 --group=0 -czf "$dist/$name.tar.gz" "$name"

# --- .deb --------------------------------------------------------------------
pkg="$stage/deb"
mkdir -p "$pkg/DEBIAN" "$pkg/usr/lib/clap" "$pkg/usr/bin" "$pkg/usr/share/doc/ytaudiobrowser"
install -m 755 "$stage/YtAudioBrowser.clap" "$pkg/usr/lib/clap/YtAudioBrowser.clap"
install -m 755 "$root/scripts/linux/install.sh" "$pkg/usr/share/doc/ytaudiobrowser/.setup-real"
mv "$pkg/usr/share/doc/ytaudiobrowser/.setup-real" "$pkg/usr/bin/ytaudiobrowser-setup"
cat > "$pkg/usr/bin/ytaudiobrowser-setup" <<'SETUP'
#!/bin/sh
# Downloads yt-dlp, deno and (if missing) ffmpeg for the current user.
exec /usr/share/ytaudiobrowser/install.sh --tools-only "$@"
SETUP
chmod 755 "$pkg/usr/bin/ytaudiobrowser-setup"
mkdir -p "$pkg/usr/share/ytaudiobrowser"
install -m 755 "$root/scripts/linux/install.sh" "$pkg/usr/share/ytaudiobrowser/install.sh"
cp "$root/README.md" "$pkg/usr/share/doc/ytaudiobrowser/README.md"
size="$(du -sk "$pkg" | cut -f1)"
cat > "$pkg/DEBIAN/control" <<CTRL
Package: ytaudiobrowser
Version: $ver
Section: sound
Priority: optional
Architecture: amd64
Depends: libgtk-3-0, libcurl4, libc6 (>= 2.31)
Recommends: ffmpeg
Installed-Size: $size
Maintainer: YT Audio Browser <noreply@example.invalid>
Description: CLAP plugin to search and download audio with yt-dlp
 Search YouTube, SoundCloud, Bandcamp and more from a plugin window and
 drag the downloaded audio into your DAW. After installing, run
 "ytaudiobrowser-setup" once as your normal user to download yt-dlp and deno.
CTRL
cat > "$pkg/DEBIAN/postinst" <<'POST'
#!/bin/sh
set -e
echo "YT Audio Browser installed. Run 'ytaudiobrowser-setup' as your normal user to download yt-dlp and deno."
exit 0
POST
chmod 755 "$pkg/DEBIAN/postinst"
dpkg-deb --root-owner-group --build "$pkg" "$dist/ytaudiobrowser_${ver}_amd64.deb" >/dev/null
ls -la "$dist"/*linux* "$dist"/*.deb
