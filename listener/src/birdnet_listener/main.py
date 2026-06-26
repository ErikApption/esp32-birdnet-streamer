"""
BirdNet Listener - UDP audio receiver and HTTP stream server.

Receives raw PCM 16-bit mono audio from the ESP32 BirdNet Streamer via UDP
and re-serves it as an HTTP audio stream with an M3U playlist endpoint.

On startup, discovers the ESP32 via mDNS and pushes this machine's IP
to the ESP32 config API so it knows where to send audio.
"""

import argparse
import asyncio
import logging
import os
import socket
import struct
from collections import deque
from typing import AsyncGenerator

import httpx
import numpy as np
import uvicorn
from fastapi import FastAPI, Request
from fastapi.responses import PlainTextResponse, StreamingResponse
from zeroconf import Zeroconf, ServiceBrowser, ServiceStateChange
from zeroconf.asyncio import AsyncZeroconf, AsyncServiceBrowser

import opuslib_next as opuslib

from birdnet_listener.noise_reducer import NoiseReducer

logger = logging.getLogger("birdnet-listener")

# ─── mDNS service type advertised by the ESP32 ───────────────────────────────
MDNS_SERVICE_TYPE = "_birdnet._udp.local."
MDNS_HTTP_SERVICE_TYPE = "_http._tcp.local."
MDNS_HOSTNAME = "esp32-birdnet"


# ─── Discover the ESP32 via mDNS ─────────────────────────────────────────────

def discover_esp32(timeout: float = 10.0) -> str | None:
    """
    Use mDNS to find the ESP32 BirdNet device on the local network.
    Returns the IP address string or None if not found.
    """
    discovered_ip: str | None = None
    found_event = asyncio.Event() if False else None  # placeholder, using threading event below

    import threading
    found = threading.Event()
    result: list[str] = []

    class Listener:
        def add_service(self, zc: Zeroconf, type_: str, name: str) -> None:
            info = zc.get_service_info(type_, name)
            if info and info.addresses:
                ip = socket.inet_ntoa(info.addresses[0])
                logger.info(f"[mDNS] Found ESP32 BirdNet at {ip} (service: {name})")
                result.append(ip)
                found.set()

        def remove_service(self, zc: Zeroconf, type_: str, name: str) -> None:
            pass

        def update_service(self, zc: Zeroconf, type_: str, name: str) -> None:
            pass

    zc = Zeroconf()
    listener = Listener()

    logger.info(f"[mDNS] Searching for {MDNS_SERVICE_TYPE} ...")
    browser = ServiceBrowser(zc, MDNS_SERVICE_TYPE, listener)

    found.wait(timeout=timeout)
    zc.close()

    if result:
        return result[0]

    # Fallback: try resolving the hostname directly
    logger.info(f"[mDNS] Service not found, trying {MDNS_HOSTNAME}.local hostname resolution...")
    try:
        ip = socket.gethostbyname(f"{MDNS_HOSTNAME}.local")
        logger.info(f"[mDNS] Resolved {MDNS_HOSTNAME}.local -> {ip}")
        return ip
    except socket.gaierror:
        logger.warning("[mDNS] Could not resolve ESP32 hostname")
        return None


# ─── Push our IP to the ESP32 config API ──────────────────────────────────────

def get_local_ip() -> str:
    """
    Get the IP address to advertise to the ESP32.

    Priority:
      1. BIRDNET_HOST_IP env var (explicit host IP, useful in Docker bridge mode)
      2. BIRDNET_LISTENER_IP env var (legacy, same purpose)
      3. Auto-detect via UDP socket trick

    In Docker bridge networking, auto-detect returns the container IP (172.x.x.x)
    which is unreachable from the ESP32. Use BIRDNET_HOST_IP to pass the real host IP.
    """
    # Check explicit env vars first
    host_ip = os.environ.get("BIRDNET_HOST_IP")
    if host_ip:
        logger.info(f"[IP] Using BIRDNET_HOST_IP={host_ip}")
        return host_ip

    listener_ip = os.environ.get("BIRDNET_LISTENER_IP")
    if listener_ip:
        logger.info(f"[IP] Using BIRDNET_LISTENER_IP={listener_ip}")
        return listener_ip

    # Auto-detect
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        # Doesn't actually send anything — just determines the local interface
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        # Warn if it looks like a Docker bridge IP
        if ip.startswith("172.") or ip.startswith("10."):
            logger.warning(
                f"[IP] Auto-detected IP is {ip} — this looks like a container/internal IP. "
                f"Set BIRDNET_HOST_IP to the host's real LAN IP if running in Docker bridge mode."
            )
        return ip
    except Exception:
        return "127.0.0.1"
    finally:
        s.close()


