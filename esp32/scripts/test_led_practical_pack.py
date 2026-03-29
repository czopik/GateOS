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


def wait_pattern(expected, timeout=2.5):
    t0 = time.time()
    last = None
    while time.time() - t0 < timeout:
        try:
            last = jget('/api/led').get('pattern')
            if last == expected:
                return True, last
        except Exception:
            pass
        time.sleep(0.12)
    return False, last


results = []

# COMMAND_REJECTED: reset while moving (clearError blocked when moving)
post('/api/control', {'action': 'open'})
time.sleep(0.25)
post('/api/control', {'action': 'reset'})
ok, last = wait_pattern('command_rejected', timeout=2.0)
results.append({'test': 'command_rejected_on_blocked_reset', 'pass': ok, 'last': last})
post('/api/control', {'action': 'stop'})
time.sleep(0.4)

# Limit event pattern visibility (override simulation)
post('/api/led', {'pattern': 'limit_close_hit', 'overrideMs': 450})
ok, last = wait_pattern('limit_close_hit', timeout=1.2)
results.append({'test': 'limit_close_hit_pattern_visible', 'pass': ok, 'last': last, 'note': 'override simulation'})

post('/api/led', {'pattern': 'limit_open_hit', 'overrideMs': 450})
ok, last = wait_pattern('limit_open_hit', timeout=1.2)
results.append({'test': 'limit_open_hit_pattern_visible', 'pass': ok, 'last': last, 'note': 'override simulation'})

# ring orientation runtime set/readback
post('/api/led', {'ringStartIndex': 3, 'ringReverse': True})
led = jget('/api/led')
results.append({
    'test': 'ring_orientation_runtime_set',
    'pass': (led.get('ringStartIndex') == 3 and led.get('ringReverse') is True),
    'ringStartIndex': led.get('ringStartIndex'),
    'ringReverse': led.get('ringReverse')
})
post('/api/led', {'ringStartIndex': 0, 'ringReverse': False})

# brightness runtime flow + applied readback
post('/api/led', {'brightness': 35})
time.sleep(0.25)
led = jget('/api/led')
results.append({
    'test': 'brightness_set_and_readback',
    'pass': led.get('brightness') == 35,
    'brightness': led.get('brightness'),
    'brightnessApplied': led.get('brightnessApplied')
})

# cleanup
post('/api/led', {'mode': 'status', 'pattern': 'off', 'overrideMs': 250})

print('LED PRACTICAL PACK RESULTS')
for r in results:
    print(r)
print({'passed': sum(1 for r in results if r.get('pass')), 'total': len(results)})
