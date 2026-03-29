import time
import requests

BASE = "http://192.168.1.43"
SERIAL = 98765431


def get_remotes():
    r = requests.get(BASE + "/api/remotes", timeout=3)
    r.raise_for_status()
    return r.json()


def find_remote(data, serial):
    for item in data.get("items", []):
        if int(item.get("serial", 0)) == int(serial):
            return item
    return None


def post_remote(payload):
    r = requests.post(BASE + "/api/remotes", json=payload, timeout=3)
    return r.status_code, r.text


def delete_remote(serial):
    r = requests.delete(BASE + "/api/remotes", json={"serial": serial}, timeout=3)
    return r.status_code, r.text


def reboot_and_wait(timeout_s=30):
    r = requests.post(BASE + "/api/reboot", timeout=3)
    if r.status_code != 200:
        raise RuntimeError(f"reboot failed: {r.status_code} {r.text}")
    t0 = time.time()
    time.sleep(1.0)
    while time.time() - t0 < timeout_s:
        try:
            rs = requests.get(BASE + "/api/status", timeout=1.2)
            if rs.status_code == 200:
                return
        except Exception:
            pass
        time.sleep(0.5)
    raise TimeoutError("device did not come back after reboot")


print("[STEP] baseline read")
base = get_remotes()
print("baseline items:", len(base.get("items", [])))

print("[STEP] cleanup target serial if exists")
existing = find_remote(base, SERIAL)
if existing:
    code, txt = delete_remote(SERIAL)
    print("cleanup delete:", code, txt)

print("[STEP] add remote")
code, txt = post_remote({"serial": SERIAL, "name": "PersistTest1", "enabled": True})
print("add:", code, txt)
if code != 200:
    raise RuntimeError("add failed")

print("[STEP] update remote (disable + rename)")
code, txt = post_remote({"action": "update", "serial": SERIAL, "name": "PersistTestDisabled", "enabled": False})
print("update:", code, txt)
if code != 200:
    raise RuntimeError("update failed")

cur = get_remotes()
entry = find_remote(cur, SERIAL)
print("before reboot entry:", entry)
if not entry:
    raise RuntimeError("entry missing before reboot")

print("[STEP] reboot #1 and verify persistence")
reboot_and_wait()
after1 = get_remotes()
entry1 = find_remote(after1, SERIAL)
print("after reboot #1 entry:", entry1)
if not entry1:
    raise RuntimeError("entry missing after reboot #1")
if entry1.get("name") != "PersistTestDisabled" or bool(entry1.get("enabled", True)) is not False:
    raise RuntimeError("entry values not persisted after reboot #1")

print("[STEP] delete remote")
code, txt = delete_remote(SERIAL)
print("delete:", code, txt)
if code != 200:
    raise RuntimeError("delete failed")

after_del = get_remotes()
entry2 = find_remote(after_del, SERIAL)
print("after delete entry:", entry2)
if entry2 is not None:
    raise RuntimeError("entry still present after delete")

print("[STEP] reboot #2 and verify delete persistence")
reboot_and_wait()
after2 = get_remotes()
entry3 = find_remote(after2, SERIAL)
print("after reboot #2 entry:", entry3)
if entry3 is not None:
    raise RuntimeError("entry reappeared after reboot #2")

print("RESULT: PASS remotes persistence across reboot")
