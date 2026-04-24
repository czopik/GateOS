# GateOS Comprehensive Gate Test Script
# Usage: .\gate_test.ps1
# Requires: PowerShell 5+, gate at http://192.168.1.44:8080

$GateIP = "192.168.1.44"
$GatePort = 8080
$BaseUrl = "http://${GateIP}:${GatePort}"
$LogFile = "gate_test_$(Get-Date -Format 'yyyyMMdd_HHmmss').log"

function Log($msg, $level = "INFO") {
    $ts = Get-Date -Format "HH:mm:ss.fff"
    $line = "[$ts] [$level] $msg"
    Write-Host $line -ForegroundColor $(switch($level) { "PASS" {"Green"} "FAIL" {"Red"} "WARN" {"Yellow"} "STEP" {"Cyan"} default {"White"} })
    Add-Content -Path $LogFile -Value $line
}

function Api-Get($path) {
    try {
        $r = Invoke-WebRequest -Uri "$BaseUrl$path" -TimeoutSec 8 -UseBasicParsing
        return ($r.Content | ConvertFrom-Json)
    } catch {
        Log "API GET $path FAILED: $_" "FAIL"
        return $null
    }
}

function Api-Post($path, $body = $null) {
    try {
        $params = @{ Uri = "$BaseUrl$path"; Method = "POST"; TimeoutSec = 10; UseBasicParsing = $true }
        if ($body) {
            $params.Body = ($body | ConvertTo-Json -Depth 5)
            $params.ContentType = "application/json"
        }
        $r = Invoke-WebRequest @params
        return ($r.Content | ConvertFrom-Json)
    } catch {
        Log "API POST $path FAILED: $_" "FAIL"
        return $null
    }
}

function Gate-Status {
    return Api-Get "/api/status-lite"
}

function Gate-Full {
    return Api-Get "/api/status"
}

function Gate-Diag {
    return Api-Get "/api/diagnostics"
}

function Gate-Control($action) {
    Log ">>> CONTROL: $action" "STEP"
    return Api-Post "/api/control" @{ action = $action }
}

function Gate-SetMaxDistance($meters) {
    Log ">>> SET maxDistance = ${meters}m" "STEP"
    return Api-Post "/api/gate/calibrate" @{ set = "max_distance"; value = $meters }
}

function Gate-WaitState($targetStates, $timeoutSec = 120, $pollMs = 500) {
    $start = Get-Date
    $lastLog = Get-Date
    while (((Get-Date) - $start).TotalSeconds -lt $timeoutSec) {
        $s = Gate-Status
        if (-not $s) { Start-Sleep -Milliseconds $pollMs; continue }
        $state = $s.state
        $pos = $s.positionMm
        $pct = $s.positionPercent
        $lo = $s.limitOpen
        $lc = $s.limitClose

        if (((Get-Date) - $lastLog).TotalMilliseconds -ge 800) {
            Log "  state=$state pos=${pos}mm pct=${pct}% limitOpen=$lo limitClose=$lc rpm=$($s.rpm) iA=$($s.iA)"
            $lastLog = Get-Date
        }

        if ($targetStates -contains $state -and -not $s.moving) {
            return $s
        }
        Start-Sleep -Milliseconds $pollMs
    }
    Log "TIMEOUT waiting for state in [$($targetStates -join ', ')]" "FAIL"
    return $null
}

function Gate-WaitMovingThenStopped($timeoutSec = 120, $pollMs = 400) {
    # First wait until gate starts moving
    $start = Get-Date
    while (((Get-Date) - $start).TotalSeconds -lt 5) {
        $s = Gate-Status
        if ($s -and $s.moving) { break }
        Start-Sleep -Milliseconds 200
    }
    # Then wait until it stops
    return Gate-WaitState @("stopped") $timeoutSec $pollMs
}

function Assert($condition, $passMsg, $failMsg) {
    if ($condition) {
        Log "  ASSERT PASS: $passMsg" "PASS"
        return $true
    } else {
        Log "  ASSERT FAIL: $failMsg" "FAIL"
        return $false
    }
}

# ============================================================
# TEST SUITE
# ============================================================

