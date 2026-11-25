# Copies the built plugin into the per-user CLAP folder (no admin needed) and
# fetches the tools if they are missing.
param(
    [string]$Plugin = (Join-Path $PSScriptRoot '..\build\Release\YtAudioBrowser.clap')
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path $Plugin)) { throw "Plugin not found: $Plugin (build it first, or pass -Plugin <path>)" }

$clapDir = Join-Path $env:LOCALAPPDATA 'Programs\Common\CLAP'
New-Item -ItemType Directory -Force -Path $clapDir | Out-Null
Copy-Item $Plugin $clapDir -Force
Write-Host "Plugin copied to $clapDir"

$tools = Join-Path $env:LOCALAPPDATA 'YtAudioBrowser\bin'
if (-not (Test-Path (Join-Path $tools 'yt-dlp.exe'))) {
    & (Join-Path $PSScriptRoot 'fetch-tools.ps1')
}
Write-Host "Done. Rescan plugins in your DAW."
