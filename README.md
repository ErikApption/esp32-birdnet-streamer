# ESP32 BirdNet Streamer

ESP32-S3 project that captures audio from an I2S MEMS microphone and streams Opus-encoded audio over UDP for BirdNET processing. Includes a Python listener that receives the UDP stream and re-serves it as an HTTP Ogg/Opus stream for playback in VLC or other media players.

**Design priority: battery life over latency.** The ESP32 accumulates ~1 second of encoded audio and sends it in a single burst, allowing the WiFi radio to sleep between transmissions. The listener absorbs this burstiness with a playout buffer and delivers smooth, paced audio to HTTP clients. This is not a real-time system — expect 2–5 seconds of latency between live audio and playback.

## Features

- **I2S microphone input** — 48 kHz, 16-bit mono from INMP441 MEMS microphone
- **Opus encoding** — 64 kbps, 60ms frames, hardware-accelerated on ESP32-S3
- **Burst UDP transmission** — 1-second bursts maximize WiFi radio sleep for battery life
- **WiFi configuration** — captive portal (WiFiManager) for zero-code network setup
- **OTA updates** — push new firmware over the network without a USB cable
- **Sleep schedule** — active from 1 hour before sunrise until sunset, deep sleep overnight
- **Power monitoring** — battery and solar voltage telemetry embedded in audio packets
- **Python listener** — UDP receiver → HTTP Ogg/Opus stream with paced delivery for VLC

## Hardware

- ESP32-S3-DevKitC1 (N16R8)
- I2S MEMS microphone (e.g. INMP441, SPH0645, ICS-43434)

### Wiring the I2S Microphone

Most I2S MEMS microphones (INMP441, SPH0645, ICS-43434) have 6 pins. Connect them to the ESP32-S3 DevKitC as follows:

| Mic Pin | Function       | Wire   | ESP32-S3 GPIO | Notes                                                                       |
| ------- | -------------- | ------ | ------------- | --------------------------------------------------------------------------- |
| VDD     | Power          | Brown  | GPIO 10       | Powered from a GPIO pin (~1.4 mA). Do NOT use 5V — MEMS mics are 3.3V.     |
| GND     | Ground         | Black  | GND           |                                                                             |
| SCK     | Bit Clock      | Orange | GPIO 4        | Serial clock driven by ESP32                                                |
| WS      | Word Select    | Yellow | GPIO 5        | Frame/channel sync signal                                                   |
| SD      | Serial Data    | Red    | GPIO 6        | Audio data output from mic                                                  |
| L/R     | Channel select | Green  | GND           | Tie to GND for left channel, 3.3V for right channel. Code defaults to left. |

#### Wiring diagram (text)

```
ESP32-S3 DevKitC              I2S MEMS Mic
─────────────────             ────────────
GPIO 10 ──────────────────────  VDD  ← powered from GPIO
GND   ────────────────────────  GND
GPIO 4 ───────────────────────  SCK
GPIO 5 ───────────────────────  WS
GPIO 6 ───────────────────────  SD
GND   ────────────────────────  L/R  ← left channel
```

#### Tips

- Keep wires short (< 10 cm) to avoid noise on the clock lines.
- Add a 100 nF decoupling capacitor between VDD and GND as close to the mic as possible.
- If you hear silence, double-check that L/R (SEL) is tied to GND (left) — the firmware reads the left channel only.
- To use the right channel instead, change `I2S_CHANNEL_FMT_ONLY_LEFT` to `I2S_CHANNEL_FMT_ONLY_RIGHT` in `main.cpp` and tie L/R to 3.3V.
- The pin assignments can be changed by editing the `I2S_WS_PIN`, `I2S_SD_PIN`, and `I2S_SCK_PIN` defines at the top of `main.cpp`.

### Complete GPIO Pin Assignment Table

All ESP32-S3 GPIOs used in this project, including the I2S microphone and the battery/solar power monitoring circuit.

| GPIO | Function          | Direction  | Subsystem        | Notes                                                        |
|------|-------------------|------------|------------------|--------------------------------------------------------------|
| 4    | I2S_SCK (BCLK)   | OUTPUT     | I2S Microphone   | Bit clock to INMP441                                         |
| 5    | I2S_WS (LRCLK)   | OUTPUT     | I2S Microphone   | Word select / frame sync                                     |
| 6    | I2S_SD (DOUT)     | INPUT      | I2S Microphone   | Serial audio data from mic                                   |
| 7    | MONITOR_EN        | OUTPUT     | Power Monitor    | MOSFET gate — enables voltage dividers (10kΩ pull-down)      |
| 8    | VBAT_SENSE        | ADC INPUT  | Power Monitor    | Battery voltage via divider (ADC1_CH7, 11dB atten)           |
| 9    | VSOL_SENSE        | ADC INPUT  | Power Monitor    | Solar panel voltage via divider (ADC1_CH8, 11dB atten)       |
| 10   | MIC_POWER         | OUTPUT     | I2S Microphone   | Powers INMP441 VDD (~1.4 mA, software-controlled)            |


