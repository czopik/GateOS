#!/usr/bin/env python3
import argparse
import json
import sys
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple
from urllib import error, request


@dataclass
class Sample:
    ts: float
    status: Dict[str, Any]


def now_iso() -> str:
    return datetime.now().isoformat(timespec="seconds")


def http_json(url: str, timeout: float = 4.0) -> Tuple[int, Dict[str, Any]]:
    req = request.Request(url, method="GET")
    with request.urlopen(req, timeout=timeout) as resp:
        raw = resp.read().decode("utf-8", errors="replace")
        data = json.loads(raw) if raw else {}
        if not isinstance(data, dict):
            raise ValueError("API did not return a JSON object")
        return resp.status, data


def gate_get(status: Dict[str, Any], key: str, default: Any = None) -> Any:
    gate = status.get("gate", {})
    if not isinstance(gate, dict):
        return default
    return gate.get(key, default)


def hb_get(status: Dict[str, Any], key: str, default: Any = None) -> Any:
    hb = status.get("hb", {})
    if not isinstance(hb, dict):
        return default
    return hb.get(key, default)


def runtime_get(status: Dict[str, Any], key: str, default: Any = None) -> Any:
    runtime = status.get("runtime", {})
    if not isinstance(runtime, dict):
        return default
    return runtime.get(key, default)


def io_get(status: Dict[str, Any], key: str, default: Any = None) -> Any:
    io = status.get("io", {})
    if not isinstance(io, dict):
        return default
    return io.get(key, default)


def detect_anomalies(curr: Sample, prev: Optional[Sample], near_end_m: float, high_rpm: int) -> List[str]:
    out: List[str] = []
    status = curr.status

    state = str(gate_get(status, "state", ""))
    moving = bool(gate_get(status, "moving", False))
    pos = float(gate_get(status, "position", 0.0) or 0.0)
    target = float(gate_get(status, "targetPosition", 0.0) or 0.0)
    max_dist = float(gate_get(status, "maxDistance", 0.0) or 0.0)
    err = int(gate_get(status, "errorCode", 0) or 0)
    stop_reason = int(gate_get(status, "stopReason", 0) or 0)

    rpm = int(hb_get(status, "rpm", 0) or 0)
    fault = int(hb_get(status, "fault", 0) or 0)
    tel_age = int(hb_get(status, "telAgeMs", -1) or -1)

    lim_open = bool(io_get(status, "limitOpen", False))
    lim_close = bool(io_get(status, "limitClose", False))

    if err != 0:
        out.append(f"ERROR state err={err} stopReason={stop_reason}")

    if state == "opening" and lim_open and abs(rpm) > 15:
        out.append(f"opening while OPEN limit active (rpm={rpm})")
    if state == "closing" and lim_close and abs(rpm) > 15:
        out.append(f"closing while CLOSE limit active (rpm={rpm})")

    if max_dist > 0.0 and pos > max_dist + 0.03:
        out.append(f"position overshoot above maxDistance (pos={pos:.3f} > max={max_dist:.3f})")
    if pos < -0.03:
        out.append(f"position below zero (pos={pos:.3f})")

    if moving and max_dist > 0.0:
        dist_to_open = max_dist - pos
        dist_to_close = pos
        if state == "opening" and dist_to_open <= near_end_m and abs(rpm) >= high_rpm:
            out.append(f"near OPEN end but RPM still high (dist={dist_to_open:.3f}m rpm={rpm})")
        if state == "closing" and dist_to_close <= near_end_m and abs(rpm) >= high_rpm:
            out.append(f"near CLOSE end but RPM still high (dist={dist_to_close:.3f}m rpm={rpm})")

    if tel_age > 2000 and moving:
        out.append(f"telemetry stale while moving (telAgeMs={tel_age})")

    if fault != 0:
        out.append(f"hover fault reported ({fault})")

    if prev is not None:
        dt = curr.ts - prev.ts
        if dt > 0:
            prev_pos = float(gate_get(prev.status, "position", pos) or pos)
            dpos = pos - prev_pos
            speed_est = dpos / dt
            if moving and state == "opening" and speed_est < -0.03:
                out.append(f"opening but position decreases (dpos={dpos:.3f}m dt={dt:.2f}s)")
            if moving and state == "closing" and speed_est > 0.03:
                out.append(f"closing but position increases (dpos={dpos:.3f}m dt={dt:.2f}s)")

    return out


