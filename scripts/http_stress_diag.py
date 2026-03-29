import time
import requests

BASE = "http://192.168.1.43"
N = 1200
DIAG_EVERY = 100

ok = 0
fail = 0
connect_timeout = 0
conn_err = 0
lat_ms = []

for i in range(1, N + 1):
    t0 = time.time()
    try:
        r = requests.get(BASE + "/api/status", timeout=1.2)
        dt = (time.time() - t0) * 1000.0
        if r.status_code == 200:
            ok += 1
            lat_ms.append(dt)
        else:
            fail += 1
    except requests.exceptions.ConnectTimeout:
        fail += 1
        connect_timeout += 1
    except requests.exceptions.ConnectionError:
        fail += 1
        conn_err += 1
    except Exception:
        fail += 1

    if i % DIAG_EVERY == 0:
        try:
            resp = requests.get(BASE + "/api/diagnostics", timeout=2.0)
            text = resp.text
            diag = resp.json()
            rt = diag.get("runtime", {})
            print(
                f"i={i} ok={ok} fail={fail} ct={connect_timeout} ce={conn_err} "
                f"heap={rt.get('freeHeap')} min={rt.get('minFreeHeap')} ws={rt.get('wsClients')} "
                f"req={rt.get('statusReqCount')} maxUs={rt.get('maxStatusDurationUs')}"
            )
        except requests.exceptions.JSONDecodeError:
            print(
                f"i={i} ok={ok} fail={fail} ct={connect_timeout} ce={conn_err} "
                f"diag_err=JSONDecodeError"
            )
        except Exception as e:
            print(f"i={i} ok={ok} fail={fail} ct={connect_timeout} ce={conn_err} diag_err={type(e).__name__}")

if lat_ms:
    lat_sorted = sorted(lat_ms)
    p95 = lat_sorted[max(0, int(len(lat_sorted) * 0.95) - 1)]
    print(
        "FINAL",
        {
            "ok": ok,
            "fail": fail,
            "connect_timeout": connect_timeout,
            "connection_error": conn_err,
            "avg_ms": sum(lat_ms) / len(lat_ms),
            "p95_ms": p95,
            "max_ms": max(lat_ms),
        },
    )
else:
    print(
        "FINAL",
        {
            "ok": ok,
            "fail": fail,
            "connect_timeout": connect_timeout,
            "connection_error": conn_err,
        },
    )