def push_config_to_esp32(esp32_ip: str, listener_ip: str, udp_port: int, timeout: float = 5.0) -> bool:
    """
    POST to the ESP32's /config endpoint to set udp_host to our IP.
    Returns True on success, False on failure.
    """
    url = f"http://{esp32_ip}/config"
    data = {
        "udp_host": listener_ip,
        "udp_port": str(udp_port),
    }

    logger.info(f"[Config] Pushing config to ESP32 at {url}: udp_host={listener_ip}, udp_port={udp_port}")

    try:
        resp = httpx.post(url, data=data, timeout=timeout)
        if resp.status_code == 200:
            logger.info(f"[Config] ESP32 config updated successfully: {resp.json()}")
            return True
        else:
            logger.error(f"[Config] ESP32 returned status {resp.status_code}: {resp.text}")
            return False
    except httpx.RequestError as e:
        logger.error(f"[Config] Failed to reach ESP32: {e}")
        return False


# ─── Audio ring buffer shared between UDP receiver and HTTP clients ───────────

class AudioBuffer:
    """Ring buffer that holds recent audio chunks for streaming to HTTP clients."""

    def __init__(self, max_chunks: int = 200):
        self._buffer: deque[bytes] = deque(maxlen=max_chunks)
        self._seq = 0
        self._subscribers: list[asyncio.Event] = []

    def push(self, data: bytes) -> None:
        self._buffer.append(data)
        self._seq += 1
        # Wake all waiting subscribers
        for event in self._subscribers:
            event.set()

    async def stream_from(self) -> AsyncGenerator[bytes, None]:
        """Yield audio chunks as they arrive. Each caller gets its own wakeup event."""
        event = asyncio.Event()
        self._subscribers.append(event)
        last_seq = self._seq
        try:
            while True:
                await event.wait()
                event.clear()

                new_chunks = self._seq - last_seq
                if new_chunks > 0:
                    available = list(self._buffer)
                    start = max(0, len(available) - new_chunks)
                    for chunk in available[start:]:
                        yield chunk
                    last_seq = self._seq
        finally:
            self._subscribers.remove(event)


# ─── UDP Receiver Protocol ────────────────────────────────────────────────────

