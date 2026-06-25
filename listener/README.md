# BirdNet Listener

A Python service that receives the raw PCM audio stream from the ESP32 BirdNet Streamer over UDP and re-serves it as an HTTP audio stream. Media players can connect using the provided M3U playlist URL.

## Requirements

- Python 3.10+
- Poetry

## Power Consumption

0.115 A 5V

## Installation

```bash
cd listener
poetry install
```

## Usage

```bash
poetry run birdnet-listener
```

On startup the listener will:

1. Discover the ESP32 via mDNS (looks for `_birdnet._udp.local.`)
2. Push its own IP and UDP port to the ESP32's `/config` API so the device knows where to stream audio
3. Start receiving UDP audio and serving it over HTTP

### CLI Options

```
--udp-port       UDP port to listen on (default: 4000)
--http-port      HTTP port to serve on (default: 8080)
--sample-rate    Audio sample rate in Hz (default: 48000)
--esp32-ip       ESP32 IP address (skips mDNS discovery)
--listener-ip    Override the local IP sent to the ESP32
--skip-discovery Skip mDNS discovery and config push entirely
--log-level      Logging level: debug, info, warning, error
```

### Environment Variables

| Variable | Description |
|----------|-------------|
| `BIRDNET_ESP32_IP` | Override the ESP32 IP address (skips mDNS discovery) |
| `BIRDNET_LISTENER_IP` | Override the local IP address sent to the ESP32 |

Environment variables take precedence over CLI arguments.

## Endpoints

| Endpoint | Description |
|----------|-------------|
| `GET /` | Service info (JSON) |
| `GET /stream.m3u` | M3U playlist file pointing to the audio stream |
| `GET /stream` | Raw audio stream (PCM 16-bit wrapped in WAV headers) |

## Connecting a Media Player

Open `http://<your-ip>:8080/stream.m3u` in VLC, foobar2000, or any media player that supports M3U playlists and WAV/PCM streams.

## Example

```bash
# Auto-discover ESP32 on the network
poetry run birdnet-listener

# Or provide the ESP32 IP explicitly
BIRDNET_ESP32_IP=192.168.1.50 poetry run birdnet-listener

# Override everything
poetry run birdnet-listener --esp32-ip 192.168.1.50 --listener-ip 192.168.1.100 --udp-port 4000
```
