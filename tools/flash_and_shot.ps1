# Build SkyDisplay, flash it, wait for the first chart, and save a screenshot over USB serial.
# Usage:  pwsh tools/flash_and_shot.ps1 [-Board wt32|indicator] [-Port COM8] [-NoFlash]
param([ValidateSet("wt32", "indicator")][string]$Board = "wt32", [string]$Port = "", [switch]$NoFlash)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$fw   = Join-Path $root "firmware\SkyDisplay"
$cli  = "$env:LOCALAPPDATA\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
if ($Board -eq "indicator") {
  # Octal PSRAM selects the Indicator in board.h. Serial goes through the board's USB-serial chip.
  $fqbn = "esp32:esp32:esp32s3:CDCOnBoot=default,FlashSize=8M,FlashMode=qio,PartitionScheme=default_8MB,PSRAM=opi"
  $baud = 460800; $w = 480; $h = 480
  $build = Join-Path $env:TEMP "skydisplay-build-ind"
} else {
  $fqbn = "esp32:esp32:esp32s3:CDCOnBoot=cdc,USBMode=hwcdc,FlashSize=16M,FlashMode=qio,PartitionScheme=app3M_fat9M_16MB,PSRAM=enabled"
  $baud = 115200; $w = 480; $h = 320
  $build = Join-Path $env:TEMP "skydisplay-build"
}
if (-not $Port) { $Port = if ($Board -eq "indicator") { "COM9" } else { "COM8" } }

if (-not $NoFlash) {
  & $cli compile -b $fqbn --build-path $build $fw 2>&1 | Select-String -Pattern 'error|Sketch uses' | ForEach-Object { $_.Line }
  if ($LASTEXITCODE -ne 0) { throw "compile failed" }
  & $cli upload -p $Port -b $fqbn --input-dir $build $fw 2>&1 | Select-Object -Last 1
  if ($LASTEXITCODE -ne 0) { throw "upload failed" }
  Start-Sleep -Seconds 1
}

$p = New-Object System.IO.Ports.SerialPort $Port,$baud
$p.DtrEnable = $false; $p.RtsEnable = $false; $p.ReadBufferSize = 2097152
$p.Open()
$log = ""; $sw = [Diagnostics.Stopwatch]::StartNew()
if (-not $NoFlash) {
  while ($sw.Elapsed.TotalSeconds -lt 45) { Start-Sleep -Milliseconds 300; $log += $p.ReadExisting(); if ($log -match 'check failed|chart updated') { break } }
  Start-Sleep -Seconds 3
}
$log += $p.ReadExisting()
($log -split "`n" | Select-String 'starting|lcd|chart updated|check failed|FAILED|swipe|tap|touch|long press') | ForEach-Object { $_.Line }
$p.DiscardInBuffer(); $p.Write("s")
$need = $w * $h * 2 + 20
$ms = New-Object IO.MemoryStream; $tmp = New-Object byte[] 65536; $sw.Restart()
while ($sw.Elapsed.TotalSeconds -lt 30) {
  if ($p.BytesToRead -gt 0) { $n = $p.Read($tmp, 0, $tmp.Length); $ms.Write($tmp, 0, $n) } else { Start-Sleep -Milliseconds 50 }
  if ($ms.Length -gt $need) { Start-Sleep -Milliseconds 300; while ($p.BytesToRead -gt 0) { $n = $p.Read($tmp, 0, $tmp.Length); $ms.Write($tmp, 0, $n) }; break }
}
$p.Close()
$raw = Join-Path $env:TEMP "skydisplay-shot.raw"
[IO.File]::WriteAllBytes($raw, $ms.ToArray())
$png = Join-Path $root ("firmware\screenshot-{0}-{1}.png" -f $Board, (Get-Date -Format "yyyy-MM-dd"))
python -I (Join-Path $PSScriptRoot "shot_to_png.py") $raw $png
