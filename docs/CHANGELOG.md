# CHANGELOG

## 2026-02-19 - OTA Wi-Fi (diagnosis + fix)

- Diagnostyka OTA:
  - potwierdzono brak gotowego profilu `espota` w `platformio.ini` (USB-only workflow),
  - brak pełnej telemetrii OTA w `/api/status` i `/api/diagnostics`,
  - brak twardej blokady `/api/control` podczas OTA.

- Zmiany OTA (minimalne):
  - dodano profil `env:esp32_ota` do uploadu firmware i `uploadfs` po Wi‑Fi,
  - OTA domyślnie wyłączone (`ota.enabled=false`),
  - rozszerzono status API o `ota.enabled`, `ota.active`, `ota.progress`, `ota.error`,
  - dodano logi startowe OTA (hostname, IP, port, ready),
  - dodano safety gating: stop napędu i blokada ruchu podczas OTA,
  - dodano blokadę `/api/control` podczas OTA (`423 ota_active`),
  - dodano UI banner „OTA wlaczone” + status OTA,
  - dodano skrypt `scripts/ota_smoke_test.py`.

- Kompatybilność:
  - brak zmian w logice sterowania bramą poza safety gatingiem OTA,
  - brak zmian schematu partycji (mniejsze ryzyko regresji).

