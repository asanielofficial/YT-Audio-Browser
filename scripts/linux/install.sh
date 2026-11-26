#!/usr/bin/env bash
# YT Audio Browser installer for Linux (per-user, no root needed).
#   ./install.sh                       plugin -> ~/.clap, tools -> ~/.local/share/YtAudioBrowser
#   ./install.sh --plugin-dir DIR      put the .clap somewhere else (add it to your DAW's scan paths)
#   ./install.sh --tools-dir DIR       put yt-dlp, deno and ffmpeg somewhere else
#   ./install.sh --tools-only          only download the tools (used after the .deb install)
#   ./install.sh --no-tools            only copy the plugin
#   ./install.sh --offline-tools DIR   take yt-dlp / deno / ffmpeg from DIR instead of downloading
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
plugin_dir="${HOME}/.clap"
tools_dir="${XDG_DATA_HOME:-$HOME/.local/share}/YtAudioBrowser"
do_plugin=1 do_tools=1 offline=""

usage() { sed -n '2,9p' "$0" | sed 's/^# \{0,1\}//'; }

while [ $# -gt 0 ]; do
  case "$1" in
    --plugin-dir) plugin_dir="$2"; shift 2 ;;
    --tools-dir) tools_dir="$2"; shift 2 ;;
    --tools-only) do_plugin=0; shift ;;
    --no-tools) do_tools=0; shift ;;
    --offline-tools) offline="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

if [ "$(id -u)" = 0 ]; then
  echo "Run this as your normal user, not root: the tools are installed per user." >&2
  exit 1
fi
if [ "$(uname -m)" != x86_64 ]; then
  echo "Only x86_64 is supported by this installer (found $(uname -m))." >&2
  exit 1
fi

fetch() { # url dest
  if command -v curl >/dev/null; then curl -fL --retry 3 -o "$2" "$1"
  elif command -v wget >/dev/null; then wget -O "$2" "$1"
  else echo "Need curl or wget to download tools." >&2; exit 1; fi
}

config_dir="${XDG_CONFIG_HOME:-$HOME/.config}/YtAudioBrowser"
ini="$config_dir/settings.ini"

# --- plugin ------------------------------------------------------------------
if [ "$do_plugin" = 1 ]; then
  src="$here/YtAudioBrowser.clap"
  [ -f "$src" ] || { echo "YtAudioBrowser.clap not found next to install.sh" >&2; exit 1; }
  mkdir -p "$plugin_dir"
  install -m 755 "$src" "$plugin_dir/YtAudioBrowser.clap"
  echo "Plugin:  $plugin_dir/YtAudioBrowser.clap"
fi

# --- tools -------------------------------------------------------------------
if [ "$do_tools" = 1 ]; then
  bin="$tools_dir/bin"
  mkdir -p "$bin"
  tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT

  if [ -n "$offline" ]; then
    for t in yt-dlp deno ffmpeg; do
      [ -f "$offline/$t" ] && install -m 755 "$offline/$t" "$bin/$t"
    done
  else
    echo "Downloading yt-dlp..."
    fetch https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp_linux "$tmp/yt-dlp"
    install -m 755 "$tmp/yt-dlp" "$bin/yt-dlp"

    echo "Downloading deno (the JavaScript runtime YouTube needs)..."
    fetch https://github.com/denoland/deno/releases/latest/download/deno-x86_64-unknown-linux-gnu.zip "$tmp/deno.zip"
    ( cd "$tmp" && python3 -c 'import sys,zipfile; zipfile.ZipFile("deno.zip").extract("deno")' 2>/dev/null \
        || unzip -o -q deno.zip deno )
    install -m 755 "$tmp/deno" "$bin/deno"

    if command -v ffmpeg >/dev/null; then
      echo "Using the ffmpeg already on your system: $(command -v ffmpeg)"
    else
      echo "ffmpeg not found; downloading a static LGPL build (about 100 MB)..."
      fetch https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-linux64-lgpl.tar.xz "$tmp/ffmpeg.tar.xz"
      mkdir -p "$tmp/ff"
      tar -xJf "$tmp/ffmpeg.tar.xz" -C "$tmp/ff" --wildcards '*/bin/ffmpeg'
      install -m 755 "$(find "$tmp/ff" -name ffmpeg -type f | head -1)" "$bin/ffmpeg"
    fi
  fi
  echo "Tools:   $bin"

  # Remember a non-default tools folder so the plugin can find it.
  default_tools="${XDG_DATA_HOME:-$HOME/.local/share}/YtAudioBrowser"
  mkdir -p "$config_dir"
  if [ "$tools_dir" != "$default_tools" ]; then
    python3 - "$ini" "$tools_dir" <<'PY' 2>/dev/null || printf '[Paths]\nToolsDir=%s\n' "$tools_dir" >> "$ini"
import sys, configparser
p, v = sys.argv[1], sys.argv[2]
c = configparser.RawConfigParser(); c.optionxform = str
c.read(p, encoding="utf-8")
if not c.has_section("Paths"): c.add_section("Paths")
c.set("Paths", "ToolsDir", v)
with open(p, "w", encoding="utf-8") as f: c.write(f, space_around_delimiters=False)
PY
  fi
fi

# --- uninstaller ---------------------------------------------------------------
if [ "$do_plugin" = 1 ]; then
  mkdir -p "$tools_dir"
  cat > "$tools_dir/uninstall.sh" <<UNINST
#!/usr/bin/env bash
# Removes YT Audio Browser. Your downloaded audio and settings are kept.
rm -f "$plugin_dir/YtAudioBrowser.clap"
rm -rf "$tools_dir/bin"
rm -f "$tools_dir/uninstall.sh"
rmdir "$tools_dir" 2>/dev/null || true
echo "Removed. Settings are in $config_dir; downloads are in your Music/YtAudioBrowser folder."
UNINST
  chmod +x "$tools_dir/uninstall.sh"
  echo "Uninstall later with: $tools_dir/uninstall.sh"
fi

echo
echo "Done. Rescan plugins in your DAW."
[ "$do_plugin" = 1 ] && [ "$plugin_dir" != "$HOME/.clap" ] && \
  echo "Note: $plugin_dir is not a default CLAP folder. Add it to your DAW's CLAP search paths."
exit 0
