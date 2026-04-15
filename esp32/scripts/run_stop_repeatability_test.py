import time
import threading
import json
import re
import requests
import serial

BASE = "http://192.168.1.43"
COM = "COM3"
BAUD = 115200

OUT_JSON = "scripts/stop_repeatability_results.json"
OUT_TXT = "scripts/stop_repeatability_report.txt"

POLL_DT = 0.20
STOP_RPM_DB = 15
STOP_TIMEOUT_S = 4.0
REPEAT_CYCLES = 5
REPEAT_TOL_MM = 20.0
OPEN_PULSE_S = 1.7

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
    rows = []
    while time.time() < end:
        rows.append(status(tag))
        time.sleep(POLL_DT)
    return rows


def poll_until_stopped(tag, timeout_s=STOP_TIMEOUT_S):
    end = time.time() + timeout_s
    rows = []
    while time.time() < end:
        r = status(tag)
        rows.append(r)
        rpm = r.get("rpm")
        if r.get("moving") is False and r.get("state") == "stopped" and isinstance(rpm, (int, float)) and abs(rpm) <= STOP_RPM_DB:
            break
        time.sleep(POLL_DT)
    return rows


def stop_metrics(rows):
    if not rows:
        return {"stopped": False, "t_to_stopped_ms": None, "t_to_rpm0_ms": None, "max_abs_rpm_after_stop": None, "final": None}
    t0 = rows[0]["ts"]
    t_st = None
    t_r0 = None
    max_abs = 0
    for r in rows:
        rpm = r.get("rpm")
        if isinstance(rpm, (int, float)):
            if abs(rpm) > max_abs:
                max_abs = abs(rpm)
            if t_r0 is None and abs(rpm) <= STOP_RPM_DB:
                t_r0 = r["ts"] - t0
        if t_st is None and r.get("moving") is False and r.get("state") == "stopped":
            t_st = r["ts"] - t0
    final = rows[-1]
    return {
        "stopped": (final.get("moving") is False and final.get("state") == "stopped"),
        "t_to_stopped_ms": t_st,
        "t_to_rpm0_ms": t_r0,
        "max_abs_rpm_after_stop": max_abs,
        "final": {
            "state": final.get("state"),
            "moving": final.get("moving"),
            "rpm": final.get("rpm"),
            "stopReason": final.get("stopReason"),
            "position": final.get("position"),
            "dist": final.get("dist"),
        },
    }


def run_stop_test(name, move_action, stop_action, pre_s=2.0):
    control(move_action)
    poll(pre_s, name + "_move")
    control(stop_action)
    rows = poll_until_stopped(name + "_after_stop")
    m = stop_metrics(rows)
    # Immediate = state/moving settles quickly + rpm decays quickly.
    immediate = (
        (m["t_to_stopped_ms"] is not None and m["t_to_stopped_ms"] <= 1200)
        and (m["t_to_rpm0_ms"] is not None and m["t_to_rpm0_ms"] <= 1200)
    )
    return {
        "test": name,
        "move": move_action,
        "stop": stop_action,
        "metrics": m,
        "status": "PASS" if immediate and m["stopped"] else "FAIL",
    }


def get_pos():
    r = status("POS")
    p = r.get("position")
    return float(p) if isinstance(p, (int, float)) else 0.0


def recover_if_error(tag):
    r = status(tag + "_chk")
    if r.get("state") == "error" or (isinstance(r.get("error"), (int, float)) and r.get("error") not in (0, None)):
        control("reset")
        poll(0.8, tag + "_reset")
        control("stop")
        poll(0.6, tag + "_stop")


def repeatability_test():
    cycles = []
    # settle
    recover_if_error("E_boot")
    control("stop")
    poll(1.5, "E_settle")

    for i in range(REPEAT_CYCLES):
        recover_if_error(f"E{i+1}_pre")
        p0 = get_pos()

        control("open")
        poll(OPEN_PULSE_S, f"E{i+1}_open")
        control("stop")
        poll_until_stopped(f"E{i+1}_stop1")
        p1 = get_pos()

        # Return to p0: close and stop when position <= p0
        control("close")
        t_end = time.time() + 12.0
        while time.time() < t_end:
            r = status(f"E{i+1}_return")
            if r.get("state") == "error":
                break
            pos = r.get("position")
            if isinstance(pos, (int, float)) and pos <= p0:
                control("stop")
                break
            time.sleep(POLL_DT)
        rows_ret = poll_until_stopped(f"E{i+1}_stop2")
        p2 = get_pos()

        err_mm = abs((p2 - p0) * 1000.0)
        cycles.append({
            "cycle": i + 1,
            "p0_m": p0,
            "p1_m": p1,
            "p2_m": p2,
            "return_error_mm": err_mm,
            "within_tolerance": err_mm <= REPEAT_TOL_MM,
            "return_stop_metrics": stop_metrics(rows_ret),
        })

    errs = [c["return_error_mm"] for c in cycles]
    summary = {
        "tolerance_mm": REPEAT_TOL_MM,
        "min_error_mm": min(errs) if errs else None,
        "max_error_mm": max(errs) if errs else None,
        "avg_error_mm": (sum(errs) / len(errs)) if errs else None,
        "all_within_tolerance": all(c["within_tolerance"] for c in cycles),
    }
    return cycles, summary