function Test-0-Connectivity {
    Log "========== TEST 0: Connectivity ==========" "STEP"
    $s = Gate-Full
    Assert ($null -ne $s) "API reachable" "API unreachable" | Out-Null
    Assert ($s.wifi.connected -eq $true) "WiFi connected ($($s.wifi.ssid))" "WiFi disconnected" | Out-Null
    Assert ($s.gate.errorCode -eq 0) "No gate errors" "Gate error code=$($s.gate.errorCode)" | Out-Null
    Log "  uptime=$($s.uptimeMs)ms heap=$($s.runtime.freeHeap) mqtt=$($s.mqtt.connected)"
    Log "  position=$($s.gate.position)m maxDistance=$($s.gate.maxDistance)m limitOpen=$($s.inputs.limitOpen) limitClose=$($s.inputs.limitClose)"
    $d = Gate-Diag
    if ($d) {
        Log "  hoverDist_raw=$($d.hoverUart.dist_mm_raw)mm dist_adj=$($d.hoverUart.dist_mm)mm hbOriginDistMm=$($d.position.hbOriginDistMm)"
    }
    return $s
}

function Test-1-MeasureDistance {
    Log "========== TEST 1: Measure exact CLOSE-to-OPEN distance ==========" "STEP"

    # Ensure gate is at CLOSE limit
    $s = Gate-Status
    if (-not $s.limitClose) {
        Log "  Gate not at CLOSE, moving to CLOSE first..." "STEP"
        Gate-Control "close" | Out-Null
        $s = Gate-WaitMovingThenStopped 120
        if (-not $s -or -not $s.limitClose) {
            Log "  Failed to reach CLOSE limit" "FAIL"
            return $null
        }
    }
    Log "  Gate at CLOSE limit confirmed" "PASS"

    # Record raw hover telemetry at CLOSE
    Start-Sleep -Seconds 1
    $d0 = Gate-Diag
    $rawClose = $d0.hoverUart.dist_mm_raw
    Log "  CLOSE raw hover distMm = $rawClose"

    # Move to OPEN
    Log "  Opening gate to measure travel distance..." "STEP"
    Gate-Control "open" | Out-Null

    # Live log while moving
    $s = Gate-WaitMovingThenStopped 120 400
    if (-not $s) {
        Log "  Gate did not stop after OPEN command" "FAIL"
        return $null
    }

    # Record raw hover telemetry at OPEN
    Start-Sleep -Seconds 1
    $d1 = Gate-Diag
    $rawOpen = $d1.hoverUart.dist_mm_raw
    Log "  OPEN raw hover distMm = $rawOpen"

    $travelMm = $rawOpen - $rawClose
    $travelM = [math]::Round($travelMm / 1000.0, 3)
    Log "  Measured travel: ${travelMm}mm = ${travelM}m" "STEP"

    # Check OPEN limit was hit
    $sFinal = Gate-Full
    $stopReason = $sFinal.gate.stopReason
    $limitOpen = $sFinal.inputs.limitOpen
    Assert ($limitOpen -eq $true) "OPEN limit switch active" "OPEN limit NOT active" | Out-Null
    Assert ($stopReason -eq 6 -or $stopReason -eq 2) "stopReason=$stopReason (6=limit_open, 2=soft_limit)" "Unexpected stopReason=$stopReason" | Out-Null
    if ($stopReason -eq 6) {
        Log "  stopReason = limit_open (CORRECT!)" "PASS"
    } elseif ($stopReason -eq 2) {
        Log "  stopReason = soft_limit (should be limit_open after fix)" "WARN"
    }

    return @{ travelMm = $travelMm; travelM = $travelM; rawClose = $rawClose; rawOpen = $rawOpen }
}

function Test-2-SetMaxDistance($measuredM) {
    Log "========== TEST 2: Set maxDistance = ${measuredM}m ==========" "STEP"

    # Use applyMaxDistance via MQTT or direct API
    # The /api/gate/calibrate with set="open" calibrates current position as maxDistance
    # But we need to set an exact value. Use the config API to set maxDistance directly.

    $cfg = Api-Get "/api/config"
    if (-not $cfg) { Log "Failed to read config" "FAIL"; return $false }
    $oldMax = $cfg.gate.maxDistance
    Log "  Old maxDistance = ${oldMax}m, new = ${measuredM}m"

    # Set via config update
    $body = @{ gate = @{ maxDistance = $measuredM; totalDistance = $measuredM } }
    $r = Api-Post "/api/config" $body
    if ($r -and $r.status -eq "ok") {
        Log "  Config updated successfully" "PASS"
    } else {
        Log "  Config update failed: $($r | ConvertTo-Json)" "FAIL"
        return $false
    }

    # Recalibrate current position as OPEN
    Start-Sleep -Seconds 2
    $r2 = Api-Post "/api/gate/calibrate" @{ set = "open" }
    if ($r2 -and $r2.status -eq "ok") {
        Log "  Calibrated current position as OPEN" "PASS"
    } else {
        Log "  Calibrate set=open failed" "FAIL"
    }

    # Verify
    Start-Sleep -Seconds 1
    $s = Gate-Full
    $newMax = $s.gate.maxDistance
    Assert ([math]::Abs($newMax - $measuredM) -lt 0.05) "maxDistance verified = ${newMax}m (expected ~${measuredM}m)" "maxDistance mismatch: $newMax vs $measuredM" | Out-Null
    return $true
}

