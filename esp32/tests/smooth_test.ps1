# GateOS Soft-Start / Soft-Stop Smoothness Test
# Tests different motion profile parameters and measures smoothness
$BaseUrl = "http://192.168.1.44:8080"
$LogFile = "smooth_test_$(Get-Date -Format 'yyyyMMdd_HHmmss').log"

function Log($msg, $level = "INFO") {
    $ts = Get-Date -Format "HH:mm:ss.fff"
    $line = "[$ts] [$level] $msg"
    Write-Host $line -ForegroundColor $(switch($level) { "PASS" {"Green"} "FAIL" {"Red"} "WARN" {"Yellow"} "STEP" {"Cyan"} "DATA" {"DarkGray"} default {"White"} })
    Add-Content -Path $LogFile -Value $line
}

function Api-Get($path) {
    try {
        $r = Invoke-WebRequest -Uri "$BaseUrl$path" -TimeoutSec 8 -UseBasicParsing
        return ($r.Content | ConvertFrom-Json)
    } catch { Log "API GET $path FAILED: $_" "FAIL"; return $null }
}

function Api-Post($path, $body = $null) {
    try {
        $params = @{ Uri = "$BaseUrl$path"; Method = "POST"; TimeoutSec = 10; UseBasicParsing = $true }
        if ($body) { $params.Body = ($body | ConvertTo-Json -Depth 5); $params.ContentType = "application/json" }
        $r = Invoke-WebRequest @params
        return ($r.Content | ConvertFrom-Json)
    } catch { Log "API POST $path FAILED: $_" "FAIL"; return $null }
}

function Gate-Status { return Api-Get "/api/status-lite" }
function Gate-Control($action) { Log ">>> CONTROL: $action" "STEP"; return Api-Post "/api/control" @{ action = $action } }

function Set-MotionConfig($rampOpenDist, $rampCloseDist, $brakeDistOpen, $brakeDistClose, $brakeForce, $maxSpeedOpen, $maxSpeedClose, $minSpeed, $rampStep) {
    $body = @{
        motion = @{
            advanced = @{
                maxSpeedOpen = $maxSpeedOpen
                maxSpeedClose = $maxSpeedClose
                minSpeed = $minSpeed
                rampOpen = @{ mode = "distance"; value = $rampOpenDist }
                rampClose = @{ mode = "distance"; value = $rampCloseDist }
                braking = @{
                    startDistanceOpen = $brakeDistOpen
                    startDistanceClose = $brakeDistClose
                    force = $brakeForce
                    mode = "active"
                }
            }
        }
        hoverUart = @{ rampStep = $rampStep }
    }
    $r = Api-Post "/api/config" $body
    if ($r -and $r.status -eq "ok") {
        Log "  Config applied: rampOpen=${rampOpenDist}m rampClose=${rampCloseDist}m brakeOpen=${brakeDistOpen}m brakeClose=${brakeDistClose}m force=$brakeForce maxOpen=$maxSpeedOpen maxClose=$maxSpeedClose min=$minSpeed step=$rampStep" "PASS"
        Start-Sleep -Seconds 2
        return $true
    }
    Log "  Config apply FAILED" "FAIL"
    return $false
}

