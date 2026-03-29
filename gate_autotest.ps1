# gate_autotest.ps1  (PowerShell 5.1 compatible)
param(
  [string]$Base = "http://192.168.1.12",
  [int]$IntervalMs = 250,

  [int]$TelAgeWarnMs = 1500,
  [int]$TelAgeFailMs = 3000,
  [int]$CmdAgeWarnMs = 1200,

  [int]$MoveTimeoutSec = 60,
  [int]$SettleSec = 30,

  [switch]$NoCommands,
  [string]$OutCsv = ""
)

$ErrorActionPreference = "Stop"

function NowIso { (Get-Date).ToString("yyyy-MM-dd HH:mm:ss.fff") }

function Get-Json([string]$Url, [int]$TimeoutSec = 3) {
  $resp = Invoke-WebRequest -UseBasicParsing -Uri $Url -TimeoutSec $TimeoutSec
  return ($resp.Content | ConvertFrom-Json)
}

function Safe-GetJson([string]$Url) {
  try { return Get-Json $Url } catch { return $null }
}

function Post-Json([string]$Url, $Obj, [int]$TimeoutSec = 3) {
  $json = ($Obj | ConvertTo-Json -Compress)
  Invoke-WebRequest -UseBasicParsing -Uri $Url -Method POST -ContentType "application/json" -Body $json -TimeoutSec $TimeoutSec | Out-Null
}

function Try-SendControl([string]$Cmd) {
  if ($NoCommands) {
    Write-Host "[INFO] NoCommands=ON -> pomijam komendę: $Cmd"
    return $true
  }

  $tries = @(
    @{ url = "$Base/api/control"; body = @{ cmd = $Cmd } },
    @{ url = "$Base/api/gate";    body = @{ cmd = $Cmd } },
    @{ url = "$Base/api/cmd";     body = @{ cmd = $Cmd } },
    @{ url = "$Base/api/command"; body = @{ cmd = $Cmd } }
  )

  foreach ($t in $tries) {
    try {
      Post-Json $t.url $t.body
      Write-Host "[OK] POST $($t.url) cmd=$Cmd"
      return $true
    } catch {
      # próbuj dalej
    }
  }

  Write-Host "[ERROR] Nie udało się wysłać komendy '$Cmd' (sprawdź endpoint /api/control)."
  return $false
}

function Get-Path($obj, [string]$path) {
  if ($null -eq $obj) { return $null }
  $cur = $obj
  foreach ($p in $path.Split(".")) {
    if ($null -eq $cur) { return $null }
    if ($cur.PSObject.Properties.Name -contains $p) { $cur = $cur.$p } else { return $null }
  }
  return $cur
}

function Get-Rpm($st) {
  $v = Get-Path $st "hoverUart.rpm"
  if ($null -ne $v) { return [int]$v }
  $v = Get-Path $st "hb.rpm"
  if ($null -ne $v) { return [int]$v }
  return $null
}

function Get-IA($st) {
  $v = Get-Path $st "hoverUart.iA"
  if ($null -ne $v) { return [double]$v }
  $v = Get-Path $st "hb.iA"
  if ($null -ne $v) { return [double]$v }
  return $null
}

function Get-TelAgeMs($st) {
  $v = Get-Path $st "hoverUart.telAgeMs"
  if ($null -ne $v) { return [int]$v }
  $v = Get-Path $st "hb.telAgeMs"
  if ($null -ne $v) { return [int]$v }
  return $null
}

function Get-CmdAgeMs($st) {
  $v = Get-Path $st "hoverUart.cmdAgeMs"
  if ($null -ne $v) { return [int]$v }
  $v = Get-Path $st "hb.cmdAgeMs"
  if ($null -ne $v) { return [int]$v }
  return $null
}

function Get-Fault($st) {
  $v = Get-Path $st "hoverUart.fault"
  if ($null -ne $v) { return [int]$v }
  $v = Get-Path $st "hb.fault"
  if ($null -ne $v) { return [int]$v }
  return $null
}

function Get-DistMm($st) {
  $v = Get-Path $st "hoverUart.dist_mm"
  if ($null -ne $v) { return [int]$v }
  $v = Get-Path $st "hb.dist_mm"
  if ($null -ne $v) { return [int]$v }
  return $null
}