function Test-3-FullOpenClose {
    Log "========== TEST 3: Full OPEN -> CLOSE -> OPEN cycle ==========" "STEP"

    # Ensure at OPEN first
    $s = Gate-Status
    if (-not $s.limitOpen) {
        Log "  Moving to OPEN first..." "STEP"
        Gate-Control "open" | Out-Null
        $s = Gate-WaitMovingThenStopped 120
    }
    Assert ($s.limitOpen -eq $true) "At OPEN limit" "Not at OPEN" | Out-Null

    # CLOSE
    Log "  --- CLOSE ---" "STEP"
    Gate-Control "close" | Out-Null
    $s = Gate-WaitMovingThenStopped 120
    Assert ($null -ne $s) "Gate stopped" "Gate did not stop" | Out-Null
    Assert ($s.limitClose -eq $true) "CLOSE limit active" "CLOSE limit NOT active (pos=$($s.positionMm)mm)" | Out-Null
    Assert ($s.positionMm -le 50) "Position near 0 ($($s.positionMm)mm)" "Position too far from 0: $($s.positionMm)mm" | Out-Null
    $sFull = Gate-Full
    Assert ($sFull.gate.stopReason -eq 7) "stopReason=limit_close (7)" "stopReason=$($sFull.gate.stopReason)" | Out-Null

    # OPEN
    Log "  --- OPEN ---" "STEP"
    Gate-Control "open" | Out-Null
    $s = Gate-WaitMovingThenStopped 120
    Assert ($null -ne $s) "Gate stopped" "Gate did not stop" | Out-Null
    Assert ($s.limitOpen -eq $true) "OPEN limit active" "OPEN limit NOT active (pos=$($s.positionMm)mm)" | Out-Null
    $sFull = Gate-Full
    Assert ($sFull.gate.stopReason -eq 6) "stopReason=limit_open (6)" "stopReason=$($sFull.gate.stopReason)" | Out-Null
    Log "  Position at OPEN: $($s.positionMm)mm, percent=$($s.positionPercent)%" "PASS"
}

function Test-4-PartialMoveReopen {
    Log "========== TEST 4: CRITICAL - Partial CLOSE then re-OPEN ==========" "STEP"
    Log "  This is the PRIMARY test for the soft-limit fix" "STEP"

    # Ensure at OPEN
    $s = Gate-Status
    if (-not $s.limitOpen) {
        Gate-Control "open" | Out-Null
        $s = Gate-WaitMovingThenStopped 120
    }
    Assert ($s.limitOpen -eq $true) "Starting at OPEN limit" "Not at OPEN" | Out-Null
    $openPosMm = $s.positionMm
    Log "  OPEN position = ${openPosMm}mm"

    # Partial CLOSE (~3 seconds)
    Log "  --- Partial CLOSE (3s) ---" "STEP"
    Gate-Control "close" | Out-Null
    Start-Sleep -Seconds 3
    Gate-Control "stop" | Out-Null
    $s = Gate-WaitState @("stopped") 10
    $partialPosMm = $s.positionMm
    Log "  Stopped at ${partialPosMm}mm (moved $($openPosMm - $partialPosMm)mm from OPEN)"
    Assert ($s.limitOpen -eq $false) "OPEN limit OFF after partial close" "OPEN limit still ON" | Out-Null
    Assert ($s.limitClose -eq $false) "CLOSE limit OFF (mid-travel)" "CLOSE limit ON (unexpected)" | Out-Null

    # Re-OPEN - THIS IS THE KEY TEST
    Log "  --- Re-OPEN (must reach OPEN limit!) ---" "STEP"
    Gate-Control "open" | Out-Null
    $s = Gate-WaitMovingThenStopped 120
    Assert ($null -ne $s) "Gate stopped" "Gate did not stop" | Out-Null

    $finalPosMm = $s.positionMm
    $limitOpen = $s.limitOpen
    $sFull = Gate-Full
    $stopReason = $sFull.gate.stopReason

    Log "  Final position = ${finalPosMm}mm, limitOpen=$limitOpen, stopReason=$stopReason"
    Assert ($limitOpen -eq $true) "OPEN limit switch ACTIVE" "OPEN limit NOT active - FIX DID NOT WORK!" | Out-Null
    Assert ($stopReason -eq 6) "stopReason=limit_open (6) - CORRECT" "stopReason=$stopReason - WRONG (expected 6=limit_open)" | Out-Null
    Assert ($stopReason -ne 2) "NOT soft_limit (good)" "Stopped by soft_limit - BUG STILL PRESENT" | Out-Null
    Assert ([math]::Abs($finalPosMm - $openPosMm) -le 100) "Position matches OPEN (diff=$([math]::Abs($finalPosMm - $openPosMm))mm)" "Position drift > 100mm" | Out-Null
}

