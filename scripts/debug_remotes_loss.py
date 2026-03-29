import time
import requests
from pathlib import Path

BASE = "http://192.168.1.43"
SERIAL = 98765431
OUT = Path("scripts")
OUT.mkdir(exist_ok=True)


def wait_online(timeout=40):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            r = requests.get(BASE + "/api/status", timeout=1.2)
            if r.status_code == 200:
                return True
        except Exception:
            pass
        time.sleep(0.5)
    return False


def snap(tag):
    cfg = requests.get(BASE + "/config.json", timeout=5).text
    rem = requests.get(BASE + "/api/remotes", timeout=5).text
    (OUT / f"debug_{tag}_config.json").write_text(cfg, encoding="utf-8")
    (OUT / f"debug_{tag}_remotes.json").write_text(rem, encoding="utf-8")
    print(tag, "config_has_serial", str(SERIAL) in cfg, "remotes_has_serial", str(SERIAL) in rem)


print("wait online", wait_online())
# cleanup
requests.delete(BASE + "/api/remotes", json={"serial": SERIAL}, timeout=3)

# add + update
print("add", requests.post(BASE + "/api/remotes", json={"serial": SERIAL, "name": "PersistA", "enabled": True}, timeout=5).status_code)
print("upd", requests.post(BASE + "/api/remotes", json={"action": "update", "serial": SERIAL, "name": "PersistB", "enabled": False}, timeout=5).status_code)

snap("before_reboot")

# pause before reboot to avoid immediate race
print("sleep before reboot")
time.sleep(2.0)
print("reboot", requests.post(BASE + "/api/reboot", timeout=5).status_code)
print("wait online after reboot", wait_online())
time.sleep(1.0)

snap("after_reboot")
print("done")
