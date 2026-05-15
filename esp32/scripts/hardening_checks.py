#!/usr/bin/env python3

from __future__ import annotations

import pathlib
import re
import sys


ROOT = pathlib.Path(__file__).resolve().parents[2]


def read_text(relative_path: str) -> str:
    return (ROOT / relative_path).read_text(encoding="utf-8")


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    config_h = read_text("esp32/Src/config_manager.h")
    config_cpp = read_text("esp32/Src/config_manager.cpp")
    web_server_cpp = read_text("esp32/Src/web_server.cpp")

    expect("struct SecurityConfig {\n  bool enabled = true;" in config_h,
           "SecurityConfig should enable API auth by default")

    expect('error = "wifi.apFallback.password_too_short";' in config_cpp,
           "Config validation should reject short AP fallback passwords outside bench mode")
    expect('error = "ota.password_required_in_production";' in config_cpp,
           "Production validation should reject OTA without password")
    expect('error = "security.enabled_required_in_production";' in config_cpp,
           "Production validation should require API security")
    expect('error = "security.apiToken_required_outside_bench";' in config_cpp,
           "Config validation should require API token outside bench when security is enabled")

    wifi_handler = re.search(
        r'server\.on\("/api/wifi", HTTP_POST, \[this\]\(AsyncWebServerRequest \*request\)\{(?P<body>.*?)\n  \}\);',
        web_server_cpp,
        re.S,
    )
    expect(wifi_handler is not None, "Expected /api/wifi HTTP_POST handler")
    wifi_body = wifi_handler.group("body") if wifi_handler else ""
    expect("isAuthorized(request)" in wifi_body,
           "/api/wifi should require auth")
    expect('"{\\"status\\":\\"not_implemented\\"}"' in wifi_body,
           "/api/wifi should return not_implemented JSON")

    ws_auth = re.search(r'bool WebServerManager::isWebSocketAuthorized\(AsyncWebServerRequest\* request\) const \{(?P<body>.*?)\n\}', web_server_cpp, re.S)
    expect(ws_auth is not None, "Expected dedicated WebSocket auth helper")
    ws_auth_body = ws_auth.group("body") if ws_auth else ""
    expect('request->hasParam("token")' in ws_auth_body,
           "WebSocket auth helper should accept token query parameter")
    expect('request->hasHeader("X-Api-Key")' in ws_auth_body,
           "WebSocket auth helper should still accept API key header when available")

    ws_connect = re.search(r'if \(type == WS_EVT_CONNECT\) \{(?P<body>.*?)\n    \}', web_server_cpp, re.S)
    expect(ws_connect is not None, "Expected WS connect handler")
    ws_body = ws_connect.group("body") if ws_connect else ""
    expect("if (!isWebSocketAuthorized(request)) {" in ws_body and "client->close();" in ws_body,
           "Unauthorized WebSocket clients should be closed immediately")
    expect(ws_body.index("client->close();") < ws_body.index("client->text(payload);"),
           "Unauthorized WebSocket path must happen before initial status send")

    ota_handler = re.search(r'server\.on\("/api/ota/upload", HTTP_POST,(?P<body>.*?)\n  \);', web_server_cpp, re.S)
    expect(ota_handler is not None, "Expected /api/ota/upload handler")
    ota_body = ota_handler.group("body") if ota_handler else ""
    expect("scheduleRestart(300);" in ota_body,
           "OTA HTTP upload should schedule restart instead of restarting inline")
    expect("delay(300);" not in ota_body and "ESP.restart();" not in ota_body,
           "OTA HTTP upload should not block in the request callback")

    print("hardening_checks: OK")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as exc:
        print(f"hardening_checks: FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)