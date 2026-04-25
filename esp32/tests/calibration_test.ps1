# GateOS Calibration Test
$BaseUrl = "http://192.168.1.44:8080"
$LogFile = "calibration_test_$(Get-Date -Format 'yyyyMMdd_HHmmss').log"

function Log($msg, $level = "INFO") {
    $ts = Get-Date -Format "HH:mm:ss.fff"
    $line = "[$ts] [$level] $msg"
    Write-Host $line -ForegroundColor $(switch($level) { "PASS" {"Green"} "FAIL" {"Red"} "WARN" {"Yellow"} "STEP" {"Cyan"} "DATA" {"DarkGray"} default {"White"} })
    Add-Content -Path $LogFile -Value $line
}

function Api-Get($path) {
    try {
        $r = Invoke-WebRequest -Uri "$BaseUrl$path" -TimeoutSec 10 -UseBasicParsing
        return ($r.Content | ConvertFrom-Json)
    } catch { Log "API GET $path FAILED: $_" "FAIL"; return $null }
}

function Api-Post($path, $body = $null) {
    try {
        $params = @{ Uri = "$BaseUrl$path"; Method = "POST"; TimeoutSec = 10; UseBasicParsing = $true }
        if ($body) { $params.Body = ($body | ConvertTo-Json -Depth 5); $params.ContentType = "application/json" }
        $r = Invoke-WebRequest @params
        return ($r.Content | ConvertFrom-Json)
    } catch {
        $code = $_.Exception.Response.StatusCode.value__
        Log "API POST $path FAILED (HTTP $code): $_" "FAIL"
        return $null
    }
}

# ============================================================
Log "======================================================" "STEP"
Log "  GateOS Calibration Test" "STEP"
Log "  Target: $BaseUrl" "STEP"
Log "======================================================" "STEP"

# --- TEST 1: Check connectivity and initial state ---
Log "========== TEST 1: Pre-calibration state ==========" "STEP"
$status = Api-Get "/api/status"
if (-not $status) { Log "Cannot reach device!" "FAIL"; exit 1 }
Log "state=$($status.gate.state) pos=$($status.gate.position)mm maxDist=$($status.gate.maxDistance)m" "INFO"
Log "limitOpen=$($status.io.limitOpen) limitClose=$($status.io.limitClose)" "INFO"
Log "hoverEnabled=$($status.hb.enabled) rpm=$($status.hb.rpm) fault=$($status.hb.fault)" "INFO"

$cfg = Api-Get "/api/config"
if (-not $cfg) { Log "Cannot read config!" "FAIL"; exit 1 }
Log "Config: maxDistance=$($cfg.gate.maxDistance)m softLimits=$($cfg.gate.softLimitsEnabled)" "INFO"
Log "Config: motorInvert=$($cfg.motor.invertDir) softStart=$($cfg.motor.softStartMs) softStop=$($cfg.motor.softStopMs)" "INFO"
Log "Config: hoverMaxSpeed=$($cfg.hoverUart.maxSpeed) rampStep=$($cfg.hoverUart.rampStep)" "INFO"

# Save original config for restore
$origMaxDist = $cfg.gate.maxDistance
$origMotorInvert = $cfg.motor.invertDir
Log "Original maxDistance=$origMaxDist motorInvert=$origMotorInvert" "STEP"

# --- TEST 2: Check calibration status before start ---
Log "========== TEST 2: Calibration status (idle) ==========" "STEP"
$calStatus = Api-Get "/api/calibration/status"
if ($calStatus) {
    Log "running=$($calStatus.running) step=$($calStatus.step) error=$($calStatus.error)" "INFO"
    if ($calStatus.running -eq $false) {
        Log "Calibration is idle - OK" "PASS"
    } else {
        Log "Calibration already running - stopping first" "WARN"
        Api-Post "/api/calibration/stop" | Out-Null
        Start-Sleep -Seconds 2
    }
} else {
    Log "Could not get calibration status" "FAIL"
}

# --- TEST 3: Start calibration ---
Log "========== TEST 3: Start calibration ==========" "STEP"
$startResult = Api-Post "/api/calibration/start"
if ($startResult -and $startResult.status -eq "ok") {
    Log "Calibration started successfully" "PASS"
} else {
    Log "Calibration start failed: $($startResult | ConvertTo-Json)" "FAIL"
    exit 1
}

