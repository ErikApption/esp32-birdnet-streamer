"""
BirdNet Listener - UDP audio receiver and HTTP stream server.

Receives raw PCM 16-bit mono audio from the ESP32 BirdNet Streamer via UDP
and re-serves it as an HTTP audio stream with an M3U playlist endpoint.

On startup, discovers the ESP32 via mDNS (in a background thread) and pushes
this machine's IP to the ESP32 config API so it knows where to send audio.
If UDP packets are already arriving, discovery is cancelled automatically.
"""

import argparse
import asyncio
import io
import logging
import os
import socket
import threading
import wave
from typing import AsyncGenerator

import uvicorn
from fastapi import FastAPI, Request
from fastapi.responses import PlainTextResponse, StreamingResponse

from birdnet_listener.audio_normalizer import AudioNormalizer
from birdnet_listener.discovery import get_local_ip, run_discovery_in_background
from birdnet_listener.noise_reducer import NoiseReducer
from birdnet_listener.ogg_opus_stream import OggOpusStream
from birdnet_listener.udp_receiver import StreamBuffer, UDPReceiverProtocol

logger = logging.getLogger("birdnet-listener")


# ─── WAV Header Generation ───────────────────────────────────────────────────

def make_wav_header(sample_rate: int, bits_per_sample: int = 16, channels: int = 1) -> bytes:
    """
    Create a WAV header for a continuous (streaming) PCM audio stream.

    Uses maximum file/data sizes to signal unknown length, which is standard
    for live WAV streams that don't have a predetermined duration.
    """
    buf = io.BytesIO()
    with wave.open(buf, "wb") as wf:
        wf.setnchannels(channels)
        wf.setsampwidth(bits_per_sample // 8)
        wf.setframerate(sample_rate)
        # Write zero frames — we only want the header
        wf.writeframes(b"")

    # The stdlib wave module writes correct sizes for 0 frames.
    # For streaming, patch the RIFF chunk size and data chunk size to 0xFFFFFFFF
    # so players treat it as an open-ended stream.
    header = bytearray(buf.getvalue())
    # RIFF chunk size at offset 4 (4 bytes, little-endian)
    header[4:8] = (0xFFFFFFFF).to_bytes(4, "little")
    # data chunk size at offset 40 (4 bytes, little-endian)
    header[40:44] = (0xFFFFFFFF - 36).to_bytes(4, "little")
    return bytes(header)


# ─── Application Factory ─────────────────────────────────────────────────────

def create_app(
    udp_port: int,
    sample_rate: int,
    http_port: int,
    channels: int = 1,
    normalizer: AudioNormalizer | None = None,
    noise_reducer: NoiseReducer | None = None,
    discovery_stop_event: threading.Event | None = None,
) -> FastAPI:
    app = FastAPI(title="BirdNet Listener")
    audio_buffer = StreamBuffer()
    opus_buffer = StreamBuffer(max_chunks=500)
    udp_protocol: UDPReceiverProtocol | None = None

    @app.on_event("startup")
    async def startup():
        nonlocal udp_protocol
        loop = asyncio.get_running_loop()

        transport, udp_protocol = await loop.create_datagram_endpoint(
            lambda: UDPReceiverProtocol(
                audio_buffer, sample_rate, channels,
                opus_buffer=opus_buffer,
                discovery_stop_event=discovery_stop_event,
            ),
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
                "opus_stream": "/stream.opus",
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
        wav_header = make_wav_header(sample_rate, channels=channels)

        async def audio_generator() -> AsyncGenerator[bytes, None]:
            yield wav_header
            async for chunk in audio_buffer.stream_from():
                processed = chunk
                if noise_reducer is not None:
                    processed = noise_reducer.reduce(processed)
                    if not processed:
                        continue
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

    @app.get("/stream.opus")
    async def stream_opus():
        """Stream live audio as Ogg/Opus (passthrough, no decoding/re-encoding)."""

        async def opus_generator() -> AsyncGenerator[bytes, None]:
            ogg_stream = OggOpusStream(sample_rate=sample_rate, channels=channels)
            yield ogg_stream.get_headers()

            try:
                async for opus_frame in opus_buffer.stream_from():
                    page_data = ogg_stream.write_opus_frame(opus_frame)
                    if page_data:
                        yield page_data
            finally:
                closing_data = ogg_stream.close()
                if closing_data:
                    yield closing_data

        return StreamingResponse(
            opus_generator(),
            media_type="audio/ogg",
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

    # ─── Discovery (background thread) ───────────────────────────────────
    esp32_ip = os.environ.get("BIRDNET_ESP32_IP") or args.esp32_ip
    discovery_stop_event: threading.Event | None = None

    if not args.skip_discovery:
        listener_ip = args.listener_ip or get_local_ip()

        discovery_stop_event = threading.Event()
        discovery_thread = threading.Thread(
            target=run_discovery_in_background,
            args=(esp32_ip, listener_ip, args.udp_port, discovery_stop_event),
            daemon=True,
            name="discovery",
        )
        discovery_thread.start()
    else:
        logger.info("Skipping mDNS discovery (--skip-discovery)")

    # ─── Audio processing ─────────────────────────────────────────────────
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

    # ─── Start the HTTP + UDP server ──────────────────────────────────────
    app = create_app(
        udp_port=args.udp_port,
        sample_rate=args.sample_rate,
        http_port=args.http_port,
        normalizer=normalizer,
        noise_reducer=noise_reducer,
        discovery_stop_event=discovery_stop_event,
    )

    logger.info(f"Starting BirdNet Listener — UDP:{args.udp_port} → HTTP:{args.http_port}")
    logger.info(f"Playlist URL: http://0.0.0.0:{args.http_port}/stream.m3u")
    logger.info(f"WAV stream:   http://0.0.0.0:{args.http_port}/stream")
    logger.info(f"Opus stream:  http://0.0.0.0:{args.http_port}/stream.opus")

    uvicorn.run(app, host="0.0.0.0", port=args.http_port, log_level=args.log_level)


if __name__ == "__main__":
    main()