function Monitor-Movement($label, $maxSec = 60, $pollMs = 250) {
    # Collect RPM and current samples during movement
    $samples = @()
    $start = Get-Date
    $wasMoving = $false
    $moveStart = $null
    $moveEnd = $null

    while (((Get-Date) - $start).TotalSeconds -lt $maxSec) {
        $s = Gate-Status
        if (-not $s) { Start-Sleep -Milliseconds $pollMs; continue }

        $sample = @{
            t = ((Get-Date) - $start).TotalMilliseconds
            rpm = [int]$s.rpm
            iA = [float]$s.iA
            pos = [int]$s.positionMm
            state = $s.state
            moving = [bool]$s.moving
        }
        $samples += $sample

        if ($s.moving -and -not $wasMoving) {
            $wasMoving = $true
            $moveStart = $sample.t
        }
        if (-not $s.moving -and $wasMoving) {
            $moveEnd = $sample.t
            # Collect 2 more samples after stop
            Start-Sleep -Milliseconds ($pollMs * 2)
            $s2 = Gate-Status
            if ($s2) {
                $samples += @{ t = ((Get-Date) - $start).TotalMilliseconds; rpm = [int]$s2.rpm; iA = [float]$s2.iA; pos = [int]$s2.positionMm; state = $s2.state; moving = [bool]$s2.moving }
            }
            break
        }
        Start-Sleep -Milliseconds $pollMs
    }

    if ($samples.Count -lt 3) {
        Log "  [$label] Not enough samples ($($samples.Count))" "WARN"
        return $null
    }

    # Analyze smoothness metrics
    $movingSamples = $samples | Where-Object { $_.moving -eq $true }
    if ($movingSamples.Count -lt 2) {
        Log "  [$label] Not enough moving samples" "WARN"
        return $null
    }

    $maxRpm = ($movingSamples | Measure-Object -Property rpm -Maximum).Maximum
    $maxIA = ($movingSamples | Measure-Object -Property iA -Maximum).Maximum
    $avgIA = ($movingSamples | Measure-Object -Property iA -Average).Average

    # Compute RPM jerk (max change between consecutive samples)
    $maxRpmJerk = 0
    $maxIAJerk = 0
    $rpmJerks = @()
    $iaJerks = @()
    for ($j = 1; $j -lt $movingSamples.Count; $j++) {
        $dRpm = [math]::Abs($movingSamples[$j].rpm - $movingSamples[$j-1].rpm)
        $dIA = [math]::Abs($movingSamples[$j].iA - $movingSamples[$j-1].iA)
        $rpmJerks += $dRpm
        $iaJerks += $dIA
        if ($dRpm -gt $maxRpmJerk) { $maxRpmJerk = $dRpm }
        if ($dIA -gt $maxIAJerk) { $maxIAJerk = $dIA }
    }
    $avgRpmJerk = ($rpmJerks | Measure-Object -Average).Average
    $avgIAJerk = ($iaJerks | Measure-Object -Average).Average

    # Compute acceleration time (samples until RPM reaches 80% of max)
    $rpmThresh = [int]($maxRpm * 0.8)
    $accelSamples = 0
    foreach ($ms in $movingSamples) {
        $accelSamples++
        if ([math]::Abs($ms.rpm) -ge $rpmThresh) { break }
    }
    $accelTimeMs = if ($accelSamples -gt 1 -and $moveStart) {
        $movingSamples[$accelSamples - 1].t - $movingSamples[0].t
    } else { 0 }

    # Duration
    $durationMs = if ($moveEnd -and $moveStart) { $moveEnd - $moveStart } else { 0 }

    # Smoothness score (lower = smoother): weighted sum of jerk metrics
    $smoothScore = [math]::Round($avgRpmJerk * 0.4 + $avgIAJerk * 100 * 0.3 + $maxIAJerk * 100 * 0.3, 1)

    $result = @{
        label = $label
        samples = $samples.Count
        movingSamples = $movingSamples.Count
        durationMs = [int]$durationMs
        maxRpm = $maxRpm
        maxIA = [math]::Round($maxIA, 2)
        avgIA = [math]::Round($avgIA, 2)
        maxRpmJerk = $maxRpmJerk
        avgRpmJerk = [math]::Round($avgRpmJerk, 1)
        maxIAJerk = [math]::Round($maxIAJerk, 2)
        avgIAJerk = [math]::Round($avgIAJerk, 3)
        accelTimeMs = [int]$accelTimeMs
        smoothScore = $smoothScore
    }

    Log "  [$label] dur=${durationMs}ms maxRpm=$maxRpm maxIA=$($result.maxIA)A avgIA=$($result.avgIA)A" "DATA"
    Log "  [$label] rpmJerk: max=$maxRpmJerk avg=$($result.avgRpmJerk) | iaJerk: max=$($result.maxIAJerk) avg=$($result.avgIAJerk)" "DATA"
    Log "  [$label] accelTime=${accelTimeMs}ms smoothScore=$smoothScore" "DATA"

    # Log all samples for detailed analysis
    foreach ($sm in $samples) {
        Log "  [$label] t=$([int]$sm.t) rpm=$($sm.rpm) iA=$($sm.iA) pos=$($sm.pos) $($sm.state)" "DATA"
    }

    return $result
}

