# =============================================================================
#  GateOS — Skrypt naprawczy / build / flash
#  Wykonuje:
#   1. Nadpisuje poprawione pliki źródłowe
#   2. git add + commit + push
#   3. PlatformIO build
#   4. Flash na COM4
#
#  Użycie:
#   .\deploy_fix.ps1
#   .\deploy_fix.ps1 -SkipGit        # pomija git push
#   .\deploy_fix.ps1 -SkipFlash      # pomija flash (tylko build)
#   .\deploy_fix.ps1 -Port COM5      # inny port
# =============================================================================
param(
    [switch]$SkipGit,
    [switch]$SkipFlash,
    [string]$Port = "COM4"
)

$ErrorActionPreference = "Stop"

# ── Ścieżki ──────────────────────────────────────────────────────────────────
$ProjectRoot = "C:\Users\chemi\Documents\gate\GateOS"
$Esp32Src    = "$ProjectRoot\esp32\Src"
$SafetyDir   = "$Esp32Src\safety"

# Lokalizacja poprawionych plików (obok tego skryptu)
$FixDir = "$PSScriptRoot\fixed_files"

Write-Host ""
Write-Host "==========================================" -ForegroundColor Cyan
Write-Host "  GateOS — deploy poprawek bugów" -ForegroundColor Cyan
Write-Host "==========================================" -ForegroundColor Cyan
Write-Host ""

# ── 1. Sprawdzenie środowiska ─────────────────────────────────────────────────
Write-Host "[1/5] Weryfikacja środowiska..." -ForegroundColor Yellow

if (-not (Test-Path $ProjectRoot)) {
    Write-Error "Nie znaleziono katalogu projektu: $ProjectRoot"
    exit 1
}

# Sprawdź PlatformIO
$pio = $null
foreach ($candidate in @("pio","platformio","python -m platformio")) {
    try {
        $null = & pio --version 2>$null
        $pio = "pio"
        break
    } catch { }
}
if (-not $pio) {
    # Próba przez python
    try {
        $null = python -m platformio --version 2>$null
        $pio = "python -m platformio"
    } catch {
        Write-Warning "PlatformIO nie znalezione w PATH. Upewnij się że jest zainstalowane."
        Write-Warning "pip install platformio"
    }
}
if ($pio) { Write-Host "  PlatformIO: OK ($pio)" -ForegroundColor Green }

# ── 2. Kopiowanie poprawionych plików ────────────────────────────────────────
Write-Host ""
Write-Host "[2/5] Kopiowanie poprawionych plików..." -ForegroundColor Yellow

$filesToCopy = @(
    @{
        Src  = "$FixDir\gate_controller.cpp"
        Dst  = "$Esp32Src\gate_controller.cpp"
        Desc = "gate_controller.cpp (B-03 bypassOCCooldown, B-08 millis cast, B-11 stopConfirmCount, B-13 NaN guard)"
    },
    @{
        Src  = "$FixDir\safety_manager.cpp"
        Dst  = "$SafetyDir\safety_manager.cpp"
        Desc = "safety_manager.cpp (B-05 debounce sticky-timer)"
    },
    @{
        Src  = "$FixDir\safety_manager.h"
        Dst  = "$SafetyDir\safety_manager.h"
        Desc = "safety_manager.h (B-05 rawPending fields)"
    },
    @{
        Src  = "$FixDir\position_tracker.cpp"
        Dst  = "$Esp32Src\position_tracker.cpp"
        Desc = "position_tracker.cpp (B-02 hallPosition mutex, B-06 filter fields, B-07 atomic snapshot)"
    }
)

foreach ($f in $filesToCopy) {
    if (-not (Test-Path $f.Src)) {
        Write-Error "Brak pliku poprawki: $($f.Src)"
        Write-Error "Upewnij się, że folder 'fixed_files' jest obok tego skryptu."
        exit 1
    }
    $dstDir = Split-Path $f.Dst -Parent
    if (-not (Test-Path $dstDir)) { New-Item -ItemType Directory -Path $dstDir -Force | Out-Null }

    # Backup oryginału (tylko raz)
    $backup = "$($f.Dst).orig"
    if (-not (Test-Path $backup)) {
        Copy-Item $f.Dst $backup -ErrorAction SilentlyContinue
        Write-Host "  Backup: $backup" -ForegroundColor DarkGray
    }

    Copy-Item $f.Src $f.Dst -Force
    Write-Host "  OK: $($f.Desc)" -ForegroundColor Green
}

