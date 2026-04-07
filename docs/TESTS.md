# GateOS Testing Documentation

## Overview

GateOS includes comprehensive automated testing for:
- Position accuracy and repeatability
- Open/close cycle timing
- UART communication reliability
- Web API responsiveness
- Safety system functionality

---

## Prerequisites

### Python Dependencies

```bash
pip install requests websocket-client
```

### Network Access

- Gate controller must be accessible on the network
- Default URL: `http://gate.local` or `http://<IP_ADDRESS>`

---

## Running Tests

### Full Test Suite

```bash
cd tests
python3 test_gate.py --url http://gate.local
```

Or using the build script:

```bash
./scripts/build.sh test --url http://gate.local
```

### Individual Tests

**Open/Close Cycle:**
```bash
python3 test_gate.py --test cycle --url http://gate.local
```

**Position Repeatability (10 cycles):**
```bash
python3 test_gate.py --test repeatability --iterations 10 --url http://gate.local
```

**UART Stress Test:**
```bash
python3 test_gate.py --test uart --url http://gate.local
```

**API Latency:**
```bash
python3 test_gate.py --test latency --url http://gate.local
```

---

## Test Descriptions

### 1. Open/Close Cycle Test

**Purpose:** Verify basic gate operation

**Procedure:**
1. Send OPEN command
2. Wait for gate to reach open limit
3. Measure opening time
4. Send CLOSE command
5. Wait for gate to reach close limit
6. Measure closing time
7. Verify final position ~0m

**Pass Criteria:**
- Gate completes full cycle
- Opening time < 60 seconds
- Closing time < 60 seconds
- Final position within 10cm of zero

---

### 2. Position Repeatability Test

**Purpose:** Verify consistent positioning over multiple cycles

**Procedure:**
1. Run N complete open/close cycles (default: 10)
2. Record open position for each cycle
3. Calculate statistics (mean, std dev, range)

**Pass Criteria:**
- Standard deviation < 50mm
- Range (max-min) < 100mm
- No timeouts or errors

**Output Example:**
```
=== Results ===
Mean position:   5.023m
Std deviation:   12.5mm
Min position:    5.005m
Max position:    5.045m
Range:           40.0mm
[OK] Position repeatability within tolerance
```

---

### 3. UART Stress Test

**Purpose:** Verify reliable communication under load

**Procedure:**
1. Send 100 rapid commands
2. Track success/failure rate
3. Monitor UART error counters
4. Calculate error rate

**Pass Criteria:**
- Command success rate > 95%
- UART error rate < 1%
- No connection drops

**Output Example:**
```
Duration:        2.34s
Commands sent:   100
Success rate:    99.0%
RX lines delta:  2500
Bad lines delta: 5
Error rate:      0.20%
[OK] UART stress test passed
```

---

### 4. Web API Latency Test

**Purpose:** Verify web server responsiveness

**Procedure:**
1. Send 20 status requests
2. Measure response time for each
3. Calculate mean, P95, max latency

**Pass Criteria:**
- Mean latency < 100ms
- P95 latency < 200ms
- No timeouts (>5s)

**Output Example:**
```
Mean latency:    23.5ms
P95 latency:     45.2ms
Max latency:     89.1ms
[OK] Web API responsive
```

---

### 5. Safety Obstacle Test (Manual)

**Purpose:** Verify obstacle detection and response

**Procedure:**
1. Place obstacle in gate path
2. Initiate close command
3. Verify gate stops immediately
4. Verify gate reverses (if configured)
5. Remove obstacle
6. Verify normal operation resumes

**Pass Criteria:**
- Gate stops within 100mm of obstacle
- No damage or unsafe behavior
- Normal operation resumes after obstacle removal

---

## Interpreting Results

### Test Output Format

```
[OK] - Test passed
[FAIL] - Test failed
[WARN] - Warning, non-critical issue
[ERROR] - Critical error, test aborted
```

### Common Failures

| Failure | Possible Cause | Solution |
|---------|---------------|----------|
| WebSocket connection failed | Wrong URL, gate offline | Check network, IP address |
| Opening timeout | Motor fault, obstruction | Check motor, remove obstacles |
| Position variance high | Wheel slip, loose coupling | Check mechanical linkage |
| UART error rate high | Wiring, baud rate mismatch | Check connections, config |
| API latency high | WiFi interference, load | Check signal strength |

---

## Continuous Integration

### Automated Test Schedule

Recommended testing frequency:
- **After firmware update**: Full suite
- **Daily**: Position repeatability (5 cycles)
- **Weekly**: Full suite + manual safety test

### CI Script Example

```bash
#!/bin/bash
# ci_test.sh

set -e

URL="http://gate.local"

echo "Running CI tests..."

# Quick health check
python3 test_gate.py --test latency --url $URL || exit 1

# Position test (3 cycles for speed)
python3 test_gate.py --test repeatability --iterations 3 --url $URL || exit 1

# UART stress (50 commands)
python3 test_gate.py --test uart --url $URL || exit 1

echo "All CI tests passed!"
```

---

## Performance Benchmarks

### Expected Values (Reference Gate)

| Metric | Target | Acceptable | Notes |
|--------|--------|------------|-------|
| Open time | 20-30s | <60s | For 5m gate |
| Close time | 20-30s | <60s | For 5m gate |
| Position σ | <20mm | <50mm | Standard deviation |
| UART errors | <0.1% | <1% | Error rate |
| API latency | <30ms | <100ms | Mean response |

---

## Troubleshooting

### Test Fails Immediately

**Check:**
1. Gate is powered on
2. WiFi network accessible
3. Correct URL/IP address
4. Firewall not blocking port 80

### Intermittent Failures

**Check:**
1. WiFi signal strength (RSSI > -70dBm)
2. Power supply stability
3. Loose connections
4. EMI/RFI interference

### Position Drift

**Check:**
1. Wheel coupling tightness
2. Limit switch alignment
3. Hall sensor gap
4. Configuration parameters

---

## Reporting Issues

When reporting test failures, include:

1. Test output (full log)
2. Gate configuration (`GET /api/config`)
3. Diagnostics (`GET /api/diagnostics`)
4. Firmware version (`GET /api/version`)
5. Environmental conditions (temperature, etc.)

---

## Future Test Additions

Planned improvements:
- [ ] Current consumption analysis
- [ ] Temperature stress testing
- [ ] Long-term endurance (1000+ cycles)
- [ ] Noise/EMI immunity testing
- [ ] Power loss recovery testing
- [ ] MQTT integration testing
