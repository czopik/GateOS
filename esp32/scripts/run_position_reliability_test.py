import time
import threading
import json
import random
import math
import re
import requests
import serial

BASE = "http://192.168.1.43"
COM = "COM3"
BAUD = 115200

OUT_JSON = "scripts/position_reliability_results.json"
OUT_TXT = "scripts/position_reliability_report.txt"

POLL_DT = 0.10
MOVE_TIMEOUT_S = 28.0
STOP_RPM_DB = 12
RANDOM_CYCLES = 20
REBOOT_COUNT = 3

serial_lines = []
api_rows = []
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


def req_json(path, timeout=3):
    return requests.get(BASE + path, timeout=timeout).json()


def snapshot(tag):
    s = req_json("/api/status", timeout=3)
    d = {}
    try:
        d = req_json("/api/debug", timeout=3)
    except Exception:
        d = {}

    gate = s.get("gate", {})
    hb = s.get("hb", {})
    dbg_hb = d.get("hover", {}) if isinstance(d.get("hover", {}), dict) else {}
    dbg_pos = d.get("position", {}) if isinstance(d.get("position", {}), dict) else {}
    dbg_in = d.get("inputs", {}) if isinstance(d.get("inputs", {}), dict) else {}

    rec = {
        "ts": now_ms(),
        "tag": tag,
        "state": gate.get("state"),
        "moving": gate.get("moving"),
        "position": gate.get("position"),
        "target": gate.get("targetPosition"),
        "maxDistance": gate.get("maxDistance"),
        "stopReason": gate.get("stopReason"),
        "error": gate.get("errorCode"),
        "rpm": hb.get("rpm"),
        "dist_mm": hb.get("dist_mm"),
        "telAge": hb.get("telAgeMs"),
        "fault": hb.get("fault"),
        # additional channels
        "dist_mm_raw": hb.get("dist_mm_raw", dbg_hb.get("dist_mm_raw")),
        "position_raw_m": dbg_pos.get("positionMetersRaw"),
        "position_filt_m": dbg_pos.get("positionMetersFiltered"),
        "limit_close_raw": dbg_in.get("limitCloseRaw"),
        "limit_open_raw": dbg_in.get("limitOpenRaw"),
    }
    api_rows.append(rec)
    return rec


def control(action):
    actions.append({"ts": now_ms(), "action": action})
    requests.post(BASE + "/api/control", json={"action": action}, timeout=4)


def poll(seconds, tag):
    end = time.time() + seconds
    rows = []
    while time.time() < end:
        rows.append(snapshot(tag))
        time.sleep(POLL_DT)
    return rows


def wait_until_idle(tag, timeout_s=MOVE_TIMEOUT_S):
    end = time.time() + timeout_s
    rows = []
    while time.time() < end:
        r = snapshot(tag)
        rows.append(r)
        if r.get("state") == "error":
            break
        rpm = r.get("rpm")
        if r.get("moving") is False and r.get("state") == "stopped" and isinstance(rpm, (int, float)) and abs(rpm) <= STOP_RPM_DB:
            break
        time.sleep(POLL_DT)
    return rows


def recover_if_error(tag):
    try:
        r = snapshot(tag + "_chk")
    except Exception:
        return
    if r.get("state") == "error" or (isinstance(r.get("error"), (int, float)) and r.get("error") not in (0, None)):
        try:
            control("reset")
            poll(1.0, tag + "_reset")
            control("stop")
            poll(0.6, tag + "_stop")
        except Exception:
            return


def get_pos():
    r = snapshot("POS")
    p = r.get("position")
    return float(p) if isinstance(p, (int, float)) else 0.0


def get_max_d():
    r = snapshot("MAX")
    m = r.get("maxDistance")
    return float(m) if isinstance(m, (int, float)) else 0.0


def mm_err(a_m, b_m):
    return abs((a_m - b_m) * 1000.0)


def mm_err_from_mm(mm_val, target_m):
    if not isinstance(mm_val, (int, float)):
        return None
    return abs((float(mm_val) / 1000.0 - target_m) * 1000.0)


