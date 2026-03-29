import time
import threading
import json
import re
import requests
import serial

BASE = "http://192.168.1.43"
COM = "COM3"
BAUD = 115200

OUT_JSON = "scripts/position_accuracy_results.json"
OUT_TXT = "scripts/position_accuracy_report.txt"

POLL_DT = 0.10
STOP_RPM_DB = 12
MOVE_TIMEOUT_S = 25.0
CRITERION_AVG_MM = 10.0
CRITERION_MAX_MM = 20.0

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
            if "LD2410" in line or "ld2410" in line:
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
        "target": s.get("gate", {}).get("targetPosition"),
        "stopReason": s.get("gate", {}).get("stopReason"),
        "error": s.get("gate", {}).get("errorCode"),
        "maxDistance": s.get("gate", {}).get("maxDistance"),
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
    rows = []
    while time.time() < end:
        rows.append(status(tag))
        time.sleep(POLL_DT)
    return rows


def recover_if_error(tag):
    r = status(tag + "_chk")
    if r.get("state") == "error" or (isinstance(r.get("error"), (int, float)) and r.get("error") not in (0, None)):
        control("reset")
        poll(1.0, tag + "_reset")
        control("stop")
        poll(0.6, tag + "_stop")


def get_position():
    r = status("POS")
    p = r.get("position")
    return float(p) if isinstance(p, (int, float)) else 0.0


def get_max_distance():
    r = status("MAX")
    m = r.get("maxDistance")
    return float(m) if isinstance(m, (int, float)) else 0.0


def wait_move_done(tag, timeout_s=MOVE_TIMEOUT_S):
    end = time.time() + timeout_s
    rows = []
    while time.time() < end:
        r = status(tag)
        rows.append(r)
        rpm = r.get("rpm")
        if r.get("state") == "error":
            break
        if r.get("moving") is False and r.get("state") == "stopped" and isinstance(rpm, (int, float)) and abs(rpm) <= STOP_RPM_DB:
            break
        time.sleep(POLL_DT)
    return rows


def goto_target(target_m, tag):
    recover_if_error(tag + "_pre")
    control(f"goto:{target_m:.3f}")
    rows = wait_move_done(tag + "_move")
    final = rows[-1] if rows else status(tag + "_final")
    p = final.get("position")
    err = None
    if isinstance(p, (int, float)):
        err = abs((p - target_m) * 1000.0)
    return {
        "target_m": target_m,
        "final_position_m": p,
        "error_mm": err,
        "final_state": final.get("state"),
        "final_moving": final.get("moving"),
        "final_rpm": final.get("rpm"),
        "final_stopReason": final.get("stopReason"),
    }


def stats(vals):
    vv = [v for v in vals if isinstance(v, (int, float))]
    if not vv:
        return {"count": 0, "min": None, "max": None, "avg": None}
    return {
        "count": len(vv),
        "min": min(vv),
        "max": max(vv),
        "avg": sum(vv) / len(vv),
    }


reader = threading.Thread(target=read_serial, daemon=True)
reader.start()
time.sleep(1.0)

results = []

recover_if_error("BOOT")
control("stop")
poll(0.8, "SETTLE")

# TEST A: P0 -> P1 -> P0 (10 cycles)
max_d = get_max_distance()
p0 = get_position()
span = 0.35
if max_d > 0.8:
    p1 = p0 + span
    if p1 > max_d - 0.15:
        p1 = p0 - span
else:
    p1 = p0 + span
if p1 < 0.15:
    p1 = 0.15

cycles_a = []
for i in range(10):
    r1 = goto_target(p1, f"A{i+1}_to_p1")
    r0 = goto_target(p0, f"A{i+1}_back_p0")
    cycles_a.append({
        "cycle": i + 1,
        "P0_start_m": p0,
        "P1_m": p1,
        "at_P1": r1,
        "back_to_P0": r0,
        "return_error_mm": r0.get("error_mm"),
    })

errs_a = [c.get("return_error_mm") for c in cycles_a]
sum_a = stats(errs_a)
results.append({
    "test": "TEST_A_REPEAT_P0_P1",
    "status": "PASS" if (sum_a["avg"] is not None and sum_a["avg"] <= CRITERION_AVG_MM and sum_a["max"] <= CRITERION_MAX_MM) else "FAIL",
    "P0_m": p0,
    "P1_m": p1,
    "summary": sum_a,
    "cycles": cycles_a,
})

# TEST B: fixed targets, repeated
base_targets = [0.50, 1.00, 1.50, 2.00, 2.50]
if max_d > 0.0:
    targets = [t for t in base_targets if t <= max_d - 0.05]
else:
    targets = base_targets

rows_b = []
for t in targets:
    for k in range(3):
        rows_b.append({
            "target_m": t,
            "run": k + 1,
            "result": goto_target(t, f"B_t{int(t*1000)}_r{k+1}"),
        })

errs_b = [r["result"].get("error_mm") for r in rows_b]
sum_b = stats(errs_b)
results.append({
    "test": "TEST_B_FIXED_TARGETS",
    "status": "PASS" if (sum_b["avg"] is not None and sum_b["avg"] <= CRITERION_AVG_MM and sum_b["max"] <= CRITERION_MAX_MM) else "FAIL",
    "targets_m": targets,
    "summary": sum_b,
    "rows": rows_b,
})