function Get-Moving($st) {
  $m = Get-Path $st "gate.moving"
  if ($null -ne $m) { return [bool]$m }
  $m = Get-Path $st "gate.movingNow"
  if ($null -ne $m) { return [bool]$m }
  $rpm = Get-Rpm $st
  if ($null -ne $rpm) { return ([int]$rpm -ne 0) }
  return $false
}

function Get-PosPercent($st) {
  $p = Get-Path $st "gate.positionPercent"
  if ($null -ne $p) { return [int]$p }

  $pos = Get-Path $st "gate.position"
  $max = Get-Path $st "gate.maxDistance"
  if ($null -ne $pos -and $null -ne $max -and [double]$max -ne 0) {
    return [int]([math]::Round(100.0 * ([double]$pos / [double]$max), 0))
  }
  return $null
}

function Wait-Until([scriptblock]$cond, [int]$timeoutSec, [int]$pollMs) {
  $sw = [Diagnostics.Stopwatch]::StartNew()
  while ($sw.Elapsed.TotalSeconds -lt $timeoutSec) {
    if (& $cond) { return $true }
    Start-Sleep -Milliseconds $pollMs
  }
  return $false
}

# CSV output
if ([string]::IsNullOrWhiteSpace($OutCsv)) {
  $stamp = (Get-Date).ToString("yyyyMMdd_HHmmss")
  $OutCsv = "gate_test_$stamp.csv"
}
Write-Host "[INFO] Log CSV: $OutCsv"
"ts,ok,http_ok,gate_state,moving,pos_percent,rpm,iA,telAgeMs,cmdAgeMs,fault,errorCode,obstacle,dist_mm,notes" | Out-File -Encoding utf8 -FilePath $OutCsv

# statystyki
$maxTel = 0
$maxCmd = 0
$anoms = 0
$lastDist = $null
$distJumps = 0

