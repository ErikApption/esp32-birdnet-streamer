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
import uvicorn
from fastapi import FastAPI, Request
from fastapi.responses import PlainTextResponse, StreamingResponse
from zeroconf import Zeroconf, ServiceBrowser, ServiceStateChange
from zeroconf.asyncio import AsyncZeroconf, AsyncServiceBrowser

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
    """Get the local IP address that can reach the network."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        # Doesn't actually send anything — just determines the local interface
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except Exception:
        return "127.0.0.1"
    finally:
        s.close()


def push_config_to_esp32(esp32_ip: str, listener_ip: str, udp_port: int) -> bool:
    """
    POST to the ESP32's /config endpoint to set udp_host to our IP.
    Returns True on success.
    """
    url = f"http://{esp32_ip}/config"
    data = {
        "udp_host": listener_ip,
        "udp_port": str(udp_port),
    }

    logger.info(f"[Config] Pushing config to ESP32 at {url}: udp_host={listener_ip}, udp_port={udp_port}")

    try:
        resp = httpx.post(url, data=data, timeout=5.0)
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
    """Thread-safe ring buffer that holds recent audio chunks for streaming."""

    def __init__(self, max_chunks: int = 200):
        self._buffer: deque[bytes] = deque(maxlen=max_chunks)
        self._event = asyncio.Event()
        self._seq = 0

    def push(self, data: bytes) -> None:
        self._buffer.append(data)
        self._seq += 1
        self._event.set()
        self._event.clear()

    async def stream_from(self) -> AsyncGenerator[bytes, None]:
        """Yield audio chunks as they arrive."""
        last_seq = self._seq
        while True:
            await self._event.wait()
            new_chunks = self._seq - last_seq
            if new_chunks > 0:
                available = list(self._buffer)
                start = max(0, len(available) - new_chunks)
                for chunk in available[start:]:
                    yield chunk
                last_seq = self._seq


# ─── UDP Receiver Protocol ────────────────────────────────────────────────────

class UDPReceiverProtocol(asyncio.DatagramProtocol):
    """Asyncio UDP protocol that pushes received audio into the buffer."""

    def __init__(self, audio_buffer: AudioBuffer):
        self.audio_buffer = audio_buffer
        self.packets_received = 0

    def datagram_received(self, data: bytes, addr: tuple) -> None:
        self.packets_received += 1
        self.audio_buffer.push(data)

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


# ─── Application Factory ─────────────────────────────────────────────────────

def create_app(udp_port: int, sample_rate: int, http_port: int) -> FastAPI:
    app = FastAPI(title="BirdNet Listener")
    audio_buffer = AudioBuffer()
    udp_protocol: UDPReceiverProtocol | None = None

    @app.on_event("startup")
    async def startup():
        nonlocal udp_protocol
        loop = asyncio.get_running_loop()

        transport, udp_protocol = await loop.create_datagram_endpoint(
            lambda: UDPReceiverProtocol(audio_buffer),
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
            "packets_received": udp_protocol.packets_received if udp_protocol else 0,
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
                yield chunk

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
    parser.add_argument("--http-port", type=int, default=8080, help="HTTP port to serve on (default: 8080)")
    parser.add_argument("--sample-rate", type=int, default=48000, help="Audio sample rate in Hz (default: 48000)")
    parser.add_argument("--esp32-ip", type=str, default=None, help="ESP32 IP (skips mDNS discovery)")
    parser.add_argument("--listener-ip", type=str, default=None, help="Override local IP sent to ESP32")
    parser.add_argument("--skip-discovery", action="store_true", help="Skip mDNS discovery and config push")
    parser.add_argument("--log-level", default="info", choices=["debug", "info", "warning", "error"])
    args = parser.parse_args()

    logging.basicConfig(
        level=getattr(logging, args.log_level.upper()),
        format="%(asctime)s [%(name)s] %(levelname)s: %(message)s",
    )

    # ─── Resolve ESP32 IP ─────────────────────────────────────────────────
    # Priority: env var > CLI arg > mDNS discovery
    esp32_ip = os.environ.get("BIRDNET_ESP32_IP") or args.esp32_ip

    if not args.skip_discovery:
        if not esp32_ip:
            logger.info("No ESP32 IP provided, attempting mDNS discovery...")
            esp32_ip = discover_esp32(timeout=10.0)

        if esp32_ip:
            # Determine our local IP to push to the ESP32
            listener_ip = os.environ.get("BIRDNET_LISTENER_IP") or args.listener_ip or get_local_ip()
            push_config_to_esp32(esp32_ip, listener_ip, args.udp_port)
        else:
            logger.warning(
                "Could not find ESP32 on the network. "
                "Set BIRDNET_ESP32_IP env var or use --esp32-ip to provide it manually. "
                "Continuing anyway — will listen for UDP packets on port %d.",
                args.udp_port,
            )
    else:
        logger.info("Skipping mDNS discovery (--skip-discovery)")

    # ─── Start the HTTP + UDP server ──────────────────────────────────────
    app = create_app(
        udp_port=args.udp_port,
        sample_rate=args.sample_rate,
        http_port=args.http_port,
    )

    logger.info(f"Starting BirdNet Listener — UDP:{args.udp_port} → HTTP:{args.http_port}")
    logger.info(f"Playlist URL: http://0.0.0.0:{args.http_port}/stream.m3u")

    uvicorn.run(app, host="0.0.0.0", port=args.http_port, log_level=args.log_level)


if __name__ == "__main__":
    main()