function Test-5-Toggle {
    Log "========== TEST 5: Toggle command ==========" "STEP"

    # Start at OPEN
    $s = Gate-Status
    if (-not $s.limitOpen) {
        Gate-Control "open" | Out-Null
        Gate-WaitMovingThenStopped 120 | Out-Null
    }

    # Toggle 1: should CLOSE
    Log "  --- Toggle 1 (expect CLOSE) ---" "STEP"
    Gate-Control "toggle" | Out-Null
    Start-Sleep -Seconds 2
    $s = Gate-Status
    Assert ($s.state -eq "closing" -or $s.positionMm -lt 8500) "Gate moving toward CLOSE" "Gate not closing" | Out-Null

    # Toggle 2: should STOP
    Log "  --- Toggle 2 (expect STOP) ---" "STEP"
    Gate-Control "toggle" | Out-Null
    $s = Gate-WaitState @("stopped") 10
    Assert ($s.state -eq "stopped") "Gate stopped" "Gate not stopped: $($s.state)" | Out-Null
    $stoppedPos = $s.positionMm
    Log "  Stopped at ${stoppedPos}mm"

    # Toggle 3: should re-OPEN (userStoppedDuringMove)
    Log "  --- Toggle 3 (expect re-OPEN after user stop) ---" "STEP"
    Gate-Control "toggle" | Out-Null
    Start-Sleep -Seconds 2
    $s = Gate-Status
    Assert ($s.state -eq "opening" -or $s.positionMm -gt $stoppedPos) "Gate moving toward OPEN" "Gate not opening: state=$($s.state)" | Out-Null

    # Let it reach OPEN
    $s = Gate-WaitMovingThenStopped 120
    Assert ($s.limitOpen -eq $true) "Reached OPEN limit via toggle" "Did not reach OPEN" | Out-Null
}

function Test-6-MultiPartialCycles {
    Log "========== TEST 6: Multiple partial move cycles (drift test) ==========" "STEP"

    # Start at OPEN
    $s = Gate-Status
    if (-not $s.limitOpen) {
        Gate-Control "open" | Out-Null
        Gate-WaitMovingThenStopped 120 | Out-Null
    }
    $s = Gate-Full
    $basePosMm = [long]($s.gate.position * 1000)
    $baseMax = $s.gate.maxDistance
    Log "  Base OPEN position = ${basePosMm}mm, maxDistance = ${baseMax}m"

    for ($i = 1; $i -le 3; $i++) {
        Log "  --- Cycle $i ---" "STEP"

        # Partial close (random 2-5 seconds)
        $closeSec = Get-Random -Minimum 2 -Maximum 5
        Gate-Control "close" | Out-Null
        Start-Sleep -Seconds $closeSec
        Gate-Control "stop" | Out-Null
        $s = Gate-WaitState @("stopped") 10
        Log "    Partial close ${closeSec}s -> pos=$($s.positionMm)mm"

        # Re-open to OPEN limit
        Gate-Control "open" | Out-Null
        $s = Gate-WaitMovingThenStopped 120
        $posMm = $s.positionMm
        $lo = $s.limitOpen
        $sFull = Gate-Full
        $sr = $sFull.gate.stopReason
        Log "    Re-OPEN -> pos=${posMm}mm limitOpen=$lo stopReason=$sr"
        Assert ($lo -eq $true) "Cycle ${i}: OPEN limit hit" "Cycle ${i}: OPEN limit NOT hit" | Out-Null
        Assert ($sr -eq 6) "Cycle ${i}: stopReason=limit_open" "Cycle ${i}: stopReason=$sr" | Out-Null
        $driftMm = [math]::Abs($posMm - $basePosMm)
        Assert ($driftMm -le 150) "Cycle ${i}: position drift < 150mm (diff=${driftMm}mm)" "Cycle ${i}: position drift ${driftMm}mm > 150mm" | Out-Null
    }
}

