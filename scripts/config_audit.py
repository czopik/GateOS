#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import argparse
import json
import os
import sys
import time
from html.parser import HTMLParser
from typing import Any, Dict, List, Optional, Tuple
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen


NETWORK_PREFIXES = ("wifi.", "mqtt.")
DEFAULT_UI_ROOT = "data"

PIN_PATHS = {
    "gpio.pwm",
    "gpio.dir",
    "gpio.en",
    "gpio.limitOpen",
    "gpio.limitClose",
    "gpio.button",
    "gpio.stop",
    "gpio.obstacle",
    "gpio.hcs",
    "gpio.led",
    "hoverUart.rx",
    "hoverUart.tx",
    "limits.open.pin",
    "limits.close.pin",
    "sensors.hall.pin",
    "sensors.photocell.pin",
    "sensors.ld2410.rx",
    "sensors.ld2410.tx",
    "led.pin",
}

SAFE_PINS = [4, 5, 13, 14, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33]


def toggle_bool(v: Any) -> bool:
    return not bool(v)


def bump_int(v: Any, step: int = 1, min_v: Optional[int] = None, max_v: Optional[int] = None) -> int:
    try:
        iv = int(v)
    except Exception:
        iv = 0
    nv = iv + step
    if min_v is not None:
        nv = max(min_v, nv)
    if max_v is not None:
        nv = min(max_v, nv)
    if nv == iv:
        nv = iv - step
        if min_v is not None:
            nv = max(min_v, nv)
        if max_v is not None:
            nv = min(max_v, nv)
    return int(nv)


def bump_float(v: Any, step: float = 0.1, min_v: Optional[float] = None, max_v: Optional[float] = None) -> float:
    try:
        fv = float(v)
    except Exception:
        fv = 0.0
    nv = fv + step
    if min_v is not None:
        nv = max(min_v, nv)
    if max_v is not None:
        nv = min(max_v, nv)
    if nv == fv:
        nv = fv - step
        if min_v is not None:
            nv = max(min_v, nv)
        if max_v is not None:
            nv = min(max_v, nv)
    return float(nv)


def mutate_string(v: Any, suffix: str = "_test") -> str:
    s = "" if v is None else str(v)
    if s.endswith(suffix):
        return s[:-len(suffix)]
    return s + suffix


def pick_pin(v: Any) -> int:
    try:
        cur = int(v)
    except Exception:
        cur = None
    for pin in SAFE_PINS:
        if cur is None or pin != cur:
            return pin
    return cur if cur is not None else SAFE_PINS[0]


def cycle_option(options: List[str], current: Any) -> Any:
    if not options:
        return mutate_string(current)
    cur = "" if current is None else str(current)
    if cur in options:
        idx = options.index(cur)
        nxt = options[(idx + 1) % len(options)]
    else:
        nxt = options[0]
    return nxt


def is_network_path(path: str) -> bool:
    return path.startswith(NETWORK_PREFIXES)


def is_pin_path(path: str) -> bool:
    if path in PIN_PATHS:
        return True
    if path.endswith(".pin") or path.endswith(".rx") or path.endswith(".tx"):
        return True
    return False


class UiScanner(HTMLParser):
    def __init__(self) -> None:
        super().__init__()
        self.fields: Dict[str, Dict[str, Any]] = {}
        self.order: List[str] = []
        self._select_path: Optional[str] = None
        self._select_opts: List[str] = []

    def _record(self, path: str, meta: Dict[str, Any]) -> None:
        if path not in self.fields:
            self.order.append(path)
            self.fields[path] = {}
        existing = self.fields[path]
        merged = dict(existing)
        for key, val in meta.items():
            if val is None or val == "":
                continue
            if key == "options":
                cur = merged.get("options", [])
                for opt in val:
                    if opt not in cur:
                        cur.append(opt)
                merged["options"] = cur
            else:
                merged[key] = val
        self.fields[path] = merged

    def handle_starttag(self, tag: str, attrs: List[Tuple[str, Optional[str]]]) -> None:
        attrs_map: Dict[str, str] = {}
        for k, v in attrs:
            attrs_map[k] = "" if v is None else v
        path = attrs_map.get("data-path", "")
        if tag in ("input", "select", "textarea") and path:
            meta = {
                "kind": tag,
                "input_type": attrs_map.get("type", "text") if tag == "input" else "select" if tag == "select" else "textarea",
                "data_type": attrs_map.get("data-type", ""),
                "step": attrs_map.get("step", ""),
                "readonly": ("readonly" in attrs_map or "disabled" in attrs_map),
            }
            self._record(path, meta)
            if tag == "select":
                self._select_path = path
                self._select_opts = []

        if tag == "option" and self._select_path:
            val = attrs_map.get("value")
            if val is not None:
                self._select_opts.append(val)

    def handle_endtag(self, tag: str) -> None:
        if tag == "select" and self._select_path:
            if self._select_opts:
                self._record(self._select_path, {"options": self._select_opts})
            self._select_path = None
            self._select_opts = []


