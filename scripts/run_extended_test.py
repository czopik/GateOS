import time
import threading
import json
import re
import requests
import serial

BASE = "http://CHANGE_DEVICE_IP"
COM = "COM3"
BAUD = 115200
OUT_JSON = "scripts/extended_test_results.json"
OUT_TXT = "scripts/extended_test_report.txt"

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
            try:
                raw = ser.readline()
                if not raw:
                    continue
                line = raw.decode("utf-8", "ignore").strip()
                if not line:
                    continue
                if "LD2410" in line or "ld2410" in line:
                    continue
                serial_lines.append({"ts": now_ms(), "line": line})
            except Exception:
                pass
    finally:
        ser.close()


def get_status(tag=""):
    s = requests.get(BASE + "/api/status", timeout=3).json()
    sample = {
        "ts": now_ms(),
        "tag": tag,
        "state": s.get("gate", {}).get("state"),
        "moving": s.get("gate", {}).get("moving"),
        "position": s.get("gate", {}).get("position"),
        "positionPercent": s.get("gate", {}).get("positionPercent"),
        "targetPosition": s.get("gate", {}).get("targetPosition"),
        "maxDistance": s.get("gate", {}).get("maxDistance"),
        "stopReason": s.get("gate", {}).get("stopReason"),
        "errorCode": s.get("gate", {}).get("errorCode"),
        "rpm": s.get("hb", {}).get("rpm"),
        "dist_mm": s.get("hb", {}).get("dist_mm"),
        "fault": s.get("hb", {}).get("fault"),
        "telAgeMs": s.get("hb", {}).get("telAgeMs"),
        "lastTelMs": s.get("hb", {}).get("lastTelMs"),
    }
    api_samples.append(sample)
    return sample


def control(action):
    actions.append({"ts": now_ms(), "action": action})
    requests.post(BASE + "/api/control", json={"action": action}, timeout=3)


def sample_for(sec, tag):
    end = time.time() + sec
    while time.time() < end:
        get_status(tag)
        time.sleep(0.4)


def rpm_sign_ok(samples, expect):
    vals = [x["rpm"] for x in samples if isinstance(x.get("rpm"), (int, float))]
    if not vals:
        return False
    if expect == "pos":
        return any(v > 20 for v in vals)
    if expect == "neg":
        return any(v < -20 for v in vals)
    return True


def monotonic(samples, key, dirn):
    vals = [x.get(key) for x in samples if isinstance(x.get(key), (int, float))]
    if len(vals) < 3:
        return False
    diffs = [vals[i + 1] - vals[i] for i in range(len(vals) - 1)]
    if dirn == "up":
        return sum(1 for d in diffs if d > 0) > sum(1 for d in diffs if d < 0)
    if dirn == "down":
        return sum(1 for d in diffs if d < 0) > sum(1 for d in diffs if d > 0)
    return False


def check_no_fault(samples):
    return all((x.get("fault") in (0, None) and x.get("errorCode") in (0, None)) for x in samples)


def final_stopped(samples):
    tail = samples[-4:] if len(samples) >= 4 else samples
    has_state = any(x.get("state") == "stopped" for x in tail)
    has_moving = any(x.get("moving") is False for x in tail)
    has_rpm0 = any((x.get("rpm") == 0) for x in tail if x.get("rpm") is not None)
    return has_state and has_moving and has_rpm0


reader = threading.Thread(target=read_serial, daemon=True)
reader.start()
time.sleep(1.0)

results = []

# TEST 1
t1 = [get_status("T1_idle") for _ in range(3)]
time.sleep(0.4)
results.append(
    {
        "test": "TEST1_IDLE",
        "pass": (
            t1[-1]["state"] == "stopped"
            and t1[-1]["moving"] is False
            and t1[-1]["rpm"] == 0
            and t1[-1]["fault"] == 0
            and t1[-1]["errorCode"] == 0
        ),
        "data": t1,
    }
)

# TEST 2
control("open")
sample_for(4.0, "T2_open")
control("stop")
sample_for(2.4, "T2_stop")
t2 = [x for x in api_samples if x["tag"] in ("T2_open", "T2_stop")]
open_part = [x for x in t2 if x["tag"] == "T2_open"]
stop_part = [x for x in t2 if x["tag"] == "T2_stop"]
results.append(
    {
        "test": "TEST2_OPEN_STOP",
        "pass": (
            any(x["state"] == "opening" for x in open_part)
            and any(x["moving"] is True for x in open_part)
            and rpm_sign_ok(open_part, "pos")
            and monotonic(open_part, "dist_mm", "up")
            and monotonic(open_part, "position", "up")
            and check_no_fault(open_part)
            and final_stopped(stop_part)
        ),
        "data": t2,
    }
)

# TEST 3
control("close")
sample_for(4.0, "T3_close")
control("stop")
sample_for(2.4, "T3_stop")
t3 = [x for x in api_samples if x["tag"] in ("T3_close", "T3_stop")]
close_part = [x for x in t3 if x["tag"] == "T3_close"]
stop_part = [x for x in t3 if x["tag"] == "T3_stop"]
results.append(
    {
        "test": "TEST3_CLOSE_STOP",
        "pass": (
            any(x["state"] == "closing" for x in close_part)
            and any(x["moving"] is True for x in close_part)
            and rpm_sign_ok(close_part, "neg")
            and monotonic(close_part, "dist_mm", "down")
            and monotonic(close_part, "position", "down")
            and check_no_fault(close_part)
            and final_stopped(stop_part)
        ),
        "data": t3,
    }
)

