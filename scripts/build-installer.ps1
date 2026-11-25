# Builds the plugin and produces dist\YtAudioBrowser-Setup.exe (or -Slim.exe) with NSIS.
# Needs CMake, Visual Studio C++ tools and NSIS (winget install NSIS.NSIS).
#   -Slim  small installer that downloads yt-dlp/ffmpeg/deno when it runs
param([switch]$Slim)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

function Run([string]$exe, [string[]]$arguments) {
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) { throw "$exe failed with exit code $LASTEXITCODE" }
}

Run 'cmake' @('-S', $root, '-B', "$root\build", '-A', 'x64')
Run 'cmake' @('--build', "$root\build", '--config', 'Release')
$plugin = "$root\build\Release\YtAudioBrowser.clap"
if (-not (Test-Path $plugin)) { throw "Plugin was not built: $plugin not found" }

$makensis = (Get-Command makensis -ErrorAction SilentlyContinue).Source
if (-not $makensis) { $makensis = "${env:ProgramFiles(x86)}\NSIS\makensis.exe" }
if (-not (Test-Path $makensis)) { throw "makensis not found. Install NSIS: winget install NSIS.NSIS" }

$name = if ($Slim) { 'YtAudioBrowser-Setup-Slim.exe' } else { 'YtAudioBrowser-Setup.exe' }
$out = "$root\dist\$name"
New-Item -ItemType Directory -Force -Path "$root\dist" | Out-Null
Remove-Item $out -ErrorAction SilentlyContinue

# Defines go in a file so paths with spaces cannot be mangled on the command line.
$defs = @("!define PLUGIN `"$plugin`"", "!define OUTFILE `"$out`"")
if ($Slim) {
    $defs += '!define SLIM 1'
    $defs += "!define FETCH `"$PSScriptRoot\fetch-tools.ps1`""
} else {
    $tools = "$root\build\installer-tools"
    & "$PSScriptRoot\fetch-tools.ps1" -Dest $tools
    $defs += "!define TOOLS `"$tools`""
}
Set-Content -Path "$root\build\installer-defs.nsh" -Value $defs -Encoding UTF8

Run $makensis @("$root\installer\installer.nsi")
if (-not (Test-Path $out)) { throw "makensis finished but $out was not created" }
Write-Host "Built $out"