reader = threading.Thread(target=read_serial, daemon=True)
reader.start()
time.sleep(1.0)

results = []
notes = []

# TEST A: STOP z pilota podczas OPEN (symulacja: toggle -> ta sama ścieżka co pilot)
results.append(run_stop_test("TEST_A_REMOTE_STOP_OPEN", "open", "toggle", 2.0))

# TEST B: STOP z pilota podczas CLOSE (symulacja: toggle -> ta sama ścieżka co pilot)
results.append(run_stop_test("TEST_B_REMOTE_STOP_CLOSE", "close", "toggle", 2.0))

# TEST C: STOP z fotokomórki/obstacle
# Brak zdalnego API do bezpośredniego wywołania onObstacle().
# Najbliższa dostępna ścieżka logiczna bez fizycznego wejścia: /api/control stop (user stop).
results.append(run_stop_test("TEST_C_OBSTACLE_STOP_SIMULATED", "open", "stop", 2.0))
notes.append("TEST_C używa symulacji przez /api/control stop; bez fizycznego wejścia obstacle nie przechodzi przez onObstacle().")

# TEST D: porównanie stop normalny vs user vs obstacle(sim)
# normalny = dojazd do celu (open i czekanie), user = toggle, obstacle(sim)=stop
control("open")
rows_norm = poll(5.0, "TEST_D_normal_motion")
control("stop")
rows_norm_stop = poll_until_stopped("TEST_D_normal_stop")
norm_m = stop_metrics(rows_norm_stop)

rows_user = []
control("open")
poll(2.0, "TEST_D_user_move")
control("toggle")
rows_user = poll_until_stopped("TEST_D_user_stop")
user_m = stop_metrics(rows_user)

rows_obs = []
control("open")
poll(2.0, "TEST_D_obs_move")
control("stop")
rows_obs = poll_until_stopped("TEST_D_obs_stop")
obs_m = stop_metrics(rows_obs)

results.append({
    "test": "TEST_D_STOP_TYPE_COMPARE",
    "status": "PASS" if user_m["stopped"] and obs_m["stopped"] else "NIEJEDNOZNACZNE",
    "normal_stop": norm_m,
    "user_stop": user_m,
    "obstacle_stop_simulated": obs_m,
})

# TEST E: repeatability
cycles, rep_summary = repeatability_test()
results.append({
    "test": "TEST_E_REPEATABILITY",
    "status": "PASS" if rep_summary["all_within_tolerance"] else "FAIL",
    "summary": rep_summary,
    "cycles": cycles,
})

stop_reader = True
reader.join(timeout=1.0)

serial_gate_hb = [
    x for x in serial_lines
    if re.search(r"\[(GATE|HB|UI)\]", x["line"])
]

out = {
    "actions": actions,
    "results": results,
    "notes": notes,
    "api_samples": api_samples,
    "serial_gate_hb": serial_gate_hb[:400],
}

with open(OUT_JSON, "w", encoding="utf-8") as f:
    json.dump(out, f, ensure_ascii=False, indent=2)

lines = []
lines.append("STOP + REPEATABILITY TEST REPORT")
lines.append("API=" + BASE)
lines.append("COM=" + COM)
lines.append("")
lines.append("NOTES:")
for n in notes:
    lines.append("- " + n)
lines.append("")
lines.append("RESULTS:")
for r in results:
    lines.append(f"{r['test']}: {r['status']}")
    if "metrics" in r:
        lines.append("  " + json.dumps(r["metrics"], ensure_ascii=False))
    if r["test"] == "TEST_D_STOP_TYPE_COMPARE":
        lines.append("  normal=" + json.dumps(r["normal_stop"], ensure_ascii=False))
        lines.append("  user=" + json.dumps(r["user_stop"], ensure_ascii=False))
        lines.append("  obstacle(sim)=" + json.dumps(r["obstacle_stop_simulated"], ensure_ascii=False))
    if r["test"] == "TEST_E_REPEATABILITY":
        lines.append("  summary=" + json.dumps(r["summary"], ensure_ascii=False))
lines.append("")
lines.append("KEY COM3 LOGS:")
for l in serial_gate_hb[:120]:
    lines.append(f"[{l['ts']}] {l['line']}")

with open(OUT_TXT, "w", encoding="utf-8") as f:
    f.write("\n".join(lines))

print("REPORT_JSON=" + OUT_JSON)
print("REPORT_TXT=" + OUT_TXT)
for r in results:
    print(f"{r['test']}: {r['status']}")
print("API_SAMPLES=" + str(len(api_samples)))
print("SERIAL_LINES=" + str(len(serial_lines)))
