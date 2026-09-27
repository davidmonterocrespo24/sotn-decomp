# SOTN ESP32-S3 - flash the firmware.
#
#   flash.ps1 COM6              -> Waveshare board: app + data partition
#   flash.ps1 COM6 -NoData      -> Waveshare board: app only
#   flash.ps1 COM9 -Xiao        -> handheld: app only, there IS no data
#                                  partition (its assets live on the microSD)
param([string]$Port = "COM6", [switch]$NoData, [switch]$Xiao)

$PY = "C:\Espressif\python_env\idf4.4_py3.10_env\Scripts\python.exe"
$ESPTOOL = "C:\Espressif5.5\frameworks\esp-idf-v5.5.4\components\esptool_py\esptool\esptool.py"

if ($Xiao) {
    $B = "$PSScriptRoot\build-xiao"
    # partitions_xiao.csv has no `storage` entry: 8 MB of flash cannot hold
    # both a 3 MB app and the 3.7 MB of disc assets, so the assets go on the
    # card. Passing -NoData here would be redundant; it is forced.
    $NoData = $true
    Write-Host "== flashing the XIAO handheld (no data partition) =="
} else {
    $B = "$PSScriptRoot\build"
    Write-Host "== flashing the Waveshare board =="
}

if (-not (Test-Path "$B\sotn_esp32s3.bin")) {
    throw "no binary in $B - build first with build.ps1$(if ($Xiao) {' xiao'})"
}

$parts = @(
    "0x0", "$B\bootloader\bootloader.bin",
    "0x8000", "$B\partition_table\partition-table.bin",
    "0x10000", "$B\sotn_esp32s3.bin"
)
if (-not $NoData) {
    $parts += @("0x610000", "$B\sotn_data.bin")
}

& $PY $ESPTOOL --chip esp32s3 --port $Port --baud 921600 write_flash @parts
