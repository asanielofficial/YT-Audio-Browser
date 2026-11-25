# Downloads yt-dlp, ffmpeg and deno into the folder the plugin searches first.
# Re-run any time to refresh them. The plugin's "Update yt-dlp" button also
# updates yt-dlp.exe in place.
param(
    [string]$Dest = (Join-Path $env:LOCALAPPDATA 'YtAudioBrowser\bin')
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'   # Invoke-WebRequest is very slow with the bar on
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

New-Item -ItemType Directory -Force -Path $Dest | Out-Null
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("ytb-" + [Guid]::NewGuid())
New-Item -ItemType Directory -Force -Path $tmp | Out-Null

try {
    Write-Host "yt-dlp.exe ..."
    Invoke-WebRequest 'https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe' `
        -OutFile (Join-Path $Dest 'yt-dlp.exe')

    Write-Host "ffmpeg (LGPL build) ..."
    $ff = Join-Path $tmp 'ffmpeg.zip'
    Invoke-WebRequest 'https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-lgpl.zip' -OutFile $ff
    Expand-Archive $ff -DestinationPath (Join-Path $tmp 'ff') -Force
    Get-ChildItem (Join-Path $tmp 'ff') -Recurse -Include ffmpeg.exe, ffprobe.exe |
        ForEach-Object { Copy-Item $_.FullName $Dest -Force }

    # YouTube needs a JavaScript runtime for yt-dlp. Deno is the only one yt-dlp
    # enables by default; the plugin passes its path explicitly.
    Write-Host "deno ..."
    $dz = Join-Path $tmp 'deno.zip'
    Invoke-WebRequest 'https://github.com/denoland/deno/releases/latest/download/deno-x86_64-pc-windows-msvc.zip' -OutFile $dz
    Expand-Archive $dz -DestinationPath $Dest -Force
}
finally {
    Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host ""
Write-Host "Installed to $Dest"
Get-ChildItem $Dest -Filter *.exe | ForEach-Object { Write-Host ("  " + $_.Name) }