def write_jsonl(path: Path, obj: Dict[str, Any]) -> None:
    with path.open("a", encoding="utf-8") as f:
        f.write(json.dumps(obj, ensure_ascii=False) + "\n")


def main() -> int:
    ap = argparse.ArgumentParser(description="Live monitor GateOS status API and flag movement anomalies")
    ap.add_argument("--host", required=True, help="Device IP or hostname")
    ap.add_argument("--port", type=int, default=8080, help="HTTP port")
    ap.add_argument("--interval", type=float, default=0.25, help="Poll interval in seconds")
    ap.add_argument("--duration", type=float, default=0.0, help="Run time in seconds; 0 = infinite")
    ap.add_argument("--near-end-m", type=float, default=0.35, help="Distance window near endpoint to enforce low RPM")
    ap.add_argument("--high-rpm", type=int, default=140, help="RPM threshold considered too high near endpoint")
    ap.add_argument("--out", default="scripts/live_api_monitor_log.jsonl", help="JSONL output file")
    args = ap.parse_args()

    base = f"http://{args.host}:{args.port}"
    status_url = base + "/api/status"
    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)

    print(f"[{now_iso()}] monitor start url={status_url} interval={args.interval}s")
    print(f"[{now_iso()}] writing log to {out_path}")

    start = time.time()
    prev: Optional[Sample] = None
    seen_events: Dict[Tuple[int, str, str], bool] = {}
    req_ok = 0
    req_fail = 0
    error_streak = 0
    error_streak_since: Optional[float] = None
    last_ok_ts: Optional[float] = None
    reboot_count = 0

    while True:
        ts = time.time()
        if args.duration > 0 and (ts - start) >= args.duration:
            break

        try:
            code, status = http_json(status_url, timeout=4.0)
            if code != 200:
                raise RuntimeError(f"HTTP {code}")
            req_ok += 1
        except (error.URLError, TimeoutError, json.JSONDecodeError, ValueError, RuntimeError) as ex:
            req_fail += 1
            if error_streak == 0:
                error_streak_since = ts
            error_streak += 1
            msg = f"[{now_iso()}] WARN api read failed: {ex}"
            print(msg)
            write_jsonl(out_path, {
                "ts": ts,
                "iso": now_iso(),
                "type": "api_error",
                "error": str(ex),
                "errorStreak": error_streak,
                "errorStreakSinceTs": error_streak_since,
                "lastOkTs": last_ok_ts,
            })
            time.sleep(max(0.05, args.interval))
            continue

        if error_streak > 0:
            recovered_after = ts - (error_streak_since or ts)
            print(
                f"[{now_iso()}] INFO api recovered after errors={error_streak} "
                f"downtime={recovered_after:.1f}s"
            )
            write_jsonl(out_path, {
                "ts": ts,
                "iso": now_iso(),
                "type": "api_recovered",
                "errors": error_streak,
                "downtimeSec": recovered_after,
                "errorStreakSinceTs": error_streak_since,
            })
            error_streak = 0
            error_streak_since = None

        curr = Sample(ts=ts, status=status)
        last_ok_ts = ts

        state = str(gate_get(status, "state", "?"))
        moving = bool(gate_get(status, "moving", False))
        pos = float(gate_get(status, "position", 0.0) or 0.0)
        tgt = float(gate_get(status, "targetPosition", 0.0) or 0.0)
        max_dist = float(gate_get(status, "maxDistance", 0.0) or 0.0)
        rpm = int(hb_get(status, "rpm", 0) or 0)
        err = int(gate_get(status, "errorCode", 0) or 0)
        tel_age = int(hb_get(status, "telAgeMs", -1) or -1)
        uptime_ms = int(status.get("uptimeMs", 0) or 0)
        boot_count = int(runtime_get(status, "bootCount", -1) or -1)
        reset_reason = str(runtime_get(status, "resetReason", "?"))
        reset_reason_code = int(runtime_get(status, "resetReasonCode", -1) or -1)
        loop_age_ms = int(runtime_get(status, "mainLoopAgeMs", -1) or -1)
        gate_age_ms = int(runtime_get(status, "gateTaskAgeMs", -1) or -1)
        status_us = int(runtime_get(status, "lastStatusDurationUs", -1) or -1)

        print(
            f"[{now_iso()}] state={state:<8} moving={int(moving)} pos={pos:6.3f} target={tgt:6.3f} "
            f"max={max_dist:6.3f} rpm={rpm:4d} telAge={tel_age:4d} err={err} "
            f"up={uptime_ms}ms boot={boot_count} rr={reset_reason}({reset_reason_code}) "
            f"loopAge={loop_age_ms} gateAge={gate_age_ms} statusUs={status_us}"
        )

        if prev is not None:
            prev_uptime = int(prev.status.get("uptimeMs", uptime_ms) or uptime_ms)
            # Uptime drop means reboot happened between successful API samples.
            if uptime_ms + 5000 < prev_uptime:
                reboot_count += 1
                prev_rr = runtime_get(prev.status, "resetReason", "?")
                prev_boot = runtime_get(prev.status, "bootCount", -1)
                print(
                    f"[{now_iso()}] ALERT reboot detected prevUp={prev_uptime}ms -> nowUp={uptime_ms}ms "
                    f"prevBoot={prev_boot} nowBoot={boot_count} nowReset={reset_reason}"
                )
                write_jsonl(out_path, {
                    "ts": ts,
                    "iso": now_iso(),
                    "type": "reboot_detected",
                    "prevUptimeMs": prev_uptime,
                    "uptimeMs": uptime_ms,
                    "prevResetReason": prev_rr,
                    "resetReason": reset_reason,
                    "resetReasonCode": reset_reason_code,
                    "prevBootCount": prev_boot,
                    "bootCount": boot_count,
                    "rebootCount": reboot_count,
                })

        if reset_reason in {"panic", "task_wdt", "int_wdt", "other_wdt", "brownout"}:
            print(f"[{now_iso()}] ALERT unhealthy reset reason observed: {reset_reason} ({reset_reason_code})")

        anomalies = detect_anomalies(curr, prev, args.near_end_m, args.high_rpm)
        for a in anomalies:
            print(f"[{now_iso()}] ALERT {a}")

        events = status.get("events", [])
        if isinstance(events, list):
            for e in events:
                if not isinstance(e, dict):
                    continue
                key = (int(e.get("ts", 0) or 0), str(e.get("level", "")), str(e.get("message", "")))
                if key in seen_events:
                    continue
                seen_events[key] = True
                print(f"[{now_iso()}] EVENT {key[1]} {key[2]} @ts={key[0]}")

        write_jsonl(out_path, {
            "ts": ts,
            "iso": now_iso(),
            "type": "sample",
            "summary": {
                "state": state,
                "moving": moving,
                "position": pos,
                "targetPosition": tgt,
                "maxDistance": max_dist,
                "rpm": rpm,
                "telAgeMs": tel_age,
                "errorCode": err,
                "uptimeMs": uptime_ms,
                "bootCount": boot_count,
                "resetReason": reset_reason,
                "resetReasonCode": reset_reason_code,
                "mainLoopAgeMs": loop_age_ms,
                "gateTaskAgeMs": gate_age_ms,
                "lastStatusDurationUs": status_us,
            },
            "anomalies": anomalies,
            "status": status,
        })

        prev = curr
        time.sleep(max(0.05, args.interval))

    print(f"[{now_iso()}] monitor stop ok={req_ok} fail={req_fail} reboots={reboot_count} log={out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