function Test-7-FullCloseThenOpen {
    Log "========== TEST 7: Full CLOSE then full OPEN (final check) ==========" "STEP"

    Gate-Control "close" | Out-Null
    $s = Gate-WaitMovingThenStopped 120
    Assert ($s.limitClose -eq $true) "Reached CLOSE limit" "Did not reach CLOSE" | Out-Null
    $sFull = Gate-Full
    Assert ($sFull.gate.stopReason -eq 7) "stopReason=limit_close" "stopReason=$($sFull.gate.stopReason)" | Out-Null

    Gate-Control "open" | Out-Null
    $s = Gate-WaitMovingThenStopped 120
    Assert ($s.limitOpen -eq $true) "Reached OPEN limit" "Did not reach OPEN" | Out-Null
    $sFull = Gate-Full
    Assert ($sFull.gate.stopReason -eq 6) "stopReason=limit_open" "stopReason=$($sFull.gate.stopReason)" | Out-Null
    Log "  Final position: $($s.positionMm)mm, percent=$($s.positionPercent)%"
}

function Test-8-DiagnosticsCheck {
    Log "========== TEST 8: Post-test diagnostics ==========" "STEP"
    $d = Gate-Diag
    $s = Gate-Full
    Log "  heap=$($d.runtime.freeHeap) minHeap=$($d.runtime.minFreeHeap)"
    Log "  gateTaskStack=$($d.runtime.gateTaskStackWords) mainLoopStack=$($d.runtime.mainLoopStackWords)"
    Log "  limitSafetyStopCount=$($d.gate.limitSafetyStopCount)"
    Log "  positionRaw=$($d.position.positionMetersRaw)m filtered=$($d.position.positionMetersFiltered)m"
    Log "  hbOriginDistMm=$($d.position.hbOriginDistMm)"
    Log "  maxDistance=$($d.position.maxDistanceMeters)m"
    Log "  mqtt=$($s.mqtt.connected) wifi=$($s.wifi.connected) rssi=$($s.wifi.rssi)"
    Log "  errorCode=$($s.gate.errorCode) hoverFault=$($d.hoverUart.fault)"
    Assert ($d.runtime.freeHeap -gt 50000) "Heap OK ($($d.runtime.freeHeap))" "Heap low" | Out-Null
    Assert ($d.hoverUart.fault -eq 0) "No hover faults" "Hover fault=$($d.hoverUart.fault)" | Out-Null
    Assert ($s.gate.errorCode -eq 0) "No gate errors" "Gate error=$($s.gate.errorCode)" | Out-Null
}

# ============================================================
# MAIN
# ============================================================

Log "======================================================" "STEP"
Log "  GateOS Gate Test Suite" "STEP"
Log "  Target: $BaseUrl" "STEP"
Log "  Log file: $LogFile" "STEP"
Log "======================================================" "STEP"

# TEST 0: Connectivity
$initStatus = Test-0-Connectivity
if (-not $initStatus) { Log "ABORT: Cannot reach gate" "FAIL"; exit 1 }

# TEST 1: Measure actual distance
$measure = Test-1-MeasureDistance
if ($measure) {
    Log "  *** Measured distance: $($measure.travelM)m ($($measure.travelMm)mm) ***" "PASS"

    # TEST 2: Set maxDistance if different from current
    $currentMax = (Gate-Full).gate.maxDistance
    $diff = [math]::Abs($currentMax - $measure.travelM)
    if ($diff -gt 0.05) {
        Log "  Distance differs by ${diff}m from current maxDistance=${currentMax}m - updating" "WARN"
        Test-2-SetMaxDistance $measure.travelM
    } else {
        Log "  Current maxDistance (${currentMax}m) matches measured ($($measure.travelM)m) within 50mm - keeping" "PASS"
    }
} else {
    Log "  Distance measurement failed, continuing with current maxDistance" "WARN"
}

# TEST 3: Full cycle
Test-3-FullOpenClose

# TEST 4: CRITICAL - Partial close then re-open
Test-4-PartialMoveReopen

# TEST 5: Toggle
Test-5-Toggle

# TEST 6: Multiple partial cycles (drift test)
Test-6-MultiPartialCycles

# TEST 7: Final full cycle
Test-7-FullCloseThenOpen

# TEST 8: Final diagnostics
Test-8-DiagnosticsCheck

Log "======================================================" "STEP"
Log "  ALL TESTS COMPLETE" "STEP"
Log "  Log file: $LogFile" "STEP"
Log "======================================================" "STEP"
