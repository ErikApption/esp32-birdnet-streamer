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
from birdnet_listener.mqtt_ha import MQTTConfig, MQTTHAIntegration
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
    opus_buffer_ms: int | None = None,
    mqtt_integration: MQTTHAIntegration | None = None,
) -> FastAPI:
    app = FastAPI(title="BirdNet Listener")
    audio_buffer = StreamBuffer()

    # Each Opus frame is 60ms. Default: 800 frames = ~48s of buffered audio.
    # With --buffer-ms, use fewer frames to reduce latency (e.g., 3000ms = 50 frames).
    frame_duration_ms = 60
    if opus_buffer_ms is not None:
        opus_max_chunks = max(1, opus_buffer_ms // frame_duration_ms)
    else:
        opus_max_chunks = 800
    opus_buffer = StreamBuffer(max_chunks=opus_max_chunks)
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
                on_telemetry=mqtt_integration.on_telemetry_received if mqtt_integration else None,
                on_stream_state_changed=mqtt_integration.on_stream_state_changed if mqtt_integration else None,
            ),
            local_addr=("0.0.0.0", udp_port),
            family=socket.AF_INET,
        )
        logger.info(f"UDP listener started on port {udp_port}")

        # Start MQTT integration if configured
        if mqtt_integration:
            try:
                await mqtt_integration.start(lambda: udp_protocol)
            except Exception as e:
                logger.error(f"[MQTT] Failed to start: {e}")

    @app.on_event("shutdown")
    async def shutdown():
        if mqtt_integration:
            await mqtt_integration.stop()

    @app.get("/")
    async def index():
        return {
            "service": "BirdNet Listener",
            "udp_port": udp_port,
            "http_port": http_port,
            "sample_rate": sample_rate,
            "normalize": normalizer is not None,
            "noise_reduce": noise_reducer is not None,
            "opus_buffer_chunks": opus_max_chunks,
            "opus_buffer_ms": opus_max_chunks * frame_duration_ms,
            "packets_received": udp_protocol.packets_received if udp_protocol else 0,
            "invalid_packets": udp_protocol._invalid_packets if udp_protocol else 0,
            "decode_errors": udp_protocol._decode_errors if udp_protocol else 0,
            "dropped_packets": udp_protocol._dropped_packets if udp_protocol else 0,
            "buffer_chunks": len(audio_buffer._buffer),
            "buffer_seq": audio_buffer._seq,
            "active_subscribers": len(audio_buffer._subscribers),
            "active_opus_subscribers": len(opus_buffer._subscribers),
            "audio_diagnostics": {
                "stream_uptime_seconds": round(udp_protocol.stream_uptime_seconds, 1) if udp_protocol else 0,
                "stream_uptime": udp_protocol._format_uptime(udp_protocol.stream_uptime_seconds) if udp_protocol else "—",
                "total_interruptions": udp_protocol._diag_total_interruptions if udp_protocol else 0,
                "consecutive_silent_intervals": udp_protocol._diag_total_silent_intervals if udp_protocol else 0,
                "reordered_packets_recovered": udp_protocol._diag_reordered_packets if udp_protocol else 0,
                "jitter_buffer_depth": len(udp_protocol._jitter_buffer) if udp_protocol else 0,
                "current_interval": {
                    "frames_decoded": udp_protocol._diag_frames_decoded if udp_protocol else 0,
                    "frames_silent": udp_protocol._diag_frames_silent if udp_protocol else 0,
                    "frames_quiet": udp_protocol._diag_frames_quiet if udp_protocol else 0,
                    "frames_clipping": udp_protocol._diag_frames_clipping if udp_protocol else 0,
                    "peak_max": udp_protocol._diag_peak_max if udp_protocol else 0,
                    "dtx_frames": udp_protocol._diag_dtx_frames if udp_protocol else 0,
                    "opus_errors": udp_protocol._diag_opus_errors if udp_protocol else 0,
                    "dropped_packets": udp_protocol._diag_dropped_this_interval if udp_protocol else 0,
                    "reordered_packets": udp_protocol._diag_reordered_this_interval if udp_protocol else 0,
                    "max_inter_packet_ms": round(udp_protocol._diag_max_inter_packet_ms, 1) if udp_protocol else 0,
                },
            },
            "power": {
                "battery_voltage": udp_protocol.battery_voltage if udp_protocol else 0.0,
                "battery_percent": udp_protocol.battery_percent if udp_protocol else 0,
                "solar_voltage": udp_protocol.solar_voltage if udp_protocol else 0.0,
                "is_charging": udp_protocol.is_charging if udp_protocol else False,
                "telemetry_packets": udp_protocol._telemetry_received if udp_protocol else 0,
            },
            "esp32_udp": {
                "rssi": udp_protocol.esp_rssi if udp_protocol else 0,
                "packets_sent": udp_protocol.esp_udp_sent if udp_protocol else 0,
                "send_errors": udp_protocol.esp_udp_errors if udp_protocol else 0,
                "packets_dropped": udp_protocol.esp_udp_dropped if udp_protocol else 0,
                "wifi_connected": udp_protocol.esp_wifi_connected if udp_protocol else False,
                "consecutive_fails": udp_protocol.esp_consecutive_fails if udp_protocol else 0,
            },
            "endpoints": {
                "playlist": "/stream.m3u",
                "stream": "/stream",
                "opus_stream": "/stream.opus",
            },
        }

    @app.get("/stream.m3u")
    async def playlist(request: Request):
        """Serve an M3U playlist pointing to the Opus audio stream."""
        host = request.headers.get("host", f"localhost:{http_port}")
        scheme = request.url.scheme
        stream_url = f"{scheme}://{host}/stream.opus"

        m3u_content = f"#EXTM3U\n#EXTINF:-1,BirdNet Live Stream\n{stream_url}\n"
        return PlainTextResponse(
            content=m3u_content,
            media_type="audio/x-mpegurl",
            headers={"Content-Disposition": "inline; filename=\"birdnet.m3u\""},
        )

    @app.get("/stream")
    async def stream(request: Request):
        """Stream live audio as WAV (PCM 16-bit).

        Note: The Opus stream (/stream.opus) is the preferred endpoint.
        This WAV stream decodes on the listener and has no pacing, so VLC
        may struggle with bursty delivery. Use --network-caching=5000 in VLC.
        """
        wav_header = make_wav_header(sample_rate, channels=channels)

        async def audio_generator() -> AsyncGenerator[bytes, None]:
            yield wav_header
            async for chunk in audio_buffer.stream_from():
                if await request.is_disconnected():
                    break
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
    async def stream_opus(request: Request):
        """Stream live audio as Ogg/Opus with paced delivery.

        The ESP32 sends audio in ~1-second bursts to maximize battery life
        (longer WiFi radio sleep between transmissions). This endpoint absorbs
        that burstiness with a playout buffer: frames are accumulated as they
        arrive from UDP and metered out to HTTP clients at real-time rate.

        Strategy:
        1. On connect, dump any pre-buffered frames immediately so VLC can
           fill its internal buffer and start playback quickly.
        2. After the initial burst, deliver frames at audio real-time rate
           (~60ms per frame) regardless of when they actually arrived from UDP.
        3. If the playout buffer runs dry (network stall), wait for more data
           and resume delivery without a clock reset.
        """
        # Frame duration at 48kHz with 60ms Opus frames
        frame_duration_s = 60.0 / 1000.0

        async def opus_generator() -> AsyncGenerator[bytes, None]:
            ogg_stream = OggOpusStream(sample_rate=sample_rate, channels=channels)
            yield ogg_stream.get_headers()

            # Pre-buffer: send recent Opus frames immediately so VLC can fill
            # its playback buffer without waiting. These have proper granule
            # positions so VLC interprets them correctly.
            buffered_frames = list(opus_buffer._buffer)
            if buffered_frames:
                page_data = ogg_stream.write_opus_frames(buffered_frames)
                if page_data:
                    yield page_data

            # Paced delivery: meter frames out at real-time rate.
            # We use a local queue that accumulates from the opus_buffer
            # and drains at frame_duration_s intervals.
            frame_queue: asyncio.Queue[bytes | None] = asyncio.Queue()

            async def feeder():
                """Pull frames from opus_buffer into the local queue."""
                try:
                    async for opus_frame in opus_buffer.stream_from():
                        await frame_queue.put(opus_frame)
                except Exception:
                    pass
                finally:
                    await frame_queue.put(None)  # sentinel

            feeder_task = asyncio.create_task(feeder())

            try:
                while True:
                    if await request.is_disconnected():
                        logger.debug("[Opus] Client disconnected, closing stream")
                        break

                    # Wait for a frame (blocks until one is available)
                    frame = await frame_queue.get()
                    if frame is None:
                        break

                    page_data = ogg_stream.write_opus_frame(frame, flush=False)
                    if page_data:
                        yield page_data

                    # Batch up to flush_interval frames if more are ready,
                    # then flush as a single Ogg page and pace.
                    frames_in_page = 1
                    flush_interval = 4  # 4 frames = 240ms per Ogg page
                    done = False

                    while frames_in_page < flush_interval and not frame_queue.empty():
                        frame = frame_queue.get_nowait()
                        if frame is None:
                            done = True
                            break
                        page_data = ogg_stream.write_opus_frame(frame, flush=False)
                        if page_data:
                            yield page_data
                        frames_in_page += 1

                    # Flush accumulated frames as one Ogg page
                    page_data = ogg_stream.flush()
                    if page_data:
                        yield page_data

                    if done:
                        break

                    # Pace: sleep for the real-time duration of the frames we just sent.
                    # Use 95% of real-time so we drain slightly faster than production
                    # rate, preventing unbounded queue growth while keeping delivery smooth.
                    await asyncio.sleep(frame_duration_s * frames_in_page * 0.95)

            finally:
                feeder_task.cancel()
                try:
                    await feeder_task
                except asyncio.CancelledError:
                    pass
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
    parser.add_argument(
        "--buffer-ms", type=int, default=None,
        help="Opus stream playout buffer size in milliseconds. Controls the amount of audio "
             "pre-buffered before streaming begins. Lower values reduce latency but may cause "
             "stuttering. Default is ~48000ms (800 frames × 60ms). Try 3000-5000 for low-latency testing. "
             "Can also be set via BUFFER_MS environment variable.",
    )
    parser.add_argument(
        "--mqtt-server", type=str, default=None,
        help="MQTT broker host (e.g. 'homeassistant.local' or '192.168.1.10'). "
             "MQTT integration is only enabled when this is set. "
             "Can also be set via MQTT_SERVER environment variable.",
    )
    parser.add_argument(
        "--mqtt-port", type=int, default=1883,
        help="MQTT broker port (default: 1883). Can also be set via MQTT_PORT env var.",
    )
    parser.add_argument(
        "--mqtt-username", type=str, default=None,
        help="MQTT broker username. Can also be set via MQTT_USERNAME env var.",
    )
    parser.add_argument(
        "--mqtt-password", type=str, default=None,
        help="MQTT broker password. Can also be set via MQTT_PASSWORD env var.",
    )
    parser.add_argument(
        "--mqtt-entity-name", type=str, default="BirdNet Streamer",
        help="Name of the device entity in Home Assistant (default: 'BirdNet Streamer'). "
             "Can also be set via MQTT_ENTITY_NAME env var.",
    )
    parser.add_argument("--log-level", default="info", choices=["debug", "info", "warning", "error"])
    args = parser.parse_args()

    # Allow BUFFER_MS env var as fallback for --buffer-ms (convenient for Docker)
    if args.buffer_ms is None:
        env_buffer_ms = os.environ.get("BUFFER_MS")
        if env_buffer_ms:
            try:
                args.buffer_ms = int(env_buffer_ms)
            except ValueError:
                pass

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

    # ─── MQTT / Home Assistant integration ───────────────────────────────
    mqtt_server = args.mqtt_server or os.environ.get("MQTT_SERVER", "")
    mqtt_integration = None

    if mqtt_server:
        mqtt_port_env = os.environ.get("MQTT_PORT")
        mqtt_config = MQTTConfig(
            host=mqtt_server,
            port=int(mqtt_port_env) if mqtt_port_env else args.mqtt_port,
            username=args.mqtt_username or os.environ.get("MQTT_USERNAME", ""),
            password=args.mqtt_password or os.environ.get("MQTT_PASSWORD", ""),
            entity_name=args.mqtt_entity_name
            if args.mqtt_entity_name != "BirdNet Streamer"
            else os.environ.get("MQTT_ENTITY_NAME", "BirdNet Streamer"),
        )
        mqtt_integration = MQTTHAIntegration(mqtt_config)
        logger.info(
            f"[MQTT] Enabled — broker: {mqtt_config.host}:{mqtt_config.port}, "
            f"entity: '{mqtt_config.entity_name}'"
        )
    else:
        logger.info("[MQTT] Disabled (no --mqtt-server or MQTT_SERVER set)")

    # ─── Start the HTTP + UDP server ──────────────────────────────────────
    app = create_app(
        udp_port=args.udp_port,
        sample_rate=args.sample_rate,
        http_port=args.http_port,
        normalizer=normalizer,
        noise_reducer=noise_reducer,
        discovery_stop_event=discovery_stop_event,
        opus_buffer_ms=args.buffer_ms,
        mqtt_integration=mqtt_integration,
    )

    logger.info(f"Starting BirdNet Listener — UDP:{args.udp_port} → HTTP:{args.http_port}")
    if args.buffer_ms is not None:
        frame_duration_ms = 60
        effective_chunks = max(1, args.buffer_ms // frame_duration_ms)
        logger.info(f"Opus buffer: {args.buffer_ms}ms ({effective_chunks} frames) — low-latency mode")
    else:
        logger.info("Opus buffer: default (800 frames, ~48s)")
    logger.info(f"Playlist URL: http://0.0.0.0:{args.http_port}/stream.m3u")
    logger.info(f"WAV stream:   http://0.0.0.0:{args.http_port}/stream")
    logger.info(f"Opus stream:  http://0.0.0.0:{args.http_port}/stream.opus")

    uvicorn.run(app, host="0.0.0.0", port=args.http_port, log_level=args.log_level)


if __name__ == "__main__":
    main()
