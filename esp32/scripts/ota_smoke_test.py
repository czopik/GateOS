#!/usr/bin/env python3
import argparse
import json
import socket
import sys
from urllib import request, error


def http_json(url: str, timeout: float = 4.0):
    req = request.Request(url, method="GET")
    with request.urlopen(req, timeout=timeout) as resp:
        raw = resp.read().decode("utf-8", errors="replace")
        return resp.status, json.loads(raw) if raw else {}


def tcp_open(host: str, port: int, timeout: float = 1.5) -> bool:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        s.connect((host, port))
        return True
    except OSError:
        return False
    finally:
        s.close()


def main() -> int:
    ap = argparse.ArgumentParser(description="GateOS OTA smoke test")
    ap.add_argument("--host", required=True, help="Device IP/host")
    ap.add_argument("--port", type=int, default=80, help="HTTP port")
    ap.add_argument("--expect-ota-enabled", action="store_true", help="Fail if ota.enabled is false")
    args = ap.parse_args()

    base = f"http://{args.host}:{args.port}"
    failures = []

    try:
        code, status = http_json(base + "/api/status")
    except (error.URLError, json.JSONDecodeError, TimeoutError) as ex:
        print(f"[FAIL] cannot read /api/status: {ex}")
        return 1

    if code != 200:
        failures.append(f"/api/status http={code}")

    ota = status.get("ota", {}) if isinstance(status, dict) else {}
    if not isinstance(ota, dict):
        failures.append("/api/status.ota is missing or not object")
        ota = {}

    for key in ("enabled", "active"):
        if key not in ota:
            failures.append(f"/api/status.ota.{key} missing")

    if args.expect_ota_enabled and not bool(ota.get("enabled", False)):
        failures.append("OTA expected enabled, got disabled")

    ota_port = ota.get("port", 3232)
    if isinstance(ota_port, int) and ota.get("enabled", False):
        if not tcp_open(args.host, ota_port):
            failures.append(f"OTA TCP port {ota_port} closed/unreachable")

    # Diagnostics contract
    try:
        dcode, diag = http_json(base + "/api/diagnostics")
        if dcode != 200:
            failures.append(f"/api/diagnostics http={dcode}")
        elif not isinstance(diag.get("ota", {}), dict):
            failures.append("/api/diagnostics.ota missing")
    except Exception as ex:  # noqa: BLE001
        failures.append(f"/api/diagnostics error: {ex}")

    if failures:
        print("[FAIL] OTA smoke test")
        for item in failures:
            print(" -", item)
        return 1

    print("[PASS] OTA smoke test")
    print(f" host={args.host} ota.enabled={ota.get('enabled')} ota.active={ota.get('active')} ota.port={ota.get('port')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