def scan_ui_paths(ui_root: str) -> Tuple[List[str], Dict[str, Dict[str, Any]]]:
    order: List[str] = []
    fields: Dict[str, Dict[str, Any]] = {}
    for root, _, files in os.walk(ui_root):
        for name in files:
            if not name.lower().endswith(".html"):
                continue
            path = os.path.join(root, name)
            try:
                with open(path, "r", encoding="utf-8", errors="replace") as fh:
                    text = fh.read()
            except Exception:
                continue
            parser = UiScanner()
            parser.feed(text)
            for p in parser.order:
                if p not in fields:
                    order.append(p)
                    fields[p] = {}
                meta = fields[p]
                incoming = parser.fields.get(p, {})
                for key, val in incoming.items():
                    if key == "options":
                        opts = meta.get("options", [])
                        for opt in val:
                            if opt not in opts:
                                opts.append(opt)
                        meta["options"] = opts
                    else:
                        if val is not None and val != "":
                            meta[key] = val
                fields[p] = meta
    return order, fields


def load_paths_file(path: str) -> List[str]:
    items: List[str] = []
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            raw = line.strip()
            if not raw or raw.startswith("#"):
                continue
            items.append(raw)
    return items


def split_path(path: str) -> List[str]:
    return [p for p in path.strip().split(".") if p]


def get_by_path(obj: Dict[str, Any], path: str) -> Tuple[bool, Any]:
    cur: Any = obj
    for key in split_path(path):
        if not isinstance(cur, dict) or key not in cur:
            return False, None
        cur = cur[key]
    return True, cur


def set_by_path(obj: Dict[str, Any], path: str, value: Any) -> None:
    cur: Any = obj
    parts = split_path(path)
    for key in parts[:-1]:
        if key not in cur or not isinstance(cur[key], dict):
            cur[key] = {}
        cur = cur[key]
    cur[parts[-1]] = value


def shallow_payload_for_path(path: str, value: Any) -> Dict[str, Any]:
    payload: Dict[str, Any] = {}
    set_by_path(payload, path, value)
    return payload


def http_json(method: str, url: str, token: Optional[str], body_obj: Optional[Dict[str, Any]] = None,
              timeout: float = 6.0, retries: int = 2, retry_delay: float = 0.5) -> Tuple[int, Dict[str, Any]]:
    headers = {"Accept": "application/json"}
    if token:
        headers["X-API-Token"] = token
        headers["X-Api-Key"] = token

    data = None
    if body_obj is not None:
        raw = json.dumps(body_obj, ensure_ascii=False).encode("utf-8")
        data = raw
        headers["Content-Type"] = "application/json"

    req = Request(url, data=data, headers=headers, method=method)
    attempt = 0
    while True:
        try:
            with urlopen(req, timeout=timeout) as resp:
                status = int(getattr(resp, "status", 200))
                raw = resp.read().decode("utf-8", errors="replace").strip()
                if not raw:
                    return status, {}
                try:
                    return status, json.loads(raw)
                except json.JSONDecodeError:
                    return status, {"_raw": raw}
        except HTTPError as e:
            raw = ""
            try:
                raw = e.read().decode("utf-8", errors="replace").strip()
            except Exception:
                pass
            return int(e.code), {"error": f"HTTP {e.code}", "body": raw}
        except (URLError, ConnectionResetError, TimeoutError) as e:
            if attempt >= retries:
                return 0, {"error": "URLERROR", "reason": str(e)}
            time.sleep(retry_delay * (attempt + 1))
            attempt += 1
        except Exception as e:
            if attempt >= retries:
                return 0, {"error": "EXCEPTION", "reason": str(e)}
            time.sleep(retry_delay * (attempt + 1))
            attempt += 1


