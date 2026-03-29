import json
import time
import urllib.request

HOST = "http://192.168.1.124"
REPORT = "D:/hoversilnikesp/esp32/scripts/motor_roundtrip_test.json"
STATUS_PATH = "/api/gate"
CONTROL_PATH = "/api/control"


def get_json(path: str):
    with urllib.request.urlopen(HOST + path, timeout=5) as r:
        return json.loads(r.read().decode("utf-8"))


def post_json(path: str, payload: dict):
    req = urllib.request.Request(
        HOST + path,
        data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=5) as r:
        raw = r.read().decode("utf-8")
    return json.loads(raw) if raw else {}


result = {"ok": False, "steps": [], "metric": {}}

try:
    s0 = get_json(STATUS_PATH)
    p0 = s0.get("position")
    result["steps"].append({"name": "start", "position": p0, "statusPath": STATUS_PATH})

    result["steps"].append({"name": "open_cmd", "resp": post_json(CONTROL_PATH, {"action": "open"}), "controlPath": CONTROL_PATH})
    time.sleep(2.5)

    s_mid = get_json(STATUS_PATH)
    p_mid = s_mid.get("position")
    result["steps"].append({"name": "after_open", "position": p_mid, "statusPath": STATUS_PATH})

    result["steps"].append({"name": "stop_1", "resp": post_json(CONTROL_PATH, {"action": "stop"}), "controlPath": CONTROL_PATH})
    time.sleep(0.8)

    result["steps"].append({"name": "close_cmd", "resp": post_json(CONTROL_PATH, {"action": "close"}), "controlPath": CONTROL_PATH})
    time.sleep(2.5)

    result["steps"].append({"name": "stop_2", "resp": post_json(CONTROL_PATH, {"action": "stop"}), "controlPath": CONTROL_PATH})
    time.sleep(1.0)

    s1 = get_json(STATUS_PATH)
    p1 = s1.get("position")
    result["steps"].append({"name": "end", "position": p1})

    if isinstance(p0, (int, float)) and isinstance(p1, (int, float)):
        diff = abs(float(p1) - float(p0))
        result["metric"] = {
            "start": float(p0),
            "end": float(p1),
            "abs_diff": diff,
            "tolerance": 0.05,
            "pass": diff <= 0.05,
        }
        result["ok"] = True
    else:
        result["metric"] = {"error": "non_numeric_position", "start": p0, "end": p1}
except Exception as e:
    result["error"] = str(e)

with open(REPORT, "w", encoding="utf-8") as f:
    json.dump(result, f, indent=2)

print("REPORT", REPORT)
