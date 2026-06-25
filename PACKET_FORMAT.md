# BirdNet Streamer UDP Packet Format

## Overview

The ESP32 BirdNet Streamer sends Opus-encoded audio over UDP using a custom lightweight framing protocol. Each UDP packet contains one or more Opus frames bundled together to reduce WiFi overhead.

## Packet Structure

```
+--------+--------+-------------+----------+
| Offset | Size   | Field       | Value    |
+--------+--------+-------------+----------+
| 0      | 1 byte | Magic[0]    | 0x4F 'O' |
| 1      | 1 byte | Magic[1]    | 0x50 'P' |
| 2      | 1 byte | Frame count | 1–255    |
| 3      | 1 byte | Sequence    | 0–255    |
+--------+--------+-------------+----------+
| 4+     | variable | Frame data (repeated frame_count times) |
+--------+--------+-------------+----------+
```

Each frame entry within the payload:

```
+--------+--------+---------------------------+
| Offset | Size   | Field                     |
+--------+--------+---------------------------+
| 0      | 2 bytes| Frame length (big-endian) |
| 2      | N bytes| Opus encoded frame data   |
+--------+--------+---------------------------+
```

## Header Fields

| Field | Description |
|-------|-------------|
| Magic | `0x4F 0x50` ("OP") — used for packet validation. Discard packets that don't start with this. |
| Frame count | Number of Opus frames in this packet. Typically 4 (configurable via `OPUS_FRAMES_PER_PACKET`). |
| Sequence | Wrapping 8-bit counter (0–255). Increments by 1 per packet. Used to detect dropped or reordered packets. |

## Encoding Parameters

| Parameter | Default Value |
|-----------|---------------|
| Sample rate | 48000 Hz |
| Channels | 1 (mono) |
| Frame duration | 60 ms |
| Samples per frame | 2880 (48000 × 0.060) |
| Bitrate | 64000 bps |
| Application | OPUS_APPLICATION_AUDIO |
| Signal type | OPUS_SIGNAL_MUSIC |
| DTX | Enabled |
| Frames per packet | 4 |
| Audio per packet | 240 ms |

## Receiver Pseudocode

```python
import socket
import opuslib

SAMPLE_RATE = 48000
CHANNELS = 1
FRAME_SIZE = 2880  # 60ms at 48kHz

decoder = opuslib.Decoder(SAMPLE_RATE, CHANNELS)
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("0.0.0.0", 4000))

last_seq = None

while True:
    data, addr = sock.recvfrom(1500)

    # Validate header
    if len(data) < 4 or data[0] != 0x4F or data[1] != 0x50:
        continue  # Invalid packet

    frame_count = data[2]
    seq = data[3]

    # Detect gaps — reset decoder on discontinuity
    if last_seq is not None:
        expected = (last_seq + 1) & 0xFF
        if seq != expected:
            # Packets were lost — reset decoder state
            decoder = opuslib.Decoder(SAMPLE_RATE, CHANNELS)
    last_seq = seq

    # Parse frames
    offset = 4
    for i in range(frame_count):
        if offset + 2 > len(data):
            break  # Truncated packet

        frame_len = (data[offset] << 8) | data[offset + 1]
        offset += 2

        if offset + frame_len > len(data):
            break  # Truncated frame

        opus_frame = data[offset:offset + frame_len]
        offset += frame_len

        # Decode Opus frame to PCM
        pcm = decoder.decode(opus_frame, FRAME_SIZE)
        # pcm is 2880 signed 16-bit samples (5760 bytes)
        process_audio(pcm)
```

## Packet Rate and Bandwidth

At default settings (48kHz, 64kbps, 60ms frames, 4 frames/packet):

| Metric | Value |
|--------|-------|
| Opus frames per second | ~16.7 |
| UDP packets per second | ~4.2 |
| Avg encoded frame size | ~480 bytes |
| Avg UDP packet payload | ~1924 bytes |
| Total bandwidth (with headers) | ~8.2 kB/s |
| Compression ratio vs raw PCM | ~11.7x |

## Sequence Number Usage

The sequence number wraps from 255 back to 0. On the receiver:

- If `(received_seq - last_seq) & 0xFF == 1` — normal, no gap
- If the difference is > 1 — packets were lost
- If the difference is very large (e.g., > 128) — likely reordering, treat as late packet

When a gap is detected, the recommended strategy is to call `opus_decode` with a NULL packet (packet loss concealment) for each missing frame, or simply reset the decoder if the gap is large.

## Discovery (mDNS)

The streamer advertises itself via mDNS:

- Hostname: `esp32-birdnet.local`
- Service: `_birdnet._udp`
- TXT records:
  - `rate=48000`
  - `codec=opus`
  - `frame_ms=60`
  - `frames_per_pkt=4`
  - `version=1.0`