function Run-TestProfile($name, $rampOpenDist, $rampCloseDist, $brakeDistOpen, $brakeDistClose, $brakeForce, $maxSpeedOpen, $maxSpeedClose, $minSpeed, $rampStep) {
    Log "---------- Profile: $name ----------" "STEP"

    Set-MotionConfig $rampOpenDist $rampCloseDist $brakeDistOpen $brakeDistClose $brakeForce $maxSpeedOpen $maxSpeedClose $minSpeed $rampStep

    # Ensure at OPEN limit first
    $s = Gate-Status
    if (-not $s.limitOpen) {
        Gate-Control "open" | Out-Null
        $timeout = 60; $sw = [System.Diagnostics.Stopwatch]::StartNew()
        while ($sw.Elapsed.TotalSeconds -lt $timeout) {
            $s = Gate-Status
            if ($s -and $s.limitOpen -and -not $s.moving) { break }
            Start-Sleep -Milliseconds 500
        }
    }

    # Test CLOSE movement (start from OPEN)
    Log "  Testing CLOSE movement..." "STEP"
    Gate-Control "close" | Out-Null
    $closeResult = Monitor-Movement "${name}_CLOSE" 80 250

    # Wait for full stop
    $timeout = 80; $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.Elapsed.TotalSeconds -lt $timeout) {
        $s = Gate-Status
        if ($s -and -not $s.moving) { break }
        Start-Sleep -Milliseconds 400
    }
    Start-Sleep -Seconds 2

    # Test OPEN movement (start from CLOSE/current)
    Log "  Testing OPEN movement..." "STEP"
    Gate-Control "open" | Out-Null
    $openResult = Monitor-Movement "${name}_OPEN" 80 250

    # Wait for full stop
    $timeout = 80; $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.Elapsed.TotalSeconds -lt $timeout) {
        $s = Gate-Status
        if ($s -and -not $s.moving) { break }
        Start-Sleep -Milliseconds 400
    }
    Start-Sleep -Seconds 2

    return @{ name = $name; close = $closeResult; open = $openResult }
}

# ============================================================
# MAIN
# ============================================================

Log "======================================================" "STEP"
Log "  GateOS Smoothness Test" "STEP"
Log "  Target: $BaseUrl" "STEP"
Log "  Log: $LogFile" "STEP"
Log "======================================================" "STEP"

# Read current config
$cfg = Api-Get "/api/config"
if (-not $cfg) { Log "Cannot read config" "FAIL"; exit 1 }
Log "Current motor: softStart=$($cfg.motor.softStartMs) softStop=$($cfg.motor.softStopMs) rampCurve=$($cfg.motor.rampCurve)"
Log "Current hover: rampStep=$($cfg.hoverUart.rampStep) maxSpeed=$($cfg.hoverUart.maxSpeed)"
Log "Current motion: maxSpeedOpen=$($cfg.motion.advanced.maxSpeedOpen) maxSpeedClose=$($cfg.motion.advanced.maxSpeedClose) minSpeed=$($cfg.motion.advanced.minSpeed)"
Log "Current rampOpen=$($cfg.motion.advanced.rampOpen.value)m rampClose=$($cfg.motion.advanced.rampClose.value)m"
Log "Current brakeOpen=$($cfg.motion.advanced.braking.startDistanceOpen)m brakeClose=$($cfg.motion.advanced.braking.startDistanceClose)m force=$($cfg.motion.advanced.braking.force)"

$results = @()

# ---- Profile 1: Current (baseline) ----
$results += Run-TestProfile "P1_BASELINE" 0.292 0.331 0.806 0.884 60 200 200 40 8

# ---- Profile 2: Smoother accel (longer ramp distance) ----
$results += Run-TestProfile "P2_SMOOTH_ACCEL" 0.6 0.6 0.806 0.884 60 200 200 35 6