class UDPReceiverProtocol(asyncio.DatagramProtocol):
    """Asyncio UDP protocol that decodes Opus and pushes PCM audio into the buffer."""

    LOG_INTERVAL = 5.0  # seconds between status logs
    PACKET_MAGIC = b"OP"
    HEADER_SIZE = 4  # 2 bytes magic + 1 byte frame_count + 1 byte sequence_number
    FRAME_SIZE = 2880  # 60ms at 48kHz mono (samples per frame)
    LARGE_GAP_THRESHOLD = 128  # gaps larger than this are treated as reordering

    def __init__(self, audio_buffer: AudioBuffer, opus_decoder: opuslib.Decoder,
                 sample_rate: int = 48000, channels: int = 1):
        self.audio_buffer = audio_buffer
        self.opus_decoder = opus_decoder
        self.sample_rate = sample_rate
        self.channels = channels
        self.packets_received = 0
        self._receiving = False
        self._packets_since_last_log = 0
        self._bytes_since_last_log = 0
        self._decode_errors = 0
        self._dropped_packets = 0
        self._invalid_packets = 0
        self._expected_seq: int | None = None
        self._log_task: asyncio.TimerHandle | None = None

    def connection_made(self, transport) -> None:
        self.transport = transport
        self._schedule_log()

    def _schedule_log(self) -> None:
        loop = asyncio.get_event_loop()
        self._log_task = loop.call_later(self.LOG_INTERVAL, self._log_status)

    def _log_status(self) -> None:
        if self._packets_since_last_log > 0:
            if not self._receiving:
                logger.info("[UDP] Receiving audio stream")
                self._receiving = True
            msg = (
                f"[UDP] {self._packets_since_last_log} packets received "
                f"({self._bytes_since_last_log / 1024:.1f} KB) in the last "
                f"{self.LOG_INTERVAL:.0f}s — total: {self.packets_received}"
            )
            if self._dropped_packets > 0:
                msg += f" — dropped: {self._dropped_packets}"
            logger.info(msg)
        else:
            if self._receiving:
                logger.info("[UDP] Stream stopped — no packets received")
                self._receiving = False
        self._packets_since_last_log = 0
        self._bytes_since_last_log = 0
        self._schedule_log()

    def datagram_received(self, data: bytes, addr: tuple) -> None:
        self.packets_received += 1
        self._packets_since_last_log += 1
        self._bytes_since_last_log += len(data)

        # ─── Validate header ─────────────────────────────────────────────
        if len(data) < self.HEADER_SIZE:
            self._invalid_packets += 1
            if self._invalid_packets <= 3:
                logger.warning(f"[UDP] Packet too short ({len(data)} bytes), need at least {self.HEADER_SIZE}")
            return

        if data[0:2] != self.PACKET_MAGIC:
            self._invalid_packets += 1
            if self._invalid_packets <= 3:
                logger.warning(f"[UDP] Invalid magic: {data[0:2].hex()} (expected 4f50), first 16 bytes: {data[:16].hex()}")
            return

        frame_count = data[2]
        seq = data[3]

        # ─── Detect dropped packets ──────────────────────────────────────
        if self._expected_seq is not None:
            if seq != self._expected_seq:
                # Calculate how many packets were missed (handles wrap-around)
                gap = (seq - self._expected_seq) & 0xFF

                if gap > self.LARGE_GAP_THRESHOLD:
                    # Very large gap likely means reordering — treat as late/duplicate packet
                    logger.debug(f"[UDP] Likely reordered packet: expected seq {self._expected_seq}, got {seq} (gap={gap})")
                    return
                else:
                    # Genuine packet loss — reset decoder state for clean recovery
                    self._dropped_packets += gap
                    logger.debug(f"[UDP] Packet drop detected: expected seq {self._expected_seq}, got {seq} (gap={gap})")
                    self.opus_decoder = opuslib.Decoder(self.sample_rate, self.channels)
        self._expected_seq = (seq + 1) & 0xFF

        # ─── Decode Opus frames ──────────────────────────────────────────
        payload = data[self.HEADER_SIZE:]
        offset = 0

        for i in range(frame_count):
            # Each frame is preceded by a 2-byte big-endian length
            if offset + 2 > len(payload):
                self._decode_errors += 1
                if self._decode_errors <= 5:
                    logger.warning(f"[UDP] Truncated packet: missing frame length at frame {i+1}/{frame_count}")
                break

            frame_len = int.from_bytes(payload[offset:offset + 2], "big")
            offset += 2

            if offset + frame_len > len(payload):
                self._decode_errors += 1
                if self._decode_errors <= 5:
                    logger.warning(f"[UDP] Truncated packet: frame {i+1}/{frame_count} needs {frame_len} bytes, only {len(payload) - offset} available")
                break

            opus_frame = payload[offset:offset + frame_len]
            offset += frame_len

            try:
                pcm = self.opus_decoder.decode(opus_frame, frame_size=self.FRAME_SIZE)
                self.audio_buffer.push(pcm)
                if self.packets_received <= 3:
                    logger.info(f"[UDP] Decoded frame: {len(opus_frame)} bytes Opus -> {len(pcm)} bytes PCM")
            except opuslib.OpusError as e:
                self._decode_errors += 1
                if self._decode_errors <= 5:
                    logger.warning(f"[UDP] Opus decode error (frame {i+1}/{frame_count}): {e}")
                elif self._decode_errors == 6:
                    logger.warning("[UDP] Suppressing further Opus decode errors")

    def error_received(self, exc: Exception) -> None:
        logger.error(f"UDP error: {exc}")


# ─── WAV Header Generation ───────────────────────────────────────────────────

