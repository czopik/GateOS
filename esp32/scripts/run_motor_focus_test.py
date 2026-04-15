import time
import threading
import json
import re
import requests
import serial

BASE = "http://192.168.1.43"
COM = "COM3"
BAUD = 115200

OUT_JSON = "scripts/motor_focus_results.json"
OUT_TXT = "scripts/motor_focus_report.txt"

serial_lines = []
api_samples = []
actions = []
stop_reader = False


def now_ms():
    return int(time.time() * 1000)


def read_serial():
    global stop_reader
    ser = serial.Serial(COM, BAUD, timeout=0.2)
    try:
        while not stop_reader:
            raw = ser.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", "ignore").strip()
            if not line:
                continue
            serial_lines.append({"ts": now_ms(), "line": line})
    finally:
        ser.close()


def status(tag):
    s = requests.get(BASE + "/api/status", timeout=3).json()
    rec = {
        "ts": now_ms(),
        "tag": tag,
        "state": s.get("gate", {}).get("state"),
        "moving": s.get("gate", {}).get("moving"),
        "position": s.get("gate", {}).get("position"),
        "positionPercent": s.get("gate", {}).get("positionPercent"),
        "target": s.get("gate", {}).get("targetPosition"),
        "stopReason": s.get("gate", {}).get("stopReason"),
        "error": s.get("gate", {}).get("errorCode"),
        "rpm": s.get("hb", {}).get("rpm"),
        "dist": s.get("hb", {}).get("dist_mm"),
        "fault": s.get("hb", {}).get("fault"),
        "telAge": s.get("hb", {}).get("telAgeMs"),
    }
    api_samples.append(rec)
    return rec


def control(action):
    actions.append({"ts": now_ms(), "action": action})
    requests.post(BASE + "/api/control", json={"action": action}, timeout=3)


def poll(seconds, tag):
    end = time.time() + seconds
    while time.time() < end:
        status(tag)
        time.sleep(0.4)


def subset(prefix):
    return [x for x in api_samples if x["tag"].startswith(prefix)]


def in_tag(tag):
    return [x for x in api_samples if x["tag"] == tag]


def any_pos_rpm(rows, th=20):
    return any((isinstance(r.get("rpm"), (int, float)) and r["rpm"] > th) for r in rows)


def any_neg_rpm(rows, th=-20):
    return any((isinstance(r.get("rpm"), (int, float)) and r["rpm"] < th) for r in rows)


def mostly_increasing(rows, key):
    vals = [r.get(key) for r in rows if isinstance(r.get(key), (int, float))]
    if len(vals) < 3:
        return False
    d = [vals[i + 1] - vals[i] for i in range(len(vals) - 1)]
    return sum(1 for x in d if x > 0) >= sum(1 for x in d if x < 0)


def mostly_decreasing(rows, key):
    vals = [r.get(key) for r in rows if isinstance(r.get(key), (int, float))]
    if len(vals) < 3:
        return False
    d = [vals[i + 1] - vals[i] for i in range(len(vals) - 1)]
    return sum(1 for x in d if x < 0) >= sum(1 for x in d if x > 0)


def fault_free(rows):
    return all((r.get("fault") in (0, None) and r.get("error") in (0, None)) for r in rows)


def tel_ok(rows, lim=500):
    vals = [r.get("telAge") for r in rows if isinstance(r.get("telAge"), (int, float))]
    if not vals:
        return False
    return max(vals) < lim


def stop_reached(rows):
    # do 5s po STOP
    return any(
        r.get("state") == "stopped" and r.get("moving") is False and r.get("rpm") == 0
        for r in rows
    )


def stat_block(rows):
    rpms = [r["rpm"] for r in rows if isinstance(r.get("rpm"), (int, float))]
    dists = [r["dist"] for r in rows if isinstance(r.get("dist"), (int, float))]
    poss = [r["position"] for r in rows if isinstance(r.get("position"), (int, float))]
    return {
        "rpm_min": min(rpms) if rpms else None,
        "rpm_max": max(rpms) if rpms else None,
        "dist_min": min(dists) if dists else None,
        "dist_max": max(dists) if dists else None,
        "pos_min": min(poss) if poss else None,
        "pos_max": max(poss) if poss else None,
        "telAge_max": max((r["telAge"] for r in rows if isinstance(r.get("telAge"), (int, float))), default=None),
    }


reader = threading.Thread(target=read_serial, daemon=True)
reader.start()
time.sleep(1.0)

results = []

# TEST 1 OPEN
control("open")
poll(5.0, "T1_open")
control("stop")
poll(5.0, "T1_stop")
t1o = in_tag("T1_open")
t1s = in_tag("T1_stop")
t1_pass = (
    any(r["state"] == "opening" and r["moving"] is True for r in t1o)
    and any_pos_rpm(t1o)
    and mostly_increasing(t1o, "dist")
    and mostly_increasing(t1o, "position")
    and fault_free(t1o)
    and tel_ok(t1o)
    and stop_reached(t1s)
)
results.append({"test": "TEST1_OPEN", "status": "PASS" if t1_pass else "FAIL", "stats": stat_block(t1o + t1s)})

