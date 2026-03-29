import time
import statistics
import requests

BASE = "http://192.168.1.43"


def post(path, payload=None, timeout=4):
    r = requests.post(BASE + path, json=payload, timeout=timeout)
    return r.status_code, r.text


def get_status():
    r = requests.get(BASE + "/api/status", timeout=3)
    r.raise_for_status()
    return r.json()


def sample_window(seconds, tag):
    out = []
    t0 = time.time()
    while time.time() - t0 < seconds:
        st = get_status()
        gate = st.get("gate", {})
        hb = st.get("hb", {})
        out.append({
            "t": time.time(),
            "tag": tag,
            "state": gate.get("state"),
            "moving": gate.get("moving"),
            "gate_pos": gate.get("position"),
            "gate_pct": gate.get("positionPercent"),
            "target": gate.get("targetPosition"),
            "hb_rpm": hb.get("rpm"),
            "hb_dir": hb.get("dir"),
            "hb_dist_mm": hb.get("dist_mm"),
            "hb_dist_mm_raw": hb.get("dist_mm_raw"),
            "hb_lastTelMs": hb.get("lastTelMs"),
        })
        time.sleep(0.12)
    return out


def monotonic_ratio(vals, increasing=True):
    pairs = list(zip(vals, vals[1:]))
    if not pairs:
        return 0.0
    good = 0
    for a, b in pairs:
        if increasing and b >= a:
            good += 1
        if (not increasing) and b <= a:
            good += 1
    return good / len(pairs)


def summarize_motion(samples, phase):
    moving = [s for s in samples if s["moving"]]
    if not moving:
        return {"phase": phase, "moving_samples": 0, "note": "no moving samples"}

    rpms = [int(s["hb_rpm"]) for s in moving if s["hb_rpm"] is not None]
    dirs = [int(s["hb_dir"]) for s in moving if s["hb_dir"] is not None]
    dnorm = [int(s["hb_dist_mm"]) for s in moving if s["hb_dist_mm"] is not None]
    draw = [int(s["hb_dist_mm_raw"]) for s in moving if s["hb_dist_mm_raw"] is not None]
    gpos = [float(s["gate_pos"]) for s in moving if s["gate_pos"] is not None]

    out = {
        "phase": phase,
        "moving_samples": len(moving),
        "rpm_median": statistics.median(rpms) if rpms else None,
        "rpm_min": min(rpms) if rpms else None,
        "rpm_max": max(rpms) if rpms else None,
        "dir_set": sorted(set(dirs)) if dirs else [],
        "gate_pos_monotonic_inc": monotonic_ratio(gpos, increasing=True) if gpos else None,
        "gate_pos_monotonic_dec": monotonic_ratio(gpos, increasing=False) if gpos else None,
        "hb_dist_norm_monotonic_inc": monotonic_ratio(dnorm, increasing=True) if dnorm else None,
        "hb_dist_norm_monotonic_dec": monotonic_ratio(dnorm, increasing=False) if dnorm else None,
        "hb_dist_raw_monotonic_inc": monotonic_ratio(draw, increasing=True) if draw else None,
        "hb_dist_raw_monotonic_dec": monotonic_ratio(draw, increasing=False) if draw else None,
    }
    return out


def main():
    results = []

    # baseline
    base = sample_window(1.2, "baseline")
    results.append({"test": "baseline", "samples": len(base), "last_state": base[-1]["state"] if base else None})

    # OPEN phase
    post("/api/control", {"action": "open"})
    open_samples = sample_window(5.0, "open")
    post("/api/control", {"action": "stop"})
    time.sleep(0.6)
    open_sum = summarize_motion(open_samples, "open")
    results.append({"test": "open_motion", **open_sum})

    # CLOSE phase
    post("/api/control", {"action": "close"})
    close_samples = sample_window(5.0, "close")
    post("/api/control", {"action": "stop"})
    time.sleep(0.6)
    close_sum = summarize_motion(close_samples, "close")
    results.append({"test": "close_motion", **close_sum})

    # short fwd/back drift check
    post("/api/control", {"action": "open"})
    s1 = sample_window(1.6, "short_open")
    post("/api/control", {"action": "close"})
    s2 = sample_window(1.6, "short_close")
    post("/api/control", {"action": "stop"})
    time.sleep(0.8)
    st_end = get_status()

    p0 = base[-1]["gate_pos"] if base and base[-1]["gate_pos"] is not None else None
    p_end = st_end.get("gate", {}).get("position")
    drift = None
    if p0 is not None and p_end is not None:
        drift = float(p_end) - float(p0)

    results.append({
        "test": "short_fwd_back",
        "samples_open": len(s1),
        "samples_close": len(s2),
        "pos_start": p0,
        "pos_end": p_end,
        "drift_m": drift,
    })

    print("HOVER POSITION E2E")
    for r in results:
        print(r)


if __name__ == "__main__":
    main()