#### Pin selection rationale

- **GPIOs 4–6** — I2S peripheral pins, grouped sequentially for clean routing.
- **GPIO 7** — MOSFET gate for the zero-quiescent-current power monitor switch. Held LOW during deep sleep by 10kΩ pull-down resistor; no RTC config needed.
- **GPIOs 8–9** — ADC1 channels (CH7, CH8). ADC1 remains usable when WiFi is active (ADC2 is not). 11dB attenuation gives 0–3.1V input range.
- **GPIO 10** — Mic power. Allows software power-cycling of the INMP441 and draws zero current during deep sleep (pin goes Hi-Z).
- All selected pins are general-purpose on the ESP32-S3-DevKitC-1 (N16R8) with no conflicting boot-strapping or flash functions.

## Power Setup

- Source: 3S NiMh batteries
- Buck/Boost module for powering the ESP32 from the 3S batteries
- NiMh charger module for charging the batteries from a USB-C solar panel
- Power monitoring for solar panel input and battery voltage

## Build Instructions

### Prerequisites

- [PlatformIO](https://platformio.org/) (CLI or VS Code extension)
- USB-C cable for initial flash

### Build and Flash (USB)

```bash
# Clone the repository
git clone https://github.com/your-user/esp32-birdnet-streamer.git
cd esp32-birdnet-streamer

# Build the firmware
pio run

# Upload via USB (board must be connected)
pio run --target upload

# Open serial monitor
pio device monitor -b 115200
```

### First Boot — WiFi Setup

1. Power on the board. It will create a WiFi access point named **BirdNet-Setup**.
2. Connect to that AP from your phone or laptop.
3. A captive portal opens automatically. Fill in:
   - Your WiFi SSID and password
   - **UDP Host** — IP address or `.local` hostname of the machine running BirdNET
   - **UDP Port** — port BirdNET is listening on (default `4000`)
   - **Sample Rate** — audio sample rate in Hz (default `48000`, see allowed values below)
   - **Latitude** / **Longitude** — your location for sunrise/sunset (default: Ottawa, 45.4215 / -75.6972)
   - **UTC Offset** — timezone offset in hours (default: `-5` for EST)
4. Click Save. The board connects to your WiFi and begins streaming.

Credentials and parameters are stored in flash — subsequent boots connect automatically.

### Changing WiFi Network

If you need to connect the board to a different WiFi network:

1. **Automatic fallback** — If the saved network is unavailable (moved location, changed router, etc.), the board will fail to connect after a few seconds and automatically relaunch the **BirdNet-Setup** captive portal. Connect to that AP and enter the new credentials.

2. **Manual reset** — To force the portal even while the saved network is available:
   - **Option A (serial):** Open a serial terminal, trigger a WiFi reset by adding a call to `wm.resetSettings()` before `wm.autoConnect(...)` in the source, flash once, then remove it.
   - **Option B (erase flash):** Run the following PlatformIO command to wipe all saved settings (WiFi credentials + parameters):
     ```bash
     pio run --target erase
     pio run --target upload
     ```
     On next boot the portal will appear as if it were the first boot.

3. **Captive portal timeout** — The portal stays active for **3 minutes**. If no one connects and configures WiFi in that time, the board restarts and tries again. This prevents the board from being stuck in AP mode indefinitely.

### WiFi Tips

- The ESP32-S3 supports **2.4 GHz only** — it cannot connect to 5 GHz networks.
- Place the board within reasonable range of your access point. Check signal strength via `GET /status` (the `rssi` field, in dBm). Anything above −70 dBm is fine.
- If the board repeatedly fails to connect, check for MAC filtering on your router or try a simpler SSID (avoid special characters).
- The board's hostname on the network is `esp32-birdnet` — you can find it in your router's DHCP client list or via mDNS as `esp32-birdnet.local`.

### Build-Time WiFi Credentials (Skip Captive Portal)

If you want the board to connect to a known network without going through the captive portal, you can bake the credentials into the firmware at compile time.

In `platformio.ini`, uncomment and edit these lines:

```ini
build_flags =
    -DBOARD_HAS_PSRAM
    -DWIFI_SSID=\"YourNetworkName\"
    -DWIFI_PASSWORD=\"YourPassword\"
```

Then build and flash:

```bash
pio run --target upload
```

When `WIFI_SSID` and `WIFI_PASSWORD` are defined, the board connects directly on boot — no AP, no portal. Other parameters (UDP host, sample rate, location, etc.) are still configurable via the HTTP API at runtime.

To go back to captive portal mode, simply comment out or remove the two `-D` flags and reflash.

## OTA (Over-The-Air) Updates

Once the board is connected to your WiFi network you can push new firmware without USB.

### Using PlatformIO CLI

```bash
# Upload over the network (replace IP with your board's IP)
pio run --target upload --upload-port 192.168.1.xxx
```

Or add an OTA environment to `platformio.ini`:

```ini
[env:ota]
extends = env:esp32-s3-devkitc1-n16r8
upload_protocol = espota
upload_port = 192.168.1.xxx   ; replace with board IP
upload_flags =
    --port=3232
```

Then simply:

```bash
pio run -e ota --target upload
```

### Using Arduino IDE

1. In **Tools > Port**, select the network port labelled `esp32-birdnet (192.168.x.x)`.
2. Upload as normal.

### Finding the Board's IP

- Check your router's DHCP client list for hostname `esp32-birdnet`.
- Or read it from the serial monitor at boot:
  ```
  [WiFi] IP: 192.168.1.42
  ```

### OTA Notes

- OTA is only available while the board is awake (sunrise−1h to sunset).
- If the board is deep-sleeping you must wait until the next active window, or power-cycle it to trigger a fresh boot.
- No authentication is configured by default. To add a password, call `ArduinoOTA.setPassword("your-password")` in `otaInit()`.

## Sleep Schedule

The board calculates local sunrise and sunset times using the configured coordinates. It is active from **1 hour before sunrise** until **sunset**, then enters deep sleep until the next morning's active window.

During deep sleep the ESP32-S3 draws ~7 µA, making this suitable for solar/battery deployments.

## mDNS (Network Discovery)

The board advertises itself on the local network via mDNS (Bonjour/Avahi) as:

```
esp32-birdnet.local
```

This means you can:

- Access the config page at `http://esp32-birdnet.local/config`
- Push OTA updates to `esp32-birdnet.local`
- Use mDNS hostnames as the UDP target (e.g. `my-server.local`)

### Advertised Services

| Service         | Protocol | Port            | Description            |
| --------------- | -------- | --------------- | ---------------------- |
| `_http._tcp`    | TCP      | 80              | HTTP configuration API |
| `_birdnet._udp` | UDP      | configured port | Audio stream source    |

### Using mDNS for the UDP Target

Instead of a numeric IP, you can set the UDP Host to a `.local` hostname in the captive portal or via the HTTP API:

```bash
# Point audio at a machine advertising itself as "birdnet-server.local"
curl -X POST "http://esp32-birdnet.local/config" \
     -d "udp_host=birdnet-server.local"
```

The board resolves `.local` names using mDNS and caches the result, re-resolving every 60 seconds. Regular hostnames fall back to DNS.

### Requirements

mDNS works out of the box on:
- macOS (Bonjour built-in)
- Linux (install `avahi-daemon` if not already present)
- Windows 10+ (mDNS support built-in for `.local` domains)

## Remote Configuration API

The board runs a lightweight HTTP server on port 80 for runtime configuration.

### GET /status

Returns current device status:

```bash
curl http://esp32-birdnet.local/status
```

```json
{
  "time": "2025-06-15 07:23:01",
  "sunrise": "05:14",
  "sunset": "20:51",
  "sleep_enabled": true,
  "ip": "192.168.1.42",
  "rssi": -54
}
```

### GET /config

Returns current configuration:

```bash
curl http://esp32-birdnet.local/config
```

```json
{
  "udp_host": "192.168.1.100",
  "udp_port": 4000,
  "latitude": "45.4215",
  "longitude": "-75.6972",
  "utc_offset": "-5",
  "sleep_enabled": true
}
```

### POST /config

Update one or more parameters. Changes are saved to flash immediately.

```bash
# Change UDP target
curl -X POST "http://esp32-birdnet.local/config" \
     -d "udp_host=birdnet-server.local&udp_port=5000"

# Change sample rate (triggers automatic reboot)
curl -X POST "http://esp32-birdnet.local/config" \
     -d "sample_rate=48000"

# Disable sleep (board stays on 24/7)
curl -X POST "http://esp32-birdnet.local/config" \
     -d "sleep_enabled=false"

# Re-enable sleep
curl -X POST "http://esp32-birdnet.local/config" \
     -d "sleep_enabled=true"
```

Available parameters: `udp_host`, `udp_port`, `sample_rate`, `latitude`, `longitude`, `utc_offset`, `sleep_enabled` (true/false).

Allowed sample rates: `8000`, `16000`, `22050`, `32000`, `44100`, `48000` Hz. Changing the sample rate requires an I2S driver reinit, so the board reboots automatically.

## UDP Audio Protocol

The ESP32 sends Opus-encoded audio in a custom lightweight framing format designed for low-overhead UDP transport.

### Packet Format

```
[Header: 4 bytes][Frame 1][Frame 2]...[Frame N][Optional Telemetry Trailer: 22 bytes]

Header:
  Byte 0-1: Magic "OP" (0x4F 0x50)
  Byte 2:   Frame count (number of Opus frames in this packet)
  Byte 3:   Sequence number (0-255, wrapping)

Each Frame:
  2 bytes:  Frame length (big-endian)
  N bytes:  Opus-encoded audio data

Telemetry Trailer (optional, appended to last packet in burst):
  Byte 0-1: Magic "TL" (0x54 0x4C)
  Byte 2-3: Battery ADC mV (big-endian)
  Byte 4-5: Solar ADC mV (big-endian)
  Byte 6-7: WiFi RSSI int16 (big-endian)
  Byte 8-11: Total packets sent (big-endian)
  Byte 12-15: Total send errors (big-endian)
  Byte 16-19: Total packets dropped (big-endian)
  Byte 20:  WiFi connected (1/0)
  Byte 21:  Consecutive send failures (capped at 255)
```

### Encoding Parameters

| Parameter        | Value                          |
| ---------------- | ------------------------------ |
| Codec            | Opus (OPUS_APPLICATION_AUDIO)  |
| Sample rate      | 48000 Hz                       |
| Channels         | 1 (mono)                       |
| Frame duration   | 60 ms                          |
| Bitrate          | 64 kbps                        |
| Frames per packet| 4 (240 ms of audio per packet) |
| Burst interval   | 1000 ms                        |
| DTX              | Enabled (saves bandwidth in silence) |

### Streaming Architecture

```
┌─────────────┐   UDP bursts    ┌────────────────┐   HTTP (paced)   ┌─────┐
│   ESP32-S3  │ ──── ~1s ────→  │ Python Listener│ ──── smooth ──→  │ VLC │
│  (battery)  │   4 packets/s   │  (always-on)   │   60ms/frame     │     │
└─────────────┘                  └────────────────┘                  └─────┘
      │                                  │
      │ Sleeps radio between             │ Absorbs bursts into
      │ bursts (~750ms/s idle)           │ playout buffer, meters
      │                                  │ frames out at real-time
      └──── Battery life priority        └──── Smooth playback for VLC
```

The listener's `/stream.opus` endpoint delivers Ogg/Opus at real-time rate regardless of how the data arrived from UDP. This decouples the ESP32's power-saving burst strategy from the playback client's timing requirements.

## Listener

The Python listener (`listener/`) receives UDP audio from the ESP32 and serves it as an HTTP Ogg/Opus stream.

### Quick Start

```bash
cd listener
poetry install
poetry run birdnet-listener
```

### Endpoints

| Endpoint       | Format    | Description                                      |
| -------------- | --------- | ------------------------------------------------ |
| `/stream.opus` | Ogg/Opus  | **Primary.** Paced delivery, works with VLC.     |
| `/stream.m3u`  | M3U       | Playlist pointing to `/stream.opus`              |
| `/stream`      | WAV/PCM   | Legacy. No pacing — may buffer in VLC.           |
| `/`            | JSON      | Status and diagnostics                           |

### VLC Playback

Open the playlist URL in VLC:

```bash
vlc http://localhost:8086/stream.m3u
```

Or directly:

```bash
vlc http://localhost:8086/stream.opus --network-caching=5000
```

The `--network-caching=5000` flag gives VLC a 5-second buffer to absorb any residual jitter. With the listener's paced delivery this usually isn't needed, but it provides extra margin.

## License

See [LICENSE](LICENSE) for details.