# ---- Profile 3: Smoother decel (longer brake dist, lower force) ----
$results += Run-TestProfile "P3_SMOOTH_DECEL" 0.292 0.331 1.2 1.2 40 200 200 35 8

# ---- Profile 4: Full smooth (longer accel + decel + lower rampStep) ----
$results += Run-TestProfile "P4_FULL_SMOOTH" 0.8 0.8 1.2 1.2 40 180 180 30 4

# ---- Profile 5: Balanced (moderate everything) ----
$results += Run-TestProfile "P5_BALANCED" 0.5 0.5 1.0 1.0 50 200 200 35 6

# ---- Profile 6: Ultra smooth (very gentle) ----
$results += Run-TestProfile "P6_ULTRA_SMOOTH" 1.0 1.0 1.5 1.5 35 160 160 25 3

# ============================================================
# COMPARE RESULTS
# ============================================================

Log "" "STEP"
Log "======================================================" "STEP"
Log "  RESULTS COMPARISON" "STEP"
Log "======================================================" "STEP"
Log "" "STEP"
Log ("  {0,-20} {1,8} {2,8} {3,8} {4,8} {5,8} {6,10}" -f "Profile", "OpenScr", "ClsScr", "AvgScr", "MaxIA", "AvgJrk", "Duration") "STEP"
Log ("  " + ("-" * 82)) "STEP"

$bestScore = 99999
$bestProfile = ""

foreach ($r in $results) {
    $openScore = if ($r.open) { $r.open.smoothScore } else { 999 }
    $closeScore = if ($r.close) { $r.close.smoothScore } else { 999 }
    $avgScore = [math]::Round(($openScore + $closeScore) / 2, 1)
    $maxIA = if ($r.open -and $r.close) { [math]::Max($r.open.maxIA, $r.close.maxIA) } else { 0 }
    $avgJrk = if ($r.open -and $r.close) { [math]::Round(($r.open.avgRpmJerk + $r.close.avgRpmJerk) / 2, 1) } else { 0 }
    $durTotal = if ($r.open -and $r.close) { $r.open.durationMs + $r.close.durationMs } else { 0 }

    Log ("  {0,-20} {1,8:F1} {2,8:F1} {3,8:F1} {4,7:F2}A {5,8:F1} {6,8}ms" -f $r.name, $openScore, $closeScore, $avgScore, $maxIA, $avgJrk, $durTotal) "INFO"

    if ($avgScore -lt $bestScore) {
        $bestScore = $avgScore
        $bestProfile = $r.name
    }
}

Log "" "STEP"
Log "  BEST PROFILE: $bestProfile (smoothScore=$bestScore)" "PASS"
Log "" "STEP"

# Find the best profile's parameters and apply them
$bestParams = switch ($bestProfile) {
    "P1_BASELINE"     { @(0.292, 0.331, 0.806, 0.884, 60, 200, 200, 40, 8) }
    "P2_SMOOTH_ACCEL" { @(0.6, 0.6, 0.806, 0.884, 60, 200, 200, 35, 6) }
    "P3_SMOOTH_DECEL" { @(0.292, 0.331, 1.2, 1.2, 40, 200, 200, 35, 8) }
    "P4_FULL_SMOOTH"  { @(0.8, 0.8, 1.2, 1.2, 40, 180, 180, 30, 4) }
    "P5_BALANCED"     { @(0.5, 0.5, 1.0, 1.0, 50, 200, 200, 35, 6) }
    "P6_ULTRA_SMOOTH" { @(1.0, 1.0, 1.5, 1.5, 35, 160, 160, 25, 3) }
    default           { @(0.5, 0.5, 1.0, 1.0, 50, 200, 200, 35, 6) }
}

Log "  Applying best profile to config..." "STEP"
Set-MotionConfig $bestParams[0] $bestParams[1] $bestParams[2] $bestParams[3] $bestParams[4] $bestParams[5] $bestParams[6] $bestParams[7] $bestParams[8]

Log "======================================================" "STEP"
Log "  SMOOTHNESS TEST COMPLETE" "STEP"
Log "  Best: $bestProfile | Score: $bestScore" "STEP"
Log "  Log: $LogFile" "STEP"
Log "======================================================" "STEP"
