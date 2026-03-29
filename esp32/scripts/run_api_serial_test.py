#!/usr/bin/env python3
import argparse
import json
import sys
from urllib import request, error


def call(method: str, url: str, body=None):
    data = None
    headers = {}
    if body is not None:
      payload = json.dumps(body).encode("utf-8")
      data = payload
      headers["Content-Type"] = "application/json"
    req = request.Request(url, data=data, method=method, headers=headers)
    try:
      with request.urlopen(req, timeout=8) as r:
        raw = r.read().decode("utf-8", errors="replace")
        return r.status, raw
    except error.HTTPError as e:
      raw = e.read().decode("utf-8", errors="replace")
      return e.code, raw


def as_json(raw: str):
    try:
      return json.loads(raw)
    except Exception:
      return None


def must(cond: bool, msg: str, failures: list):
    if not cond:
      failures.append(msg)


def main() -> int:
    ap = argparse.ArgumentParser(description="Calibration API regression check")
    ap.add_argument("--host", default="CHANGE_DEVICE_IP")
    ap.add_argument("--port", type=int, default=80)
    ap.add_argument("--report", default="scripts/calibration_api_report.json")
    args = ap.parse_args()

    base = f"http://{args.host}:{args.port}"
    failures = []
    report = {"base": base, "steps": []}

    def step(name: str, method: str, path: str, body=None):
      code, raw = call(method, base + path, body)
      js = as_json(raw)
      report["steps"].append({"name": name, "code": code, "json": js, "raw": raw[:400]})
      return code, js, raw

    # status contract
    c, js, _ = step("manual_status_initial", "GET", "/api/calibration/manual/status")
    must(c == 200, "manual/status should return HTTP 200", failures)
    must(isinstance(js, dict), "manual/status should return JSON object", failures)
    if isinstance(js, dict):
      for key in ["enabled", "active", "step", "limitClose", "limitOpen", "positionMm", "travelMm", "error"]:
        must(key in js, f"manual/status missing field: {key}", failures)

    # start
    c, js, _ = step("manual_start", "POST", "/api/calibration/manual/start")
    must(c in (200, 409), "manual/start should return 200 or 409", failures)

    # status after start
    c, js, _ = step("manual_status_after_start", "GET", "/api/calibration/manual/status")
    must(c == 200, "manual/status after start should return HTTP 200", failures)
    if isinstance(js, dict):
      must(bool(js.get("active", False)) is True, "manual/status active should be true after start", failures)
      must(str(js.get("step", "")) != "idle", "manual/status step should progress after start", failures)

    # apply without completion must fail
    c, js, _ = step("manual_apply_without_ready", "POST", "/api/calibration/manual/apply")
    must(c >= 400, "manual/apply without ready should fail", failures)

    # cancel
    c, js, _ = step("manual_cancel", "POST", "/api/calibration/manual/cancel")
    must(c == 200, "manual/cancel should return 200", failures)

    # status after cancel
    c, js, _ = step("manual_status_after_cancel", "GET", "/api/calibration/manual/status")
    must(c == 200, "manual/status after cancel should return HTTP 200", failures)
    if isinstance(js, dict):
      must(bool(js.get("active", True)) is False, "manual/status active should be false after cancel", failures)

    report["ok"] = len(failures) == 0
    report["failures"] = failures
    with open(args.report, "w", encoding="utf-8") as f:
      json.dump(report, f, indent=2, ensure_ascii=False)

    if failures:
      print("[FAIL] calibration regression")
      for x in failures:
        print(" -", x)
      return 1
    print("[PASS] calibration regression")
    return 0


if __name__ == "__main__":
    sys.exit(main())