function Log-Row($st, [bool]$ok, [string]$notes) {
  $ts = NowIso
  $gateState = Get-Path $st "gate.state"
  $moving = Get-Moving $st
  $pos = Get-PosPercent $st
  $rpm = Get-Rpm $st
  $iA = Get-IA $st
  $tel = Get-TelAgeMs $st
  $cmd = Get-CmdAgeMs $st
  $fault = Get-Fault $st
  $err = Get-Path $st "gate.errorCode"
  $obs = Get-Path $st "gate.obstacle"
  $dist = Get-DistMm $st

  if ($null -ne $tel -and $tel -gt $script:maxTel) { $script:maxTel = $tel }
  if ($null -ne $cmd -and $cmd -gt $script:maxCmd) { $script:maxCmd = $cmd }

  if ($null -ne $dist -and $null -ne $script:lastDist) {
    $d = [int]$dist - [int]$script:lastDist
    if ([math]::Abs($d) -gt 1500 -and -not $moving) {
      $script:distJumps++
      if ($notes) { $notes += " | " }
      $notes += "DIST_JUMP($d)"
    }
  }
  $script:lastDist = $dist

  $okInt = 0
  if ($ok) { $okInt = 1 }
  $movInt = 0
  if ($moving) { $movInt = 1 }

  if ($null -eq $gateState) { $gateState = "" }
  $gateState = ($gateState -replace ",",";")
  if ($null -eq $notes) { $notes = "" }
  $notes = ($notes -replace '"',"'" )

  $line = '{0},{1},{2},{3},{4},{5},{6},{7},{8},{9},{10},{11},{12},{13},"{14}"' -f `
    $ts, $okInt, $okInt, $gateState, $movInt, $pos, $rpm, $iA, $tel, $cmd, $fault, $err, $obs, $dist, $notes

  Add-Content -Encoding utf8 -Path $OutCsv -Value $line
}

function Sample([string]$notes) {
  $st = Safe-GetJson "$Base/api/status"
  if ($null -eq $st) {
    $script:anoms++
    Add-Content -Encoding utf8 -Path $OutCsv -Value "$(NowIso),0,0,,,,,,,,,,,,,""HTTP_FAIL $notes"""
    return $null
  }

  $moving = Get-Moving $st
  $tel = Get-TelAgeMs $st
  $cmd = Get-CmdAgeMs $st
  $fault = Get-Fault $st
  $err = Get-Path $st "gate.errorCode"

  $localNotes = $notes

  if ($null -ne $tel) {
    if ($tel -ge $TelAgeFailMs) {
      $script:anoms++
      if ($localNotes) { $localNotes += " | " }
      $localNotes += "TEL_AGE_FAIL($tel)"
    } elseif ($tel -ge $TelAgeWarnMs) {
      if ($localNotes) { $localNotes += " | " }
      $localNotes += "TEL_AGE_WARN($tel)"
    }
  }

  if ($moving -and $null -ne $cmd -and $cmd -ge $CmdAgeWarnMs) {
    $script:anoms++
    if ($localNotes) { $localNotes += " | " }
    $localNotes += "CMD_AGE_WARN($cmd)"
  }

  if ($null -ne $fault -and $fault -ne 0) {
    $script:anoms++
    if ($localNotes) { $localNotes += " | " }
    $localNotes += "FAULT($fault)"
  }

  if ($null -ne $err -and [int]$err -ne 0) {
    $script:anoms++
    if ($localNotes) { $localNotes += " | " }
    $localNotes += "ERRCODE($err)"
  }

  Log-Row $st $true $localNotes
  return $st
}

function Run-Monitor([int]$sec, [string]$tag) {
  $iters = [int][math]::Ceiling(($sec * 1000.0) / $IntervalMs)
  for ($i=0; $i -lt $iters; $i++) {
    Sample $tag | Out-Null
    Start-Sleep -Milliseconds $IntervalMs
  }
}

function Wait-MoveStop([string]$tag) {
  $ok = Wait-Until -timeoutSec $MoveTimeoutSec -pollMs $IntervalMs -cond {
    $st = Safe-GetJson "$Base/api/status"
    if ($null -eq $st) { return $false }
    Log-Row $st $true $tag
    $moving = Get-Moving $st
    return (-not $moving)
  }
  if (-not $ok) {
    Write-Host "[WARN] Timeout czekania na STOP (>$MoveTimeoutSec s)"
    $script:anoms++
  }
}

# =========================
# START TESTU
# =========================
Write-Host "[INFO] Start monitor baseline 3s..."
Run-Monitor 3 "BASELINE"

if (-not $NoCommands) {
  Write-Host "`n[TEST] CLOSE -> czekam aż przestanie jechać..."
  Try-SendControl "close" | Out-Null
  Wait-MoveStop "AFTER_CLOSE_WAITSTOP"

  Write-Host "[TEST] Settle $SettleSec s..."
  Run-Monitor $SettleSec "SETTLE_AFTER_CLOSE"

  Write-Host "`n[TEST] OPEN -> czekam aż przestanie jechać..."
  Try-SendControl "open" | Out-Null
  Wait-MoveStop "AFTER_OPEN_WAITSTOP"

  Write-Host "[TEST] Settle $SettleSec s..."
  Run-Monitor $SettleSec "SETTLE_AFTER_OPEN"

  Write-Host "`n[TEST] STOP w trakcie ruchu (krótki ruch + STOP)..."
  Try-SendControl "close" | Out-Null
  Run-Monitor 2 "MOVE_BEFORE_STOP"
  Try-SendControl "stop" | Out-Null
  Run-Monitor 5 "AFTER_STOP"
} else {
  Write-Host "[INFO] NoCommands=ON -> tylko monitoring 60s"
  Run-Monitor 60 "MONITOR_ONLY"
}

Write-Host "`n==================== PODSUMOWANIE ===================="
Write-Host "CSV: $OutCsv"
Write-Host "Max telAgeMs: $maxTel (WARN=$TelAgeWarnMs / FAIL=$TelAgeFailMs)"
Write-Host "Max cmdAgeMs: $maxCmd (WARN=$CmdAgeWarnMs podczas ruchu)"
Write-Host "Anomalie: $anoms"
Write-Host "Skoki dystansu na postoju: $distJumps"
Write-Host "======================================================"
