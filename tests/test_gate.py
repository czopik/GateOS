#!/usr/bin/env python3
"""
GateOS Automated Test Suite

Tests:
1. Position accuracy and repeatability
2. Open/close cycle timing
3. UART communication stress
4. Safety fault injection
5. Web API responsiveness
"""

import requests
import websocket
import json
import time
import sys
import statistics
from datetime import datetime

# Configuration
BASE_URL = "http://gate.local"  # or IP address
WS_URL = f"ws://{BASE_URL.split('://')[1]}/ws"
TEST_ITERATIONS = 10
POSITION_TOLERANCE_MM = 50  # 5cm tolerance

class GateTester:
    def __init__(self, base_url):
        self.base_url = base_url.rstrip('/')
        self.ws = None
        self.results = []
        self.position_history = []
        
    def connect(self):
        """Establish WebSocket connection"""
        try:
            self.ws = websocket.create_connection(f"ws://{self.base_url.split('://')[1]}/ws", timeout=5)
            print(f"[OK] WebSocket connected to {self.base_url}")
            return True
        except Exception as e:
            print(f"[FAIL] WebSocket connection failed: {e}")
            return False
    
    def disconnect(self):
        """Close WebSocket connection"""
        if self.ws:
            self.ws.close()
    
    def send_command(self, cmd):
        """Send gate command via REST API"""
        try:
            resp = requests.post(f"{self.base_url}/api/control", 
                               json={"action": cmd}, timeout=5)
            return resp.status_code == 200
        except Exception as e:
            print(f"[ERROR] Command failed: {e}")
            return False
    
    def get_status(self):
        """Get current gate status"""
        try:
            resp = requests.get(f"{self.base_url}/api/status", timeout=5)
            if resp.status_code == 200:
                return resp.json()
        except:
            pass
        return None
    
    def wait_for_state(self, target_state, timeout_s=30):
        """Wait for gate to reach target state"""
        start = time.time()
        while time.time() - start < timeout_s:
            status = self.get_status()
            if status and status.get('state') == target_state:
                return True
            time.sleep(0.1)
        return False
    
    def wait_for_movement_stop(self, timeout_s=30):
        """Wait for gate to stop moving"""
        start = time.time()
        while time.time() - start < timeout_s:
            status = self.get_status()
            if status and not status.get('moving', True):
                return status
            time.sleep(0.1)
        return None
    
    def test_open_close_cycle(self):
        """Test complete open/close cycle"""
        print("\n=== Test: Open/Close Cycle ===")
        
        # Start from closed position
        self.send_command("stop")
        time.sleep(0.5)
        
        # Open
        t0 = time.time()
        self.send_command("open")
        if not self.wait_for_state("OPENING", timeout_s=2):
            print("[FAIL] Gate did not start opening")
            return False
        
        status = self.wait_for_movement_stop(timeout_s=60)
        open_time = time.time() - t0
        
        if not status:
            print("[FAIL] Opening timeout")
            return False
        
        open_pos = status.get('position', 0)
        print(f"[OK] Opened in {open_time:.2f}s, position={open_pos:.3f}m")
        
        # Close
        t0 = time.time()
        self.send_command("close")
        if not self.wait_for_state("CLOSING", timeout_s=2):
            print("[FAIL] Gate did not start closing")
            return False
        
        status = self.wait_for_movement_stop(timeout_s=60)
        close_time = time.time() - t0
        
        if not status:
            print("[FAIL] Closing timeout")
            return False
        
        close_pos = status.get('position', 999)
        print(f"[OK] Closed in {close_time:.2f}s, position={close_pos:.3f}m")
        
        # Verify positions
        if close_pos > 0.1:  # Should be near zero
            print(f"[WARN] Close position offset: {close_pos:.3f}m")
        
        return True
    
    def test_position_repeatability(self, iterations=5):
        """Test position repeatability over multiple cycles"""
        print(f"\n=== Test: Position Repeatability ({iterations} cycles) ===")
        
        positions = []
        
        for i in range(iterations):
            print(f"\nCycle {i+1}/{iterations}")
            
            # Full open
            self.send_command("open")
            status = self.wait_for_movement_stop(timeout_s=60)
            if not status:
                print(f"[FAIL] Cycle {i+1}: Opening timeout")
                return False
            
            open_pos = status.get('position', 0)
            positions.append(open_pos)
            print(f"  Open position: {open_pos:.3f}m")
            
            # Full close
            self.send_command("close")
            status = self.wait_for_movement_stop(timeout_s=60)
            if not status:
                print(f"[FAIL] Cycle {i+1}: Closing timeout")
                return False
            
            time.sleep(0.5)
        
        # Analyze results
        if len(positions) < 2:
            print("[FAIL] Not enough data points")
            return False
        
        mean_pos = statistics.mean(positions)
        stdev_pos = statistics.stdev(positions) if len(positions) > 1 else 0
        min_pos = min(positions)
        max_pos = max(positions)
        
        print(f"\n=== Results ===")
        print(f"Mean position:   {mean_pos:.3f}m")
        print(f"Std deviation:   {stdev_pos*1000:.1f}mm")
        print(f"Min position:    {min_pos:.3f}m")
        print(f"Max position:    {max_pos:.3f}m")
        print(f"Range:           {(max_pos-min_pos)*1000:.1f}mm")
        
        if stdev_pos * 1000 > POSITION_TOLERANCE_MM:
            print(f"[FAIL] Position variance exceeds tolerance ({POSITION_TOLERANCE_MM}mm)")
            return False
        
        print(f"[OK] Position repeatability within tolerance")
        return True
    
    def test_uart_stress(self, num_commands=100):
        """Stress test UART communication"""
        print(f"\n=== Test: UART Stress ({num_commands} commands) ===")
        
        status = self.get_status()
        if not status:
            print("[FAIL] Cannot get initial status")
            return False
        
        initial_rx = status.get('uartRxLines', 0)
        initial_bad = status.get('uartBadLines', 0)
        
        t0 = time.time()
        success = 0
        failures = 0
        
        for i in range(num_commands):
            if self.send_command("get"):
                success += 1
            else:
                failures += 1
            
            if i % 10 == 0:
                print(f"  Progress: {i}/{num_commands}")
        
        duration = time.time() - t0
        
        # Check final stats
        status = self.get_status()
        if status:
            final_rx = status.get('uartRxLines', 0)
            final_bad = status.get('uartBadLines', 0)
            
            rx_delta = final_rx - initial_rx
            bad_delta = final_bad - initial_bad
            error_rate = (bad_delta / rx_delta * 100) if rx_delta > 0 else 0
            
            print(f"\n=== Results ===")
            print(f"Duration:        {duration:.2f}s")
            print(f"Commands sent:   {success + failures}")
            print(f"Success rate:    {success/(success+failures)*100:.1f}%")
            print(f"RX lines delta:  {rx_delta}")
            print(f"Bad lines delta: {bad_delta}")
            print(f"Error rate:      {error_rate:.2f}%")
            
            if error_rate > 1.0:
                print(f"[FAIL] UART error rate too high")
                return False
        
        print(f"[OK] UART stress test passed")
        return True
    
    def test_web_api_latency(self, num_requests=20):
        """Test web API response latency"""
        print(f"\n=== Test: Web API Latency ({num_requests} requests) ===")
        
        latencies = []
        
        for i in range(num_requests):
            t0 = time.time()
            status = self.get_status()
            latency = (time.time() - t0) * 1000  # ms
            
            if status:
                latencies.append(latency)
            else:
                print(f"  Request {i+1}: TIMEOUT")
        
        if len(latencies) < num_requests * 0.8:
            print(f"[FAIL] Too many timeouts")
            return False
        
        mean_lat = statistics.mean(latencies)
        p95_lat = sorted(latencies)[int(len(latencies) * 0.95)]
        max_lat = max(latencies)
        
        print(f"\n=== Results ===")
        print(f"Mean latency:    {mean_lat:.1f}ms")
        print(f"P95 latency:     {p95_lat:.1f}ms")
        print(f"Max latency:     {max_lat:.1f}ms")
        
        if mean_lat > 100:
            print(f"[WARN] Mean latency higher than expected")
        
        print(f"[OK] Web API responsive")
        return True
    
    def test_safety_obstacle(self):
        """Test obstacle detection (requires physical test)"""
        print("\n=== Test: Safety Obstacle Detection ===")
        print("[MANUAL TEST REQUIRED]")
        print("1. Place obstacle in gate path")
        print("2. Send 'close' command")
        print("3. Verify gate stops and reverses")
        print("4. Remove obstacle")
        print("5. Verify normal operation resumes")
        
        response = input("Did obstacle detection work correctly? (y/n): ")
        return response.lower() == 'y'
    
    def run_all_tests(self):
        """Run complete test suite"""
        print("=" * 60)
        print("GateOS Automated Test Suite")
        print(f"Started: {datetime.now().isoformat()}")
        print("=" * 60)
        
        if not self.connect():
            print("[ABORT] Cannot connect to gate controller")
            return False
        
        results = {}
        
        try:
            results['open_close'] = self.test_open_close_cycle()
            results['repeatability'] = self.test_position_repeatability(TEST_ITERATIONS)
            results['uart_stress'] = self.test_uart_stress(100)
            results['api_latency'] = self.test_web_api_latency(20)
            results['safety'] = self.test_safety_obstacle()
            
        except KeyboardInterrupt:
            print("\n[ABORT] Tests interrupted by user")
        except Exception as e:
            print(f"\n[ERROR] Test suite failed: {e}")
        finally:
            self.disconnect()
        
        # Summary
        print("\n" + "=" * 60)
        print("TEST SUMMARY")
        print("=" * 60)
        
        passed = sum(1 for v in results.values() if v)
        total = len(results)
        
        for name, result in results.items():
            status = "PASS" if result else "FAIL"
            print(f"  {name:20} [{status}]")
        
        print(f"\nTotal: {passed}/{total} tests passed")
        
        if passed == total:
            print("\n[SUCCESS] All tests passed!")
            return True
        else:
            print(f"\n[WARNING] {total - passed} test(s) failed")
            return False


def main():
    import argparse
    
    parser = argparse.ArgumentParser(description='GateOS Test Suite')
    parser.add_argument('--url', default=BASE_URL, help=f'Base URL (default: {BASE_URL})')
    parser.add_argument('--iterations', type=int, default=TEST_ITERATIONS, 
                       help='Number of test iterations')
    parser.add_argument('--test', choices=['all', 'cycle', 'repeatability', 'uart', 'latency'],
                       default='all', help='Specific test to run')
    
    args = parser.parse_args()
    
    tester = GateTester(args.url)
    global TEST_ITERATIONS
    TEST_ITERATIONS = args.iterations
    
    if args.test == 'all':
        success = tester.run_all_tests()
    else:
        if not tester.connect():
            sys.exit(1)
        
        try:
            if args.test == 'cycle':
                success = tester.test_open_close_cycle()
            elif args.test == 'repeatability':
                success = tester.test_position_repeatability(args.iterations)
            elif args.test == 'uart':
                success = tester.test_uart_stress(100)
            elif args.test == 'latency':
                success = tester.test_web_api_latency(20)
        finally:
            tester.disconnect()
    
    sys.exit(0 if success else 1)


if __name__ == '__main__':
    main()
