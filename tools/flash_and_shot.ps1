# Build SkyDisplay, flash it to the WT32-SC01 Plus, wait for the first chart, and save a screenshot.
# Usage:  pwsh tools/flash_and_shot.ps1 [-Port COM8] [-NoFlash]
param([string]$Port = "COM8", [switch]$NoFlash)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$fw   = Join-Path $root "firmware\SkyDisplay"
$cli  = "$env:LOCALAPPDATA\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
$fqbn = "esp32:esp32:esp32s3:CDCOnBoot=cdc,USBMode=hwcdc,FlashSize=16M,FlashMode=qio,PartitionScheme=app3M_fat9M_16MB,PSRAM=enabled"
$build = Join-Path $env:TEMP "skydisplay-build"

if (-not $NoFlash) {
  & $cli compile -b $fqbn --build-path $build $fw 2>&1 | Select-String -Pattern 'error|Sketch uses' | ForEach-Object { $_.Line }
  if ($LASTEXITCODE -ne 0) { throw "compile failed" }
  & $cli upload -p $Port -b $fqbn --input-dir $build $fw 2>&1 | Select-Object -Last 1
  if ($LASTEXITCODE -ne 0) { throw "upload failed" }
  Start-Sleep -Seconds 1
}

$p = New-Object System.IO.Ports.SerialPort $Port,115200
$p.DtrEnable = $false; $p.RtsEnable = $false; $p.ReadBufferSize = 1048576
$p.Open()
$log = ""; $sw = [Diagnostics.Stopwatch]::StartNew()
if (-not $NoFlash) {
  while ($sw.Elapsed.TotalSeconds -lt 45) { Start-Sleep -Milliseconds 300; $log += $p.ReadExisting(); if ($log -match 'check failed|chart updated') { break } }
  Start-Sleep -Seconds 3
}
$log += $p.ReadExisting()
($log -split "`n" | Select-String 'chart updated|check failed|FAILED|swipe|tap|touch|long press') | ForEach-Object { $_.Line }
$p.DiscardInBuffer(); $p.Write("s")
$ms = New-Object IO.MemoryStream; $tmp = New-Object byte[] 65536; $sw.Restart()
while ($sw.Elapsed.TotalSeconds -lt 20) {
  if ($p.BytesToRead -gt 0) { $n = $p.Read($tmp, 0, $tmp.Length); $ms.Write($tmp, 0, $n) } else { Start-Sleep -Milliseconds 50 }
  if ($ms.Length -gt 307300) { Start-Sleep -Milliseconds 300; while ($p.BytesToRead -gt 0) { $n = $p.Read($tmp, 0, $tmp.Length); $ms.Write($tmp, 0, $n) }; break }
}
$p.Close()
$raw = Join-Path $env:TEMP "skydisplay-shot.raw"
[IO.File]::WriteAllBytes($raw, $ms.ToArray())
$png = Join-Path $root ("firmware\screenshot-{0}.png" -f (Get-Date -Format "yyyy-MM-dd"))
python -I (Join-Path $PSScriptRoot "shot_to_png.py") $raw $png