# TEST C: same target from both directions
if max_d > 0.0:
    target_c = min(1.5, max(0.5, max_d * 0.5))
    low_side = max(0.15, target_c - 0.45)
    high_side = min(max_d - 0.15, target_c + 0.45)
else:
    target_c = 1.5
    low_side = 0.5
    high_side = 2.2

rows_c = []
for i in range(5):
    goto_target(low_side, f"C{i+1}_prep_low")
    from_open = goto_target(target_c, f"C{i+1}_from_open")
    rows_c.append({"cycle": i + 1, "from": "open_side", "target_m": target_c, "result": from_open})

    goto_target(high_side, f"C{i+1}_prep_high")
    from_close = goto_target(target_c, f"C{i+1}_from_close")
    rows_c.append({"cycle": i + 1, "from": "close_side", "target_m": target_c, "result": from_close})

errs_c_open = [r["result"].get("error_mm") for r in rows_c if r["from"] == "open_side"]
errs_c_close = [r["result"].get("error_mm") for r in rows_c if r["from"] == "close_side"]
sum_c_open = stats(errs_c_open)
sum_c_close = stats(errs_c_close)
status_c = "PASS" if (
    sum_c_open["avg"] is not None and sum_c_close["avg"] is not None
    and sum_c_open["avg"] <= CRITERION_AVG_MM and sum_c_close["avg"] <= CRITERION_AVG_MM
    and sum_c_open["max"] <= CRITERION_MAX_MM and sum_c_close["max"] <= CRITERION_MAX_MM
) else "FAIL"

results.append({
    "test": "TEST_C_BIDIRECTIONAL_TARGET",
    "status": status_c,
    "target_m": target_c,
    "open_side": sum_c_open,
    "close_side": sum_c_close,
    "rows": rows_c,
})

all_errs = []
for r in results:
    if r["test"] == "TEST_A_REPEAT_P0_P1":
        all_errs.extend([c.get("return_error_mm") for c in r["cycles"]])
    elif r["test"] == "TEST_B_FIXED_TARGETS":
        all_errs.extend([x["result"].get("error_mm") for x in r["rows"]])
    elif r["test"] == "TEST_C_BIDIRECTIONAL_TARGET":
        all_errs.extend([x["result"].get("error_mm") for x in r["rows"]])

overall = stats(all_errs)
overall_status = "PASS" if (overall["avg"] is not None and overall["avg"] <= CRITERION_AVG_MM and overall["max"] <= CRITERION_MAX_MM) else "FAIL"

stop_reader = True
reader.join(timeout=1.0)

serial_focus = [
    x for x in serial_lines
    if re.search(r"\[(GATE|HB|UI|MOTOR)\]", x["line"]) and ("LD2410" not in x["line"]) and ("ld2410" not in x["line"])
]

out = {
    "criteria": {"avg_mm": CRITERION_AVG_MM, "max_mm": CRITERION_MAX_MM},
    "actions": actions,
    "results": results,
    "overall": overall,
    "overall_status": overall_status,
    "api_samples": api_samples,
    "serial_focus": serial_focus[:600],
}

with open(OUT_JSON, "w", encoding="utf-8") as f:
    json.dump(out, f, ensure_ascii=False, indent=2)

lines = []
lines.append("POSITION ACCURACY TEST REPORT")
lines.append("API=" + BASE)
lines.append("COM=" + COM)
lines.append("")
lines.append("CRITERIA: avg<=%.1fmm, max<=%.1fmm" % (CRITERION_AVG_MM, CRITERION_MAX_MM))
lines.append("")
for r in results:
    lines.append(f"{r['test']}: {r['status']}")
    if r["test"] == "TEST_A_REPEAT_P0_P1":
        lines.append("  summary=" + json.dumps(r["summary"], ensure_ascii=False))
    if r["test"] == "TEST_B_FIXED_TARGETS":
        lines.append("  summary=" + json.dumps(r["summary"], ensure_ascii=False))
    if r["test"] == "TEST_C_BIDIRECTIONAL_TARGET":
        lines.append("  open_side=" + json.dumps(r["open_side"], ensure_ascii=False))
        lines.append("  close_side=" + json.dumps(r["close_side"], ensure_ascii=False))
lines.append("")
lines.append("OVERALL: %s %s" % (overall_status, json.dumps(overall, ensure_ascii=False)))
lines.append("")
lines.append("KEY COM3 LOGS:")
for l in serial_focus[:200]:
    lines.append(f"[{l['ts']}] {l['line']}")

with open(OUT_TXT, "w", encoding="utf-8") as f:
    f.write("\n".join(lines))

print("REPORT_JSON=" + OUT_JSON)
print("REPORT_TXT=" + OUT_TXT)
for r in results:
    print(f"{r['test']}: {r['status']}")
print("OVERALL_STATUS=" + overall_status)
print("OVERALL=" + json.dumps(overall, ensure_ascii=False))
print("API_SAMPLES=" + str(len(api_samples)))
print("SERIAL_LINES=" + str(len(serial_lines)))