# --- TEST 4: Monitor calibration progress ---
Log "========== TEST 4: Monitor calibration ==========" "STEP"
$prevStep = ""
$timeout = 180  # 3 minutes max
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$needsConfirm = $false
$calibCompleted = $false
$calibError = $false

while ($sw.Elapsed.TotalSeconds -lt $timeout) {
    Start-Sleep -Milliseconds 500
    $calStatus = Api-Get "/api/calibration/status"
    if (-not $calStatus) { 
        Log "Lost calibration status!" "WARN"
        continue 
    }

    $step = $calStatus.step
    $progress = $calStatus.progress
    $msg = $calStatus.message
    $err = $calStatus.error

    if ($step -ne $prevStep) {
        Log "STEP: $step (progress=$progress%) msg='$msg'" "STEP"
        $prevStep = $step
    }

    # Check if direction confirmation needed
    if ($step -eq "confirm_direction" -or $step -eq "wait_confirm") {
        if (-not $needsConfirm) {
            $needsConfirm = $true
            Log "Direction confirmation required!" "STEP"
            
            # Read proposed direction
            $proposed = $calStatus.proposed
            Log "Proposed config: $($calStatus | ConvertTo-Json -Depth 3 -Compress)" "DATA"
            
            # Auto-confirm with current direction (no invert)
            Start-Sleep -Seconds 1
            Log "Auto-confirming direction (invert=false)..." "STEP"
            $confirmResult = Api-Post "/api/calibration/confirm_dir" @{ invert = 0 }
            if ($confirmResult) {
                Log "Direction confirmed" "PASS"
            } else {
                # Try alternative confirm endpoint
                $confirmResult = Api-Post "/api/calibration/confirm_dir" @{ invert = $false }
                if ($confirmResult) {
                    Log "Direction confirmed (alt)" "PASS"
                } else {
                    Log "Direction confirm failed!" "FAIL"
                }
            }
        }
    }

    # Check completion
    if ($step -eq "done" -or $step -eq "complete" -or $step -eq "finished") {
        $calibCompleted = $true
        Log "Calibration completed! step=$step" "PASS"
        break
    }

    # Check error
    if ($err -and $err.Length -gt 0) {
        $calibError = $true
        Log "Calibration error: $err (step=$step)" "FAIL"
        break
    }

    # Check if stopped
    if ($calStatus.running -eq $false -and $sw.Elapsed.TotalSeconds -gt 5) {
        if ($step -eq "done" -or $step -eq "complete") {
            $calibCompleted = $true
            Log "Calibration completed (running=false, step=$step)" "PASS"
        } else {
            Log "Calibration stopped unexpectedly (step=$step msg='$msg')" "WARN"
        }
        break
    }

    # Log hover telemetry periodically
    if ([int]$sw.Elapsed.TotalSeconds % 5 -eq 0) {
        $sl = Api-Get "/api/status-lite"
        if ($sl) {
            Log "  state=$($sl.state) pos=$($sl.positionMm)mm rpm=$($sl.rpm) iA=$($sl.iA) limitO=$($sl.limitOpen) limitC=$($sl.limitClose)" "DATA"
        }
    }
}

if (-not $calibCompleted -and -not $calibError) {
    Log "Calibration timed out after ${timeout}s" "FAIL"
}

# --- TEST 5: Check calibration results ---
Log "========== TEST 5: Post-calibration status ==========" "STEP"
$calFinal = Api-Get "/api/calibration/status"
if ($calFinal) {
    Log "Final calibration state:" "STEP"
    Log "  running=$($calFinal.running) step=$($calFinal.step) progress=$($calFinal.progress)" "INFO"
    Log "  message='$($calFinal.message)'" "INFO"
    Log "  error='$($calFinal.error)'" "INFO"
    
    # Log proposed values if available
    if ($calFinal.proposed) {
        Log "  Proposed values: $($calFinal.proposed | ConvertTo-Json -Depth 3 -Compress)" "DATA"
    }
}

# --- TEST 6: Apply calibration (if completed) ---
if ($calibCompleted) {
    Log "========== TEST 6: Apply calibration ==========" "STEP"
    $applyResult = Api-Post "/api/calibration/apply"
    if ($applyResult -and $applyResult.status -eq "ok") {
        Log "Calibration applied successfully!" "PASS"
        Log "Device will restart..." "STEP"
        
        # Wait for reboot
        Start-Sleep -Seconds 8
        $retries = 20
        for ($i = 0; $i -lt $retries; $i++) {
            try {
                $s = Api-Get "/api/status-lite"
                if ($s) {
                    Log "Device back online after restart" "PASS"
                    break
                }
            } catch {}
            Start-Sleep -Seconds 2
        }
        if ($i -ge $retries) {
            Log "Device did not come back online!" "FAIL"
        }
    } else {
        Log "Calibration apply failed: $($applyResult | ConvertTo-Json)" "FAIL"
    }
}