# TEST 4
control("open")
sample_for(1.6, "T4_open")
control("close")
sample_for(1.6, "T4_close")
control("stop")
sample_for(1.8, "T4_stop")
t4 = [x for x in api_samples if x["tag"].startswith("T4_")]
results.append(
    {
        "test": "TEST4_QUICK_REVERSE",
        "pass": (
            rpm_sign_ok([x for x in t4 if x["tag"] == "T4_open"], "pos")
            and rpm_sign_ok([x for x in t4 if x["tag"] == "T4_close"], "neg")
            and check_no_fault(t4)
            and final_stopped([x for x in t4 if x["tag"] == "T4_stop"])
        ),
        "data": t4,
    }
)

# TEST 5
for a, b in [("open", "T5_o1"), ("close", "T5_c1"), ("open", "T5_o2"), ("close", "T5_c2")]:
    control(a)
    sample_for(1.2, b)
    control("stop")
    sample_for(1.1, b + "_s")

t5 = [x for x in api_samples if x["tag"].startswith("T5_")]
results.append(
    {
        "test": "TEST5_MULTI_START_STOP",
        "pass": (
            check_no_fault(t5)
            and any(x["moving"] is True for x in t5)
            and any(x["moving"] is False for x in t5 if x["tag"].endswith("_s"))
        ),
        "data": t5,
    }
)

# TEST 8
control("open")
sample_for(6.0, "T8_long_open")
control("stop")
sample_for(2.0, "T8_long_stop")
t8 = [x for x in api_samples if x["tag"].startswith("T8_")]
open8 = [x for x in t8 if x["tag"] == "T8_long_open"]
results.append(
    {
        "test": "TEST8_LONG_MOTION",
        "pass": (
            any(x["state"] == "opening" for x in open8)
            and rpm_sign_ok(open8, "pos")
            and monotonic(open8, "dist_mm", "up")
            and check_no_fault(t8)
            and final_stopped([x for x in t8 if x["tag"] == "T8_long_stop"])
            and all((x.get("telAgeMs") is None or x.get("telAgeMs") < 500) for x in open8)
        ),
        "data": t8,
    }
)

# TEST 6 derived
serial_kw = [
    l
    for l in serial_lines
    if re.search(r"(open|close|stop|rpm|tel|fault|error|state|hover|command)", l["line"], re.I)
]
api_ref = []
for tag in ["T2_open", "T3_close"]:
    subset = [x for x in api_samples if x["tag"] == tag]
    if subset:
        rpms = [x["rpm"] for x in subset if isinstance(x.get("rpm"), (int, float))]
        api_ref.append(
            {
                "tag": tag,
                "state_set": sorted(set(x["state"] for x in subset)),
                "moving_set": sorted(set(bool(x["moving"]) for x in subset)),
                "rpm_min": min(rpms) if rpms else None,
                "rpm_max": max(rpms) if rpms else None,
                "fault_set": sorted(set(x["fault"] for x in subset)),
            }
        )

t6_status = "PASS" if len(serial_kw) > 0 else "NIEJEDNOZNACZNE"
results.append(
    {
        "test": "TEST6_COM3_API_CONSISTENCY",
        "pass": (t6_status == "PASS"),
        "status": t6_status,
        "serialEvidenceCount": len(serial_kw),
        "apiRef": api_ref,
    }
)

# TEST 7 derived
stop_tags = ["T2_stop", "T3_stop", "T4_stop", "T8_long_stop"]
stops = [x for x in api_samples if x["tag"] in stop_tags]
t7 = final_stopped(stops) and check_no_fault(stops)
results.append({"test": "TEST7_AFTER_STOP_BEHAVIOR", "pass": t7, "data": stops[-20:]})

stop_reader = True
reader.join(timeout=1.0)

out = {
    "actions": actions,
    "results": results,
    "serial_lines_count": len(serial_lines),
    "serial_lines_sample": serial_lines[:120],
    "serial_keyword_sample": serial_kw[:120],
    "api_samples_count": len(api_samples),
}

with open(OUT_JSON, "w", encoding="utf-8") as f:
    json.dump(out, f, ensure_ascii=False, indent=2)

status_map = []
for r in results:
    if "status" in r:
        st = r["status"]
    else:
        st = "PASS" if r.get("pass") else "FAIL"
    status_map.append((r["test"], st))

lines = []
lines.append("EXTENDED FUNCTIONAL TEST REPORT")
lines.append("API base: " + BASE)
lines.append("Serial: " + COM + " @ " + str(BAUD))
lines.append("")
lines.append("Commands used:")
for a in actions:
    lines.append(f"POST /api/control {{\"action\":\"{a['action']}\"}} @ {a['ts']}")
lines.append("GET /api/status polled every 400ms during tests")
lines.append("")
lines.append("Results:")
for t, s in status_map:
    lines.append(f"{t}: {s}")
lines.append("")
lines.append("Key COM3 log excerpts (LD2410 filtered):")
for e in serial_kw[:40]:
    lines.append(f"[{e['ts']}] {e['line']}")
lines.append("")
lines.append("API reference (OPEN/CLOSE):")
for a in api_ref:
    lines.append(json.dumps(a, ensure_ascii=False))

with open(OUT_TXT, "w", encoding="utf-8") as f:
    f.write("\n".join(lines))

print("REPORT_JSON=" + OUT_JSON)
print("REPORT_TXT=" + OUT_TXT)
for t, s in status_map:
    print(t + ": " + s)
print("SERIAL_LINES=" + str(len(serial_lines)))
print("SERIAL_KEYWORD_LINES=" + str(len(serial_kw)))
print("API_SAMPLES=" + str(len(api_samples)))