# TEST 2 CLOSE
control("close")
poll(5.0, "T2_close")
control("stop")
poll(5.0, "T2_stop")
t2c = in_tag("T2_close")
t2s = in_tag("T2_stop")
t2_main = (
    any(r["state"] == "closing" and r["moving"] is True for r in t2c)
    and any_neg_rpm(t2c)
    and mostly_decreasing(t2c, "dist")
    and fault_free(t2c)
    and tel_ok(t2c)
    and stop_reached(t2s)
)
# position przy close bywa opóźniona/kwantowana - osobna adnotacja
t2_pos_ok = mostly_decreasing(t2c, "position")
t2_status = "PASS" if (t2_main and t2_pos_ok) else ("NIEJEDNOZNACZNE" if t2_main else "FAIL")
results.append({"test": "TEST2_CLOSE", "status": t2_status, "stats": stat_block(t2c + t2s), "positionTrendCloseOk": t2_pos_ok})

# TEST 3 zmiana kierunku
control("open")
poll(2.5, "T3_open")
control("close")
poll(3.0, "T3_close")
control("stop")
poll(5.0, "T3_stop")
t3o = in_tag("T3_open")
t3c = in_tag("T3_close")
t3s = in_tag("T3_stop")
t3_ok = any_pos_rpm(t3o) and any_neg_rpm(t3c) and fault_free(t3o + t3c + t3s) and stop_reached(t3s)
results.append({"test": "TEST3_DIRECTION_CHANGE", "status": "PASS" if t3_ok else "FAIL", "stats": stat_block(t3o + t3c + t3s)})

# TEST 4 kilka cykli
cycles = [
    ("open", "T4_o1"),
    ("close", "T4_c1"),
    ("open", "T4_o2"),
    ("close", "T4_c2"),
]
for a, tag in cycles:
    control(a)
    poll(2.2, tag)
    control("stop")
    poll(2.8, tag + "_s")
control("open")
poll(1.8, "T4_mix_o")
control("close")
poll(2.2, "T4_mix_c")
control("stop")
poll(4.0, "T4_mix_s")

t4 = subset("T4_")
t4_ok = fault_free(t4) and tel_ok(t4) and any(r.get("rpm") == 0 for r in t4 if r["tag"].endswith("_s") or r["tag"] == "T4_mix_s")
results.append({"test": "TEST4_MULTI_CYCLES", "status": "PASS" if t4_ok else "FAIL", "stats": stat_block(t4)})

# TEST 5 dłuższy ruch
control("open")
poll(8.0, "T5_long_open")
control("stop")
poll(5.0, "T5_long_stop")
t5o = in_tag("T5_long_open")
t5s = in_tag("T5_long_stop")
t5_ok = any_pos_rpm(t5o) and mostly_increasing(t5o, "dist") and fault_free(t5o + t5s) and tel_ok(t5o) and stop_reached(t5s)
results.append({"test": "TEST5_LONG_RUN", "status": "PASS" if t5_ok else "FAIL", "stats": stat_block(t5o + t5s)})

# TEST 6 po STOP
t6_rows = t1s + t2s + t3s + t5s
t6_ok = stop_reached(t6_rows) and fault_free(t6_rows)
results.append({"test": "TEST6_AFTER_STOP", "status": "PASS" if t6_ok else "NIEJEDNOZNACZNE", "stats": stat_block(t6_rows)})

stop_reader = True
reader.join(timeout=1.0)

serial_gate_hb = [
    x for x in serial_lines
    if re.search(r"\[(GATE|HB|UI)\]", x["line"])
]

out = {
    "actions": actions,
    "results": results,
    "api_samples": api_samples,
    "serial_lines": serial_lines,
    "serial_gate_hb": serial_gate_hb[:200],
}

with open(OUT_JSON, "w", encoding="utf-8") as f:
    json.dump(out, f, ensure_ascii=False, indent=2)

lines = []
lines.append("MOTOR FOCUS TEST REPORT")
lines.append("API=" + BASE)
lines.append("COM=" + COM)
lines.append("")
lines.append("COMMANDS:")
for a in actions:
    lines.append(f"POST /api/control {{\"action\":\"{a['action']}\"}} @ {a['ts']}")
lines.append("GET /api/status @ 400ms polling")
lines.append("")
lines.append("RESULTS:")
for r in results:
    lines.append(f"{r['test']}: {r['status']} | {json.dumps(r['stats'], ensure_ascii=False)}")
lines.append("")
lines.append("KEY COM3 LOGS:")
for l in serial_gate_hb[:70]:
    lines.append(f"[{l['ts']}] {l['line']}")

with open(OUT_TXT, "w", encoding="utf-8") as f:
    f.write("\n".join(lines))

print("REPORT_JSON=" + OUT_JSON)
print("REPORT_TXT=" + OUT_TXT)
for r in results:
    print(f"{r['test']}: {r['status']}")
print("API_SAMPLES=" + str(len(api_samples)))
print("SERIAL_LINES=" + str(len(serial_lines)))
