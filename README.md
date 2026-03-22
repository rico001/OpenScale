# OpenTrackFit

ESP32-based bridge that reads weight data from a Bluetooth (BLE) body composition scale and makes it available via a local web interface, MQTT, and HTTP webhooks.

## Supported Hardware

- **Microcontroller**: ESP32-WROOM-32 (any ESP32 dev board)

### Compatible Scales

OpenTrackFit works with BLE body composition scales that use the **ElinkThings/SWAN platform** (BLE service `0xFFB0`). Many affordable smart scales from different brands share this common hardware and protocol — they are manufactured by the same OEM in China and sold under various names.

**Tested:**
- FitTrack Dara (Model: FT-DARA-WH01-GL) — BLE chip: Dialog Semi DA14531

**Expected to work** (same platform, untested):
- GAIAM Smart Weight Scale
- MGB / Icomon scales (SWAN protocol)
- Other scales advertising as `"FitTrack"` via BLE

> If your scale uses BLE service `0xFFB0` with characteristics `FFB1`/`FFB2`/`FFB3`, it likely works. You may need to adjust the `SCALE_NAME` constant in `src/main.cpp` to match your scale's BLE advertised name.

## Features

- Automatic BLE connection to scale when someone steps on it
- Last measurement displayed on a responsive web page with body composition tiles
- **Body composition analysis**: BMI, body fat %, muscle mass, water %, bone mass, BMR, protein %, metabolic age, visceral fat, and more — calculated from weight + user profile
- **Multi-profile support**: Create up to 8 user profiles (name, gender, age, height) — active profile shown on home page
- WiFi configuration via captive portal with SSID scan (no hardcoded credentials)
- MQTT publishing of full body composition data to any broker
- HTTP POST webhook forwarding with all measurement data
- Settings page for profiles, WiFi, MQTT, and HTTP webhook configuration
- REST API for external systems to poll measurement data
- NTP time sync (CET/CEST timezone)
- Auto-reconnect after scale powers off
- mDNS support (`http://opentrackfit.local`)

## Getting Started

### Prerequisites

- [PlatformIO](https://platformio.org/) (CLI or IDE plugin)
- ESP32 dev board connected via USB

### Build & Flash

```bash
pio run -t upload
```

### Serial Monitor

```bash
pio device monitor
```

> **Note**: Close the serial monitor before flashing, otherwise the upload will fail.

### Initial WiFi Setup

1. On first boot, the ESP32 creates a WiFi access point:
   - **SSID**: `OpenTrackFit`
   - **Password**: `12345678`
2. Connect to the AP and open `http://192.168.4.1`
3. Select your home WiFi network from the scan list (or enter manually) and enter the password
4. The ESP32 tests the connection and shows success or failure
5. On success, the ESP32 restarts and joins your network

### Usage

1. Open `http://opentrackfit.local` (or the IP shown in the serial log)
2. Step on the scale — the weight measurement begins automatically
3. The final weight and timestamp are displayed after the measurement stabilizes

| Home | Settings |
|------|----------|
| ![Home Page](resources/home.png) | ![Settings Page](resources/settings.png) |

### Settings

Navigate to `/setup` to configure:

- **Profile** — User profiles with name, gender, age, height (select active profile)
- **WLAN** — WiFi credentials (triggers reconnect)
- **MQTT** — Broker, port, topic, user/password (saved inline, no reconnect)
- **HTTP Webhook** — POST URL for weight data forwarding (saved inline)

### REST API

| Endpoint | Description |
|----------|-------------|
| `GET /api/last-weight-data` | Last measurement with body composition (JSON) |
| `GET /api/settings` | Current settings and profiles |
| `GET /api/docs` | API documentation |

Example response:
```json
{
  "weight": 102.1,
  "time": "22.03.2026 18:37",
  "profile": "Peter",
  "bmi": 26.9,
  "body_fat_pct": 24.3,
  "muscle_pct": 68.1,
  "water_pct": 55.3,
  "bone_mass": 3.5,
  "bmr": 2064,
  "protein_pct": 17.4,
  "metabolic_age": 39,
  "visceral_fat": 10,
  "subcutaneous_fat_pct": 20.3,
  "ideal_weight": 83.7,
  "weight_control": 18.4,
  "fat_mass": 24.8,
  "fat_free_weight": 77.3,
  "muscle_mass": 69.6,
  "protein_mass": 17.8
}
```

## Serial Output

```
=== OpenTrackFit ===
Mode:  LAN (Station)
IP:    192.168.1.42
mDNS:  http://opentrackfit.local
-----------------------------
FitTrack found!
Connecting to 03:b3:ec:c1:cf:24...
Connected. Waiting for measurement...

  Measuring: 102.6 kg
  Measuring: 102.7 kg
>>> FINAL WEIGHT: 102.7 kg <<<
MQTT published to opentrackfit/weight
HTTP POST https://example.com/webhook -> 200
>> Scale disconnected. Rescanning...
```

## WiFi Behavior

| Situation | Behavior |
|-----------|----------|
| First boot (no saved WiFi) | Starts AP mode for configuration |
| Saved WiFi available | Connects to WiFi (STA mode) |
| WiFi lost for >60 seconds | Falls back to AP mode for 5 minutes |
| AP mode timeout (5 min) | Retries saved WiFi credentials |

## Development

This project was predominantly built using [Claude Code](https://claude.ai/claude-code) (AI-assisted development).

## License

MIT