def make_wav_header(sample_rate: int, bits_per_sample: int = 16, channels: int = 1) -> bytes:
    """
    Create a WAV header for a continuous stream.
    Uses max values for sizes to indicate unknown/streaming length.
    """
    byte_rate = sample_rate * channels * (bits_per_sample // 8)
    block_align = channels * (bits_per_sample // 8)
    data_size = 0xFFFFFFFF - 36
    file_size = 0xFFFFFFFF

    header = struct.pack(
        "<4sI4s4sIHHIIHH4sI",
        b"RIFF",
        file_size,
        b"WAVE",
        b"fmt ",
        16,
        1,                # PCM format
        channels,
        sample_rate,
        byte_rate,
        block_align,
        bits_per_sample,
        b"data",
        data_size,
    )
    return header


# ─── Audio Normalizer ─────────────────────────────────────────────────────────

class AudioNormalizer:
    """
    Peak-normalizes PCM 16-bit audio chunks to a target dBFS level.

    Uses a smoothed gain factor to avoid abrupt volume changes between chunks.
    """

    def __init__(self, target_db: float = -3.0, attack: float = 0.1, release: float = 0.5):
        """
        Args:
            target_db: Target peak level in dBFS (e.g., -3.0 means normalize peaks to -3 dB).
            attack: How quickly gain increases when signal gets louder (0-1, lower = smoother).
            release: How quickly gain decreases when signal gets quieter (0-1, lower = smoother).
        """
        self.target_linear = 10 ** (target_db / 20.0) * 32767
        self.attack = attack
        self.release = release
        self.current_gain = 1.0

    def normalize(self, data: bytes) -> bytes:
        """Normalize a PCM 16-bit mono audio chunk, returning the normalized bytes."""
        samples = np.frombuffer(data, dtype=np.int16).astype(np.float32)

        if len(samples) == 0:
            return data

        peak = np.max(np.abs(samples))
        if peak < 1.0:
            # Silence or near-silence — don't amplify noise
            return data

        desired_gain = self.target_linear / peak

        # Smooth the gain change to avoid clicking/pumping
        if desired_gain < self.current_gain:
            # Signal got louder — respond quickly (attack)
            self.current_gain += self.attack * (desired_gain - self.current_gain)
        else:
            # Signal got quieter — respond slowly (release)
            self.current_gain += self.release * (desired_gain - self.current_gain)

        # Apply gain and clip to int16 range
        normalized = samples * self.current_gain
        normalized = np.clip(normalized, -32768, 32767)

        return normalized.astype(np.int16).tobytes()


# ─── Application Factory ─────────────────────────────────────────────────────

def create_app(
    udp_port: int,
    sample_rate: int,
    http_port: int,
    channels: int = 1,
    normalizer: AudioNormalizer | None = None,
    noise_reducer: NoiseReducer | None = None,
) -> FastAPI:
    app = FastAPI(title="BirdNet Listener")
    audio_buffer = AudioBuffer()
    udp_protocol: UDPReceiverProtocol | None = None

    @app.on_event("startup")
    async def startup():
        nonlocal udp_protocol
        loop = asyncio.get_running_loop()

        # Set up Opus decoder
        opus_decoder = opuslib.Decoder(sample_rate, channels)
        logger.info(f"Opus decoder initialized ({sample_rate} Hz, {channels}ch)")

        transport, udp_protocol = await loop.create_datagram_endpoint(
            lambda: UDPReceiverProtocol(audio_buffer, opus_decoder, sample_rate, channels),
            local_addr=("0.0.0.0", udp_port),
            family=socket.AF_INET,
        )
        logger.info(f"UDP listener started on port {udp_port}")

    @app.get("/")
    async def index():
        return {
            "service": "BirdNet Listener",
            "udp_port": udp_port,
            "http_port": http_port,
            "sample_rate": sample_rate,
            "normalize": normalizer is not None,
            "noise_reduce": noise_reducer is not None,
            "packets_received": udp_protocol.packets_received if udp_protocol else 0,
            "invalid_packets": udp_protocol._invalid_packets if udp_protocol else 0,
            "decode_errors": udp_protocol._decode_errors if udp_protocol else 0,
            "dropped_packets": udp_protocol._dropped_packets if udp_protocol else 0,
            "buffer_chunks": len(audio_buffer._buffer),
            "buffer_seq": audio_buffer._seq,
            "active_subscribers": len(audio_buffer._subscribers),
            "endpoints": {
                "playlist": "/stream.m3u",
                "stream": "/stream",
            },
        }

    @app.get("/stream.m3u")
    async def playlist(request: Request):
        """Serve an M3U playlist pointing to the audio stream."""
        host = request.headers.get("host", f"localhost:{http_port}")
        scheme = request.url.scheme
        stream_url = f"{scheme}://{host}/stream"

        m3u_content = f"#EXTM3U\n#EXTINF:-1,BirdNet Live Stream\n{stream_url}\n"
        return PlainTextResponse(
            content=m3u_content,
            media_type="audio/x-mpegurl",
            headers={"Content-Disposition": "inline; filename=\"birdnet.m3u\""},
        )

    @app.get("/stream")
    async def stream():
        """Stream live audio as WAV (PCM 16-bit)."""
        wav_header = make_wav_header(sample_rate)

        async def audio_generator() -> AsyncGenerator[bytes, None]:
            yield wav_header
            async for chunk in audio_buffer.stream_from():
                processed = chunk
                # Apply noise reduction first (operates on raw signal)
                if noise_reducer is not None:
                    processed = noise_reducer.reduce(processed)
                    if not processed:
                        continue
                # Then normalize the cleaned signal
                if normalizer is not None:
                    processed = normalizer.normalize(processed)
                yield processed

        return StreamingResponse(
            audio_generator(),
            media_type="audio/wav",
            headers={
                "Cache-Control": "no-cache, no-store",
                "Connection": "keep-alive",
                "X-Content-Type-Options": "nosniff",
            },
        )

    return app


# ─── CLI Entry Point ──────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="BirdNet Listener - UDP to HTTP audio bridge")
    parser.add_argument("--udp-port", type=int, default=4000, help="UDP port to listen on (default: 4000)")
    parser.add_argument("--http-port", type=int, default=8086, help="HTTP port to serve on (default: 8086)")
    parser.add_argument("--sample-rate", type=int, default=48000, help="Audio sample rate in Hz (default: 48000)")
    parser.add_argument("--esp32-ip", type=str, default=None, help="ESP32 IP (skips mDNS discovery)")
    parser.add_argument("--listener-ip", type=str, default=None, help="Override local IP sent to ESP32")
    parser.add_argument("--skip-discovery", action="store_true", help="Skip mDNS discovery and config push")
    parser.add_argument("--normalize", action="store_true", help="Enable audio normalization (boosts quiet audio)")
    parser.add_argument(
        "--normalize-target-db", type=float, default=-3.0,
        help="Target peak level in dBFS when normalizing (default: -3.0)",
    )
    parser.add_argument("--noise-reduce", action="store_true", help="Enable spectral gating noise reduction")
    parser.add_argument(
        "--noise-reduce-threshold-db", type=float, default=-12.0,
        help="Gate threshold in dB above noise floor (default: -12.0, lower = more aggressive)",
    )
    parser.add_argument("--log-level", default="info", choices=["debug", "info", "warning", "error"])
    args = parser.parse_args()

    logging.basicConfig(
        level=getattr(logging, args.log_level.upper()),
        format="%(asctime)s [%(name)s] %(levelname)s: %(message)s",
    )

    # ─── Discover ESP32 and push config (retries until successful) ───────
    # Priority: env var > CLI arg > mDNS discovery
    esp32_ip = os.environ.get("BIRDNET_ESP32_IP") or args.esp32_ip

    if not args.skip_discovery:
        import time

        # get_local_ip() checks BIRDNET_HOST_IP and BIRDNET_LISTENER_IP internally
        listener_ip = args.listener_ip or get_local_ip()

        while True:
            # Discover ESP32 if we don't have an IP yet
            if not esp32_ip:
                logger.info("No ESP32 IP provided, attempting mDNS discovery...")
                esp32_ip = discover_esp32(timeout=10.0)

            if not esp32_ip:
                logger.warning("[Discovery] ESP32 not found, retrying in 5s...")
                time.sleep(5)
                continue

            # Try to push config
            if push_config_to_esp32(esp32_ip, listener_ip, args.udp_port):
                break

            # Config push failed — ESP32 might have moved or rebooted
            logger.warning("[Discovery] Config push failed, re-discovering in 5s...")
            esp32_ip = None
            time.sleep(5)
    else:
        logger.info("Skipping mDNS discovery (--skip-discovery)")

    # ─── Start the HTTP + UDP server ──────────────────────────────────────
    normalizer = None
    if args.normalize:
        normalizer = AudioNormalizer(target_db=args.normalize_target_db)
        logger.info(f"Audio normalization enabled (target: {args.normalize_target_db} dBFS)")

    noise_reducer = None
    if args.noise_reduce:
        noise_reducer = NoiseReducer(
            sample_rate=args.sample_rate,
            gate_threshold_db=args.noise_reduce_threshold_db,
        )
        logger.info(
            f"Noise reduction enabled (threshold: {args.noise_reduce_threshold_db} dB, "
            f"estimating noise profile from first ~2s of audio)"
        )

    app = create_app(
        udp_port=args.udp_port,
        sample_rate=args.sample_rate,
        http_port=args.http_port,
        normalizer=normalizer,
        noise_reducer=noise_reducer,
    )

    logger.info(f"Starting BirdNet Listener — UDP:{args.udp_port} → HTTP:{args.http_port}")
    logger.info(f"Playlist URL: http://0.0.0.0:{args.http_port}/stream.m3u")

    uvicorn.run(app, host="0.0.0.0", port=args.http_port, log_level=args.log_level)


if __name__ == "__main__":
    main()
