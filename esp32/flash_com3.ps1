# flash_com3.ps1
param(
  [string]$Port = "COM3",
  [string]$Env = ""
)

$ErrorActionPreference = "Stop"

function Run($cmd) {
  Write-Host ">> $cmd"
  iex $cmd
}

# 0) sanity
if (-not (Get-Command pio -ErrorAction SilentlyContinue)) {
  throw "Nie widzę 'pio' w PATH. Odpal z PlatformIO/VSCode terminala albo doinstaluj platformio."
}

# 1) ERASING
Write-Host "`n=== 1) ERASE FLASH ($Port) ==="
$eraseOk = $false
try {
  $envArg = ""
  if ($Env -ne "") { $envArg = "-e $Env" }
  Run "pio run $envArg -t erase --upload-port $Port"
  $eraseOk = $true
} catch {
  Write-Host "[WARN] pio -t erase nie zadziałało, próbuję esptool..."
}

if (-not $eraseOk) {
  # Fallback: esptool (wymaga python + esptool w środowisku)
  # Najczęściej działa, jeśli masz python w PATH albo w venv
  try {
    Run "python -m esptool --port $Port erase_flash"
    $eraseOk = $true
  } catch {
    throw "Nie udało się wyczyścić flash: ani 'pio -t erase', ani 'python -m esptool erase_flash'."
  }
}

# 2) UPLOAD firmware
Write-Host "`n=== 2) UPLOAD FIRMWARE ($Port) ==="
$envArg2 = ""
if ($Env -ne "") { $envArg2 = "-e $Env" }
Run "pio run $envArg2 -t upload --upload-port $Port"

# 3) UPLOAD LittleFS
Write-Host "`n=== 3) UPLOAD LittleFS ($Port) ==="
Run "pio run $envArg2 -t uploadfs --upload-port $Port"

Write-Host "`nDONE ✅"
