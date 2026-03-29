import time
import requests

BASE = "http://192.168.1.43"

def jget(path):
    r = requests.get(BASE + path, timeout=3)
    r.raise_for_status()
    return r.json()

def post(path, payload=None):
    r = requests.post(BASE + path, json=payload, timeout=3)
    return r.status_code, r.text

def wait_pattern(expected, timeout=4.0):
    t0 = time.time()
    last = None
    while time.time() - t0 < timeout:
        try:
            p = jget('/api/led').get('pattern')
            last = p
            if p == expected:
                return True, p
        except Exception:
            pass
        time.sleep(0.15)
    return False, last

def snapshot(label):
    try:
        led = jget('/api/led')
        st = jget('/api/status')
        return {
            'label': label,
            'pattern': led.get('pattern'),
            'mode': led.get('mode'),
            'gate_state': st.get('gate', {}).get('state'),
            'moving': st.get('gate', {}).get('moving'),
        }
    except Exception as e:
        return {'label': label, 'error': type(e).__name__}

results = []

# Boot pattern check (best-effort)
try:
    post('/api/reboot')
    time.sleep(0.8)
    boot_seen, boot_last = wait_pattern('boot', timeout=2.5)
    results.append({'test': 'boot_pattern', 'pass': boot_seen, 'last': boot_last})
except Exception as e:
    results.append({'test': 'boot_pattern', 'pass': False, 'error': type(e).__name__})

# Ensure status mode and no manual override
post('/api/led', {'mode': 'status', 'pattern': 'off', 'overrideMs': 200})
time.sleep(0.4)

# 1) Idle open/closed -> off (API-level verification)
results.append({'test': 'idle_off_closed_or_open', 'snapshot': snapshot('idle_check'), 'pass': snapshot('idle_check').get('pattern') == 'off'})

# 3) opening pattern
post('/api/control', {'action': 'open'})
ok, last = wait_pattern('opening', timeout=3.0)
results.append({'test': 'opening_pattern', 'pass': ok, 'last': last, 'snapshot': snapshot('after_open_cmd')})

# 4) closing pattern
post('/api/control', {'action': 'close'})
ok, last = wait_pattern('closing', timeout=3.0)
results.append({'test': 'closing_pattern', 'pass': ok, 'last': last, 'snapshot': snapshot('after_close_cmd')})

# 5) user stop pulse then off
post('/api/control', {'action': 'stop'})
ok1, last1 = wait_pattern('stopped', timeout=2.0)
ok2, last2 = wait_pattern('off', timeout=4.0)
results.append({'test': 'user_stop_then_off', 'pass': bool(ok1 and ok2), 'stopped_seen': ok1, 'off_seen': ok2, 'last': [last1, last2]})

# 6) learn mode pattern
post('/api/learn', {'enable': True})
ok, last = wait_pattern('learn', timeout=2.0)
results.append({'test': 'learn_mode_pattern', 'pass': ok, 'last': last})

# 7) remote added (simulated)
serial_add = int(time.time()) % 100000000
post('/api/test_remote', {'serial': serial_add, 'encript': 1, 'btnToggle': True, 'btnGreen': False})
ok, last = wait_pattern('remote_ok', timeout=2.0)
results.append({'test': 'remote_added_pattern', 'pass': ok, 'last': last, 'serial': serial_add})

# 8) remote rejected (simulated)
post('/api/learn', {'enable': False})
serial_rej = serial_add + 1
post('/api/test_remote', {'serial': serial_rej, 'encript': 1, 'btnToggle': True, 'btnGreen': False})
ok, last = wait_pattern('remote_reject', timeout=2.0)
results.append({'test': 'remote_rejected_pattern', 'pass': ok, 'last': last, 'serial': serial_rej})

# 9) obstacle pattern (simulation through LED override API)
post('/api/led', {'pattern': 'obstacle', 'overrideMs': 900})
ok, last = wait_pattern('obstacle', timeout=1.5)
results.append({'test': 'obstacle_pattern_simulated', 'pass': ok, 'last': last, 'note': 'simulated via /api/led override'})

# 10) error pattern (simulation through LED override API)
post('/api/led', {'pattern': 'error', 'overrideMs': 900})
ok, last = wait_pattern('error', timeout=1.5)
results.append({'test': 'error_pattern_simulated', 'pass': ok, 'last': last, 'note': 'simulated via /api/led override'})

# 11) after effects -> off
ok, last = wait_pattern('off', timeout=3.0)
results.append({'test': 'back_to_idle_off', 'pass': ok, 'last': last})

# 12) ota pattern (simulation through LED override API)
post('/api/led', {'pattern': 'ota', 'overrideMs': 900})
ok, last = wait_pattern('ota', timeout=1.5)
results.append({'test': 'ota_pattern_simulated', 'pass': ok, 'last': last, 'note': 'simulated via /api/led override'})

# clear
post('/api/led', {'pattern': 'off', 'overrideMs': 300})
post('/api/learn', {'enable': False})

print('LED TEST RESULTS')
for r in results:
    print(r)

passed = sum(1 for r in results if r.get('pass'))
print({'passed': passed, 'total': len(results)})