# --- TEST 7: Verify post-calibration config ---
Log "========== TEST 7: Verify calibrated config ==========" "STEP"
Start-Sleep -Seconds 3
$newCfg = Api-Get "/api/config"
if ($newCfg) {
    Log "New maxDistance=$($newCfg.gate.maxDistance)m (was $origMaxDist)" "INFO"
    Log "New motorInvert=$($newCfg.motor.invertDir) (was $origMotorInvert)" "INFO"
    Log "New softStart=$($newCfg.motor.softStartMs) softStop=$($newCfg.motor.softStopMs)" "INFO"
    Log "New hbOriginDistMm=$($newCfg.gate.hbOriginDistMm)" "INFO"
    
    if ($newCfg.gate.maxDistance -gt 0 -and $newCfg.gate.maxDistance -ne $origMaxDist) {
        Log "maxDistance changed: $origMaxDist -> $($newCfg.gate.maxDistance)" "PASS"
    } elseif ($newCfg.gate.maxDistance -eq $origMaxDist) {
        Log "maxDistance unchanged ($origMaxDist) - calibration may not have measured distance" "WARN"
    }
}

$newStatus = Api-Get "/api/status"
if ($newStatus) {
    Log "Post-cal: state=$($newStatus.gate.state) pos=$($newStatus.gate.position)mm maxDist=$($newStatus.gate.maxDistance)m" "INFO"
    Log "Post-cal: limitOpen=$($newStatus.io.limitOpen) limitClose=$($newStatus.io.limitClose)" "INFO"
    Log "Post-cal: homing=$($newStatus.homing.result) reason=$($newStatus.homing.reason)" "INFO"
}

# --- TEST 8: Functional test after calibration ---
Log "========== TEST 8: Post-calibration movement test ==========" "STEP"
$s = Api-Get "/api/status-lite"
if ($s) {
    $startPos = $s.positionMm
    Log "Starting position: ${startPos}mm limitO=$($s.limitOpen) limitC=$($s.limitClose)" "INFO"
    
    # Open gate
    Log "Opening gate..." "STEP"
    Api-Post "/api/control" @{ action = "open" } | Out-Null
    $sw2 = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw2.Elapsed.TotalSeconds -lt 90) {
        Start-Sleep -Milliseconds 800
        $s = Api-Get "/api/status-lite"
        if ($s) {
            Log "  state=$($s.state) pos=$($s.positionMm)mm rpm=$($s.rpm) limitO=$($s.limitOpen)" "DATA"
            if (-not $s.moving -and $sw2.Elapsed.TotalSeconds -gt 3) { break }
        }
    }
    $s = Api-Get "/api/status-lite"
    if ($s -and $s.limitOpen) {
        Log "OPEN limit reached at $($s.positionMm)mm" "PASS"
    } else {
        Log "OPEN limit NOT reached (pos=$($s.positionMm)mm limitOpen=$($s.limitOpen) stopReason=$($s.stopReason))" "WARN"
    }
    
    Start-Sleep -Seconds 2
    
    # Close gate
    Log "Closing gate..." "STEP"
    Api-Post "/api/control" @{ action = "close" } | Out-Null
    $sw2 = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw2.Elapsed.TotalSeconds -lt 90) {
        Start-Sleep -Milliseconds 800
        $s = Api-Get "/api/status-lite"
        if ($s) {
            Log "  state=$($s.state) pos=$($s.positionMm)mm rpm=$($s.rpm) limitC=$($s.limitClose)" "DATA"
            if (-not $s.moving -and $sw2.Elapsed.TotalSeconds -gt 3) { break }
        }
    }
    $s = Api-Get "/api/status-lite"
    if ($s -and $s.limitClose) {
        Log "CLOSE limit reached at $($s.positionMm)mm" "PASS"
    } else {
        Log "CLOSE limit NOT reached (pos=$($s.positionMm)mm limitClose=$($s.limitClose) stopReason=$($s.stopReason))" "WARN"
    }
}

Log "======================================================" "STEP"
Log "  CALIBRATION TEST COMPLETE" "STEP"
Log "  Log: $LogFile" "STEP"
Log "======================================================" "STEP"
