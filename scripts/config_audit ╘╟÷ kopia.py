#!/usr/bin/env python3
"""
Simple config persistence audit for GateOS.

Usage:
  python scripts/config_audit.py --url http://192.168.1.12 --token gateos123

The script compares /api/config before/after modifying each field and reports whether the change persisted.
"""

import argparse
import copy
import json
import sys
import urllib.error
import urllib.request


def request_json(method, url, token=None, payload=None):
    data = None
    headers = {}
    if token:
        headers["X-Api-Key"] = token
    if payload is not None:
        data = json.dumps(payload).encode("utf-8")
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            body = resp.read().decode("utf-8")
            if body:
                return resp.status, json.loads(body)
            return resp.status, None
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8")
        return exc.code, body
    except urllib.error.URLError as exc:
        raise RuntimeError(f"{method} {url} failed: {exc}") from exc


def get_config(base_url, token):
    status, data = request_json("GET", f"{base_url}/api/config", token=token)
    if status != 200:
        raise RuntimeError(f"GET /api/config -> {status}: {data}")
    return data


def post_config(base_url, token, cfg):
    status, data = request_json("POST", f"{base_url}/api/config", token=token, payload=cfg)
    if status != 200:
        raise RuntimeError(f"POST /api/config -> {status}: {data}")
    return data


def get_by_path(obj, path):
    cur = obj
    for part in path.split("."):
        if not isinstance(cur, dict):
            return None
        cur = cur.get(part)
        if cur is None:
            return None
    return cur


def set_by_path(obj, path, value):
    cur = obj
    parts = path.split(".")
    for part in parts[:-1]:
        if part not in cur or not isinstance(cur[part], dict):
            cur[part] = {}
        cur = cur[part]
    cur[parts[-1]] = value


def bump_force(value):
    base = value if isinstance(value, (int, float)) else 55
    delta = 5
    if base >= 95:
        candidate = base - delta
    else:
        candidate = base + delta
    candidate = int(max(0, min(100, candidate)))
    if candidate == base:
        candidate = int(max(0, min(100, base - 10)))
    return candidate


FIELD_TESTS = [
    ("wifi.ssid", lambda old: f"{old or 'GateOS'}-test"),
    ("mqtt.topicBase", lambda old: f"{old or 'brama'}-cfg"),
    ("safety.obstacleAction", lambda old: "reverse" if old != "reverse" else "open"),
    ("security.enabled", lambda old: not bool(old)),
    ("motion.ui.speedOpen", lambda old: "slow" if old != "slow" else "fast"),
    ("motion.advanced.braking.force", bump_force),
    ("motion.advanced.rampOpen.value", lambda old: max(0.1, (old or 1.0) + 0.5)),
]


def main():
    parser = argparse.ArgumentParser(description="Audit config persistence via /api/config.")
    parser.add_argument("--url", default="http://127.0.0.1", help="Base URL of GateOS (include scheme).")
    parser.add_argument("--token", default="", help="Optional API token (X-Api-Key).")
    args = parser.parse_args()
    base_url = args.url.rstrip("/")
    token = args.token or None

    try:
        baseline = get_config(base_url, token)
    except Exception as exc:
        print("Failed to fetch baseline config:", exc, file=sys.stderr)
        sys.exit(1)

    results = []
    for path, transform in FIELD_TESTS:
        config = copy.deepcopy(baseline)
        current_value = get_by_path(config, path)
        target_value = transform(current_value)
        if target_value == current_value:
            target_value = transform(current_value)
        set_by_path(config, path, target_value)

        ok = False
        reason = "not attempted"
        try:
            post_config(base_url, token, config)
            current = get_config(base_url, token)
            observed = get_by_path(current, path)
            ok = observed == target_value
            reason = "matched" if ok else f"observed={observed!r}"
        except Exception as exc:
            reason = str(exc)
        finally:
            try:
                post_config(base_url, token, baseline)
            except Exception as exc:
                print("WARNING: Failed to restore baseline config:", exc, file=sys.stderr)

        results.append((path, ok, reason))

    print("\nConfig audit results:")
    for path, ok, detail in results:
        status = "PASS" if ok else "FAIL"
        print(f"  {status} {path} -> {detail}")

    fails = [r for r in results if not r[1]]
    if fails:
        sys.exit(1)


if __name__ == "__main__":
    main()