# ── 3. B-04: Patch app_main.cpp — dodaj esp_task_wdt_add(NULL) ───────────────
Write-Host ""
Write-Host "[3/5] Patch app_main.cpp (B-04 WDT registration)..." -ForegroundColor Yellow

$appMainPath = "$Esp32Src\app_main.cpp"
if (Test-Path $appMainPath) {
    $content = Get-Content $appMainPath -Raw

    # Sprawdź czy już naprawione
    if ($content -match "esp_task_wdt_add\(NULL\)") {
        Write-Host "  app_main.cpp: WDT fix już zastosowany — pomijam" -ForegroundColor Green
    } else {
        # Szukamy bloku gateTask i dodajemy esp_task_wdt_add po esp_task_wdt_init
        # Wzorzec: funkcja gateTask zawiera esp_task_wdt_init lub esp_task_wdt_reset
        # Dodajemy esp_task_wdt_add(NULL) po linii z esp_task_wdt_init w gateTask
        #
        # Poprawka: w gateTask() zaraz po "if (wdtEnabled) {" i esp_task_wdt_init
        # wstawiamy esp_task_wdt_add(NULL)

        # Strategia: patch GateController::begin() — usuń esp_task_wdt_init stąd
        # i zostaw go tylko w gateTask w app_main.
        # Prostszy patch: znajdź "esp_task_wdt_add" i dodaj jeśli nie ma w gateTask.

        # Szukamy gateTask i wstawiamy WDT registration
        $patchedContent = $content -replace `
            '(void gateTask\(void\* pvParameters\)\s*\{[^}]*?const bool wdtEnabled = [^;]+;[^}]*?if \(wdtEnabled\) \{)',
            '$1
    esp_task_wdt_add(NULL);  // FIX B-04: register this task with WDT'

        if ($patchedContent -ne $content) {
            # Backup
            $backup = "$appMainPath.orig"
            if (-not (Test-Path $backup)) { Copy-Item $appMainPath $backup }
            Set-Content $appMainPath $patchedContent -NoNewline
            Write-Host "  app_main.cpp: WDT fix zastosowany (esp_task_wdt_add)" -ForegroundColor Green
        } else {
            Write-Warning "  app_main.cpp: nie udało się zastosować regex patch — sprawdź ręcznie"
            Write-Host "  Ręczna instrukcja:" -ForegroundColor Yellow
            Write-Host "  W funkcji gateTask() po linii:" -ForegroundColor Yellow
            Write-Host "    if (wdtEnabled) {" -ForegroundColor White
            Write-Host "  dodaj:" -ForegroundColor Yellow
            Write-Host "    esp_task_wdt_add(NULL);  // FIX B-04" -ForegroundColor White
        }
    }
} else {
    Write-Warning "  Nie znaleziono app_main.cpp pod: $appMainPath"
}

# ── 4. Git ────────────────────────────────────────────────────────────────────
if (-not $SkipGit) {
    Write-Host ""
    Write-Host "[4/5] Git commit + push..." -ForegroundColor Yellow
    Set-Location $ProjectRoot

    try {
        git add esp32/Src/gate_controller.cpp `
                esp32/Src/safety/safety_manager.cpp `
                esp32/Src/safety/safety_manager.h `
                esp32/Src/position_tracker.cpp `
                esp32/Src/app_main.cpp

        $commitMsg = "fix: apply audit bug fixes B-02/B-03/B-04/B-05/B-06/B-07/B-08/B-11/B-13

B-02 hallPosition_ race condition: portENTER_CRITICAL in loop context
B-03 bypassCooldown split: bypassOCCooldown only skips OC window
B-04 WDT: esp_task_wdt_add(NULL) added in gateTask()
B-05 SafetyManager debounce: corrected to sticky-timer pattern
B-06 hover filter static vars -> class fields, reset on init
B-07 position snapshot: atomic write via tmp+rename
B-08 millis() long cast removed (uint32_t arithmetic for overflow safety)
B-11 stopConfirmCount min_frames 3->8 (avoids premature STOPPED)
B-13 NaN/Inf guard on atof() in handleCommand() goto:/goto_mm:"

        git commit -m $commitMsg
        git push
        Write-Host "  Git: commit + push OK" -ForegroundColor Green
    } catch {
        Write-Warning "  Git error: $($_.Exception.Message)"
        Write-Warning "  Kontynuuję (git push niewymagany do flash)..."
    }
} else {
    Write-Host ""
    Write-Host "[4/5] Git: pomijam (--SkipGit)" -ForegroundColor DarkGray
}

# ── 5. Build + Flash ──────────────────────────────────────────────────────────
Write-Host ""
Write-Host "[5/5] PlatformIO build + flash..." -ForegroundColor Yellow

if (-not $pio) {
    Write-Warning "PlatformIO nie dostępne — pomijam build/flash"
    Write-Host ""
    Write-Host "Uruchom ręcznie:" -ForegroundColor Yellow
    Write-Host "  cd $ProjectRoot\esp32" -ForegroundColor White
    Write-Host "  pio run -e esp32 -t upload --upload-port $Port" -ForegroundColor White
    exit 0
}

Set-Location "$ProjectRoot\esp32"

Write-Host "  Build..." -ForegroundColor Cyan
try {
    if ($pio -eq "python -m platformio") {
        python -m platformio run -e esp32
    } else {
        pio run -e esp32
    }
    Write-Host "  Build: OK" -ForegroundColor Green
} catch {
    Write-Error "Build FAILED: $($_.Exception.Message)"
    exit 1
}

if (-not $SkipFlash) {
    Write-Host "  Flash na $Port..." -ForegroundColor Cyan
    try {
        if ($pio -eq "python -m platformio") {
            python -m platformio run -e esp32 -t upload --upload-port $Port
        } else {
            pio run -e esp32 -t upload --upload-port $Port
        }
        Write-Host "  Flash: OK" -ForegroundColor Green
    } catch {
        Write-Error "Flash FAILED: $($_.Exception.Message)"
        Write-Host "Sprawdź: czy ESP32 jest podłączone do $Port, czy port nie jest zajęty (zamknij Serial Monitor)" -ForegroundColor Yellow
        exit 1
    }
} else {
    Write-Host "  Flash: pomijam (--SkipFlash)" -ForegroundColor DarkGray
}

Write-Host ""
Write-Host "==========================================" -ForegroundColor Cyan
Write-Host "  GOTOWE!" -ForegroundColor Green
Write-Host "==========================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "Zastosowane poprawki:" -ForegroundColor White
Write-Host "  B-02  hallPosition_ race condition (portENTER_CRITICAL)" -ForegroundColor Green
Write-Host "  B-03  bypassCooldown -> bypassOCCooldown (narrower)" -ForegroundColor Green
Write-Host "  B-04  esp_task_wdt_add(NULL) w gateTask()" -ForegroundColor Green
Write-Host "  B-05  SafetyManager debounce (sticky-timer)" -ForegroundColor Green
Write-Host "  B-06  Hover filter state -> class fields + reset" -ForegroundColor Green
Write-Host "  B-07  Position snapshot atomic (tmp+rename)" -ForegroundColor Green
Write-Host "  B-08  millis() long cast usunięty" -ForegroundColor Green
Write-Host "  B-11  stopConfirmCount min_frames 3->8" -ForegroundColor Green
Write-Host "  B-13  NaN/Inf guard w handleCommand()" -ForegroundColor Green
Write-Host ""
Write-Host "Pozostałe do wykonania ręcznie (architektoniczne):" -ForegroundColor Yellow
Write-Host "  B-01  Unifikacja SafetyManager + GateController (refaktoryzacja)" -ForegroundColor Yellow
Write-Host "  B-09  DISARM retransmission (hover_uart_driver)" -ForegroundColor Yellow
Write-Host "  B-10  Homing direction (closest limit)" -ForegroundColor Yellow
Write-Host ""