def goto_target(target_m, tag):
    recover_if_error(tag + "_pre")
    control(f"goto:{target_m:.3f}")
    rows = wait_until_idle(tag + "_move")
    f = rows[-1] if rows else snapshot(tag + "_final")
    pos = f.get("position")
    e_gate = mm_err(pos, target_m) if isinstance(pos, (int, float)) else None
    e_hb = mm_err_from_mm(f.get("dist_mm"), target_m)
    e_hb_raw = mm_err_from_mm(f.get("dist_mm_raw"), target_m) if isinstance(f.get("dist_mm_raw"), (int, float)) else None
    return {
        "target_m": target_m,
        "final": f,
        "err_gate_mm": e_gate,
        "err_hb_mm": e_hb,
        "err_hb_raw_mm": e_hb_raw,
    }


def calc_stats(vals):
    x = [float(v) for v in vals if isinstance(v, (int, float))]
    if not x:
        return {"count": 0, "min": None, "max": None, "avg": None, "stddev": None}
    avg = sum(x) / len(x)
    var = sum((v - avg) ** 2 for v in x) / len(x)
    return {
        "count": len(x),
        "min": min(x),
        "max": max(x),
        "avg": avg,
        "stddev": math.sqrt(var),
    }


def wait_api_back(timeout_s=60):
    end = time.time() + timeout_s
    while time.time() < end:
        try:
            _ = req_json("/api/status", timeout=2)
            return True
        except Exception:
            time.sleep(1.0)
    return False


random.seed(42)
reader = threading.Thread(target=read_serial, daemon=True)
reader.start()
time.sleep(1.0)

results = []
notes = []

recover_if_error("BOOT")
control("stop")
poll(1.0, "SETTLE")

max_d = get_max_d()
if max_d <= 0.8:
    notes.append("maxDistance<=0.8m; test random targets może być ograniczony.")

# Random stress test: >=20 cycles, random targets, both directions, random start distance, short pauses
rand_cycles = []
low = 0.15
high = max(0.20, max_d - 0.15) if max_d > 0 else 2.5
for i in range(RANDOM_CYCLES):
    side = "low" if (i % 2 == 0) else "high"
    start_t = low if side == "low" else high
    gt_start = goto_target(start_t, f"R{i+1}_to_start_{side}")

    t = random.uniform(low + 0.05, high - 0.05) if high - low > 0.12 else low
    gt = goto_target(t, f"R{i+1}_to_rand")

    rand_cycles.append({
        "cycle": i + 1,
        "start_side": side,
        "start_target": start_t,
        "start_result": gt_start,
        "target": t,
        "result": gt,
    })
    # short dwell
    poll(0.25, f"R{i+1}_dwell")

errs_gate_rand = [c["result"].get("err_gate_mm") for c in rand_cycles]
errs_hb_rand = [c["result"].get("err_hb_mm") for c in rand_cycles]
errs_hb_raw_rand = [c["result"].get("err_hb_raw_mm") for c in rand_cycles]

results.append({
    "test": "RANDOM_20_CYCLES",
    "status": "DONE",
    "gate_stats_mm": calc_stats(errs_gate_rand),
    "hb_stats_mm": calc_stats(errs_hb_rand),
    "hb_raw_stats_mm": calc_stats(errs_hb_raw_rand),
    "cycles": rand_cycles,
})

# Manual STOP disturbance then goto accuracy
manual_rows = []
for i in range(6):
    control("open")
    poll(0.6, f"M{i+1}_open")
    control("stop")
    wait_until_idle(f"M{i+1}_stop")
    t = random.uniform(low + 0.1, high - 0.1) if high - low > 0.25 else low
    g = goto_target(t, f"M{i+1}_goto")
    manual_rows.append({"step": i + 1, "target": t, "result": g})

errs_manual = [x["result"].get("err_gate_mm") for x in manual_rows]
results.append({
    "test": "AFTER_MANUAL_STOPS",
    "status": "DONE",
    "gate_stats_mm": calc_stats(errs_manual),
    "rows": manual_rows,
})

# Reboot consistency test
reboot_rows = []
for i in range(REBOOT_COUNT):
    try:
        requests.post(BASE + "/api/reboot", timeout=3)
    except Exception:
        pass
    ok = wait_api_back(timeout_s=80)
    if not ok:
        reboot_rows.append({"reboot": i + 1, "ok": False})
        continue
    time.sleep(2.0)
    recover_if_error(f"RB{i+1}")
    t = random.uniform(low + 0.1, high - 0.1) if high - low > 0.25 else low
    try:
        g = goto_target(t, f"RB{i+1}_goto")
        reboot_rows.append({"reboot": i + 1, "ok": True, "target": t, "result": g})
    except Exception as e:
        reboot_rows.append({"reboot": i + 1, "ok": False, "error": str(e)})

