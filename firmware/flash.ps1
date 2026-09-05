param(
    [string]$Port = "COM3",
    [int]$Baud = 460800
)

$ErrorActionPreference = "Stop"
$python = "C:\Users\Adrian\.espressif\python_env\idf5.5_py3.14_env\Scripts\python.exe"
$buildDir = Join-Path $PSScriptRoot "build"

if (-not (Test-Path (Join-Path $buildDir "flash_args"))) {
    throw "Firmware has not been built. Run idf.py -C firmware build first."
}

Push-Location $buildDir
try {
    & $python -m esptool --chip esp32c6 -p $Port -b $Baud `
        --before usb_reset --after hard_reset write_flash '@flash_args'
    if ($LASTEXITCODE -ne 0) {
        throw "esptool failed with exit code $LASTEXITCODE"
    }
} finally {
    Pop-Location
}