def normalize_base_url(url: str) -> str:
    return url.strip().rstrip("/")


def endpoint(base: str, path: str) -> str:
    return f"{base}{path}"


def load_config(base: str, token: Optional[str]) -> Dict[str, Any]:
    status, js = http_json("GET", endpoint(base, "/api/config"), token, None)
    if status in (401, 403):
        raise RuntimeError("Unauthorized: provide --token (security enabled).")
    if status != 200:
        raise RuntimeError(f"GET /api/config failed: status={status} resp={js}")
    if isinstance(js, dict) and "config" in js and isinstance(js["config"], dict):
        return js["config"]
    if isinstance(js, dict):
        return js
    raise RuntimeError(f"Unexpected /api/config response: {js}")


def load_motion_profile(base: str, token: Optional[str]) -> Tuple[bool, Dict[str, Any]]:
    status, js = http_json("GET", endpoint(base, "/api/motion/profile"), token, None)
    if status in (401, 403):
        raise RuntimeError("Unauthorized: provide --token (security enabled).")
    if status != 200:
        return False, {}
    if isinstance(js, dict) and "motion" in js and isinstance(js["motion"], dict):
        return True, js
    if isinstance(js, dict):
        return True, js
    return False, {}


def save_partial_config(base: str, token: Optional[str], partial: Dict[str, Any]) -> Tuple[int, Dict[str, Any]]:
    return http_json("POST", endpoint(base, "/api/config"), token, partial)


def pretty(v: Any) -> str:
    try:
        return json.dumps(v, ensure_ascii=False)
    except Exception:
        return str(v)


def values_equal(a: Any, b: Any) -> bool:
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return abs(float(a) - float(b)) < 1e-4
    return a == b


def infer_type(meta: Dict[str, Any], cur: Any) -> str:
    data_type = (meta.get("data_type") or "").strip().lower()
    input_type = (meta.get("input_type") or "").strip().lower()
    if input_type == "checkbox":
        return "bool"
    if meta.get("kind") == "select":
        return "enum"
    if data_type in ("int", "float", "bool"):
        return data_type
    if input_type == "number":
        step = (meta.get("step") or "").strip()
        if "." in step:
            return "float"
        return "int"
    if isinstance(cur, bool):
        return "bool"
    if isinstance(cur, int):
        return "int"
    if isinstance(cur, float):
        return "float"
    return "string"


def generate_value(path: str, meta: Dict[str, Any], cur: Any) -> Any:
    if is_pin_path(path):
        return pick_pin(cur)
    kind = infer_type(meta, cur)
    if kind == "bool":
        return toggle_bool(cur)
    if kind == "int":
        return bump_int(cur, step=1)
    if kind == "float":
        return bump_float(cur, step=0.1)
    if kind == "enum":
        options = meta.get("options", [])
        next_val = cycle_option(options, cur)
        data_type = (meta.get("data_type") or "").strip().lower()
        if data_type == "int":
            try:
                return int(next_val)
            except Exception:
                return bump_int(cur, step=1)
        if data_type == "float":
            try:
                return float(next_val)
            except Exception:
                return bump_float(cur, step=0.1)
        return next_val
    return mutate_string(cur)


def format_line(status: str, path: str, detail: str = "") -> str:
    if detail:
        return f"{status} {path} {detail}"
    return f"{status} {path}"