errs_reboot = [x["result"].get("err_gate_mm") for x in reboot_rows if x.get("ok") and isinstance(x.get("result"), dict)]
results.append({
    "test": "AFTER_REBOOTS",
    "status": "DONE",
    "gate_stats_mm": calc_stats(errs_reboot),
    "rows": reboot_rows,
})

# Physical reference (if limit close signal available): force close and verify close reference
ref_rows = []
for i in range(6):
    control("close")
    rows = wait_until_idle(f"REF{i+1}_close")
    f = rows[-1] if rows else snapshot(f"REF{i+1}_final")
    ref_rows.append({
        "run": i + 1,
        "position_m": f.get("position"),
        "dist_mm": f.get("dist_mm"),
        "limit_close_raw": f.get("limit_close_raw"),
        "state": f.get("state"),
        "stopReason": f.get("stopReason"),
    })
    poll(0.20, f"REF{i+1}_dwell")

ref_pos_errs = [abs(float(r.get("position_m", 0.0)) * 1000.0) for r in ref_rows if isinstance(r.get("position_m"), (int, float))]
results.append({
    "test": "PHYSICAL_REFERENCE_CLOSE",
    "status": "DONE",
    "close_zero_error_stats_mm": calc_stats(ref_pos_errs),
    "rows": ref_rows,
})

# Final consistency summary
all_gate_errs = []
for r in results:
    if r.get("test") == "RANDOM_20_CYCLES":
        all_gate_errs.extend([c["result"].get("err_gate_mm") for c in r["cycles"]])
    if r.get("test") == "AFTER_MANUAL_STOPS":
        all_gate_errs.extend([x["result"].get("err_gate_mm") for x in r["rows"]])
    if r.get("test") == "AFTER_REBOOTS":
        all_gate_errs.extend([x["result"].get("err_gate_mm") for x in r["rows"] if x.get("ok") and isinstance(x.get("result"), dict)])

summary = {
    "gate_all_stats_mm": calc_stats(all_gate_errs),
}

stop_reader = True
reader.join(timeout=1.0)

serial_focus = [
    x for x in serial_lines
    if re.search(r"\[(GATE|HB|UI|MOTOR)\]", x["line"])
]

out = {
    "notes": notes,
    "actions": actions,
    "results": results,
    "summary": summary,
    "api_rows": api_rows,
    "serial_focus": serial_focus[:900],
}

with open(OUT_JSON, "w", encoding="utf-8") as f:
    json.dump(out, f, ensure_ascii=False, indent=2)

lines = []
lines.append("POSITION RELIABILITY STRESS REPORT")
lines.append("API=" + BASE)
lines.append("COM=" + COM)
lines.append("")
if notes:
    lines.append("NOTES:")
    for n in notes:
        lines.append("- " + n)
    lines.append("")

for r in results:
    lines.append(f"{r['test']}: {r['status']}")
    if "gate_stats_mm" in r:
        lines.append("  gate_stats_mm=" + json.dumps(r["gate_stats_mm"], ensure_ascii=False))
    if "hb_stats_mm" in r:
        lines.append("  hb_stats_mm=" + json.dumps(r["hb_stats_mm"], ensure_ascii=False))
    if "hb_raw_stats_mm" in r:
        lines.append("  hb_raw_stats_mm=" + json.dumps(r["hb_raw_stats_mm"], ensure_ascii=False))
    if "close_zero_error_stats_mm" in r:
        lines.append("  close_zero_error_stats_mm=" + json.dumps(r["close_zero_error_stats_mm"], ensure_ascii=False))

lines.append("")
lines.append("SUMMARY:")
lines.append(json.dumps(summary, ensure_ascii=False))
lines.append("")
lines.append("KEY COM3 LOGS:")
for l in serial_focus[:220]:
    lines.append(f"[{l['ts']}] {l['line']}")

with open(OUT_TXT, "w", encoding="utf-8") as f:
    f.write("\n".join(lines))

print("REPORT_JSON=" + OUT_JSON)
print("REPORT_TXT=" + OUT_TXT)
for r in results:
    print(f"{r['test']}: {r['status']}")
print("SUMMARY=" + json.dumps(summary, ensure_ascii=False))
print("API_ROWS=" + str(len(api_rows)))
print("SERIAL_LINES=" + str(len(serial_lines)))