def run_audit(base: str, token: Optional[str], paths: List[str], meta_map: Dict[str, Dict[str, Any]],
              include_network: bool, no_restore: bool, fail_fast: bool, check_motion_profile: bool,
              report_file: Optional[str], delay: float) -> int:
    total = passed = failed = skipped = 0
    lines: List[str] = []
    current_token = token

    for path in paths:
        if not path:
            continue
        meta = meta_map.get(path, {})
        if meta.get("kind") == "select" and len(meta.get("options", [])) <= 1:
            skipped += 1
            lines.append(format_line("SKIPPED", path, "reason=single_option"))
            continue
        if meta.get("readonly"):
            skipped += 1
            lines.append(format_line("SKIPPED", path, "reason=readonly"))
            continue
        if is_network_path(path) and not include_network:
            try:
                cfg = load_config(base, current_token)
                exists, _ = get_by_path(cfg, path)
                note = "reason=network" if exists else "reason=network_missing"
            except Exception as e:
                note = f"reason=network_check_failed error={e}"
            skipped += 1
            lines.append(format_line("SKIPPED", path, note))
            continue

        total += 1
        before_cfg = load_config(base, current_token)
        existed, before_val = get_by_path(before_cfg, path)

        if path == "motor.pwmMax":
            _, pwm_min = get_by_path(before_cfg, "motor.pwmMin")
            try:
                cur = int(before_val)
            except Exception:
                cur = 0
            min_v = int(pwm_min) if pwm_min is not None else 0
            if cur + 1 <= 255:
                new_val = cur + 1
            elif cur - 1 >= min_v:
                new_val = cur - 1
            else:
                new_val = cur
        elif path == "motor.pwmMin":
            _, pwm_max = get_by_path(before_cfg, "motor.pwmMax")
            try:
                cur = int(before_val)
            except Exception:
                cur = 0
            max_v = int(pwm_max) if pwm_max is not None else 255
            if cur + 1 <= max_v:
                new_val = cur + 1
            elif cur - 1 >= 0:
                new_val = cur - 1
            else:
                new_val = cur
        else:
            new_val = generate_value(path, meta, before_val)
        if values_equal(new_val, before_val):
            failed += 1
            lines.append(format_line("FAIL", path, f"reason=no_change before={pretty(before_val)}"))
            if fail_fast:
                break
            continue

        old_token = current_token
        try:
            payload = shallow_payload_for_path(path, new_val)
            status, resp = save_partial_config(base, current_token, payload)
            if status in (401, 403):
                raise RuntimeError("Unauthorized: provide --token (security enabled).")
            if status != 200:
                failed += 1
                lines.append(format_line("FAIL", path, f"reason=post_failed status={status} resp={pretty(resp)}"))
                if fail_fast:
                    break
                continue

            if path == "security.apiToken" and current_token is not None:
                current_token = str(new_val)

            time.sleep(0.15)
            after_cfg = load_config(base, current_token)
            after_exists, after_val = get_by_path(after_cfg, path)

            if after_exists and values_equal(after_val, new_val):
                mp_ok = True
                if check_motion_profile and path.startswith("motion."):
                    ok, mp = load_motion_profile(base, current_token)
                    if ok:
                        mp_exists, mp_val = get_by_path(mp, path)
                        if not (mp_exists and values_equal(mp_val, new_val)):
                            mp_ok = False
                            failed += 1
                            lines.append(format_line("FAIL", path, f"reason=motion_profile_mismatch expected={pretty(new_val)} got={pretty(mp_val)}"))
                    else:
                        mp_ok = False
                        failed += 1
                        lines.append(format_line("FAIL", path, "reason=motion_profile_unavailable"))
                if mp_ok:
                    passed += 1
                    lines.append(format_line("PASS", path, f"set={pretty(new_val)}"))
            else:
                reason = "not_persisted"
                if not existed:
                    reason = "missing_in_config"
                elif not after_exists:
                    reason = "missing_after_save"
                failed += 1
                lines.append(format_line("FAIL", path, f"reason={reason} expected={pretty(new_val)} got={pretty(after_val)} before={pretty(before_val)}"))
                if fail_fast:
                    break

        except Exception as e:
            failed += 1
            lines.append(format_line("FAIL", path, f"reason=exception error={e}"))
            if fail_fast:
                break
        finally:
            if not no_restore:
                try:
                    rb_payload = shallow_payload_for_path(path, before_val)
                    status, resp = save_partial_config(base, current_token, rb_payload)
                    if status not in (200, 401, 403):
                        lines.append(format_line("WARN", path, f"rollback_failed status={status} resp={pretty(resp)}"))
                except Exception as e:
                    lines.append(format_line("WARN", path, f"rollback_failed error={e}"))

        if path == "security.apiToken":
            current_token = old_token
        if delay > 0:
            time.sleep(delay)

    summary = f"SUMMARY total={total} pass={passed} fail={failed} skipped={skipped}"
    lines.append("")
    lines.append(summary)

    for line in lines:
        print(line)

    if report_file:
        try:
            with open(report_file, "w", encoding="utf-8") as fh:
                for line in lines:
                    fh.write(line + "\n")
        except Exception as e:
            print(f"[WARN] report write failed: {e}")

    return 0 if failed == 0 else 1


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default=None, help="Base URL, e.g. http://192.168.1.12")
    ap.add_argument("--token", default=None, help="API token (X-API-Token / X-Api-Key) if security is enabled")
    ap.add_argument("--only", default=None, help="Filter by prefix, e.g. motion, safety, wifi")
    ap.add_argument("--no-restore", action="store_true", help="Do not rollback changes after each test")
    ap.add_argument("--list", action="store_true", help="Print resolved path list and exit")
    ap.add_argument("--fail-fast", action="store_true", help="Stop on first FAIL")
    ap.add_argument("--scan-ui", action="store_true", help="Scan UI (data/*.html) for data-path fields")
    ap.add_argument("--ui-root", default=DEFAULT_UI_ROOT, help="UI root directory (default: data)")
    ap.add_argument("--paths-file", default=None, help="Read paths from file (one per line)")
    ap.add_argument("--paths-out", default=None, help="Write resolved paths to file")
    ap.add_argument("--include-network", action="store_true", help="Include wifi/mqtt fields in tests")
    ap.add_argument("--report-file", default=None, help="Write audit report to file")
    ap.add_argument("--skip-motion-profile", action="store_false", dest="check_motion_profile",
                    help="Skip /api/motion/profile check for motion.* paths")
    ap.add_argument("--delay", type=float, default=0.1, help="Sleep between field tests (seconds)")
    ap.set_defaults(check_motion_profile=True)
    args = ap.parse_args()

    paths: List[str] = []
    meta_map: Dict[str, Dict[str, Any]] = {}

    if args.scan_ui:
        ui_paths, ui_meta = scan_ui_paths(args.ui_root)
        paths.extend(ui_paths)
        meta_map.update(ui_meta)

    if args.paths_file:
        paths.extend(load_paths_file(args.paths_file))

    if args.only:
        prefix = args.only.strip().rstrip(".")
        prefix_dot = prefix + "."
        paths = [p for p in paths if p == prefix or p.startswith(prefix_dot)]

    # Deduplicate while preserving order.
    seen = set()
    deduped: List[str] = []
    for p in paths:
        if not p or p in seen:
            continue
        seen.add(p)
        deduped.append(p)
    paths = deduped

    if args.paths_out:
        try:
            with open(args.paths_out, "w", encoding="utf-8") as fh:
                for p in paths:
                    fh.write(p + "\n")
        except Exception as e:
            print(f"[WARN] paths write failed: {e}")

    if args.list:
        for p in paths:
            print(p)
        return 0

    if not args.url:
        print("[ERROR] --url is required (unless using --list).")
        return 2

    if not args.token:
        print("[WARN] --token not provided. If security.enabled=true, requests will get 401/403.")

    if not paths:
        print("[ERROR] No paths resolved. Use --scan-ui or --paths-file.")
        return 2

    base = normalize_base_url(args.url)

    try:
        return run_audit(
            base=base,
            token=args.token,
            paths=paths,
            meta_map=meta_map,
            include_network=args.include_network,
            no_restore=args.no_restore,
            fail_fast=args.fail_fast,
            check_motion_profile=args.check_motion_profile,
            report_file=args.report_file,
            delay=args.delay,
        )
    except Exception as e:
        print(f"[ERROR] {e}")
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
