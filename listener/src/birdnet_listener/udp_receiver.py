"""
UDP receiver protocol and stream buffer for receiving Opus-encoded audio.

Handles packet parsing, sequence tracking, and pushes raw Opus frames into
a buffer. Opus decoding to PCM only happens when there are active WAV stream
subscribers, saving CPU when nobody is listening.
"""

import asyncio
import logging
import threading
from collections import deque
from typing import AsyncGenerator

import opuslib_next as opuslib

logger = logging.getLogger("birdnet-listener")


class StreamBuffer:
    """Ring buffer that holds recent data chunks for streaming to HTTP clients."""

    def __init__(self, max_chunks: int = 200):
        self._buffer: deque[bytes] = deque(maxlen=max_chunks)
        self._seq = 0
        self._subscribers: list[asyncio.Event] = []

    @property
    def has_subscribers(self) -> bool:
        return len(self._subscribers) > 0

    def push(self, data: bytes) -> None:
        self._buffer.append(data)
        self._seq += 1
        for event in self._subscribers:
            event.set()

    async def stream_from(self) -> AsyncGenerator[bytes, None]:
        """Yield data chunks as they arrive. Each caller gets its own wakeup event."""
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


class UDPReceiverProtocol(asyncio.DatagramProtocol):
    """
    Asyncio UDP protocol that receives Opus audio packets.

    Raw Opus frames are always pushed to the opus_buffer (cheap passthrough).
    Opus-to-PCM decoding only occurs when the audio_buffer has active subscribers
    (i.e., someone is connected to the WAV stream), saving CPU otherwise.
    """

    LOG_INTERVAL = 5.0  # seconds between status logs
    PACKET_MAGIC = b"OP"
    HEADER_SIZE = 4  # 2 bytes magic + 1 byte frame_count + 1 byte sequence_number
    FRAME_SIZE = 2880  # 60ms at 48kHz mono (samples per frame)
    LARGE_GAP_THRESHOLD = 128  # gaps larger than this are treated as reordering

    def __init__(
        self,
        audio_buffer: StreamBuffer,
        sample_rate: int = 48000,
        channels: int = 1,
        opus_buffer: StreamBuffer | None = None,
        discovery_stop_event: threading.Event | None = None,
    ):
        self.audio_buffer = audio_buffer
        self.opus_buffer = opus_buffer
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
        self._discovery_stop_event = discovery_stop_event

        # Decoder is created lazily when first needed
        self._opus_decoder: opuslib.Decoder | None = None
        self._decoder_needs_reset = False

    def _get_decoder(self) -> opuslib.Decoder:
        """Get or create the Opus decoder, resetting if needed after packet loss."""
        if self._opus_decoder is None or self._decoder_needs_reset:
            self._opus_decoder = opuslib.Decoder(self.sample_rate, self.channels)
            self._decoder_needs_reset = False
        return self._opus_decoder

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
            if not self.audio_buffer.has_subscribers:
                msg += " (decoding skipped, no WAV clients)"
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

        # On first packet, cancel the discovery thread — ESP32 is already streaming to us
        if self._discovery_stop_event is not None and self.packets_received == 1:
            self._discovery_stop_event.set()
            self._discovery_stop_event = None

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
                gap = (seq - self._expected_seq) & 0xFF

                if gap > self.LARGE_GAP_THRESHOLD:
                    logger.debug(f"[UDP] Likely reordered packet: expected seq {self._expected_seq}, got {seq} (gap={gap})")
                    return
                else:
                    self._dropped_packets += gap
                    logger.debug(f"[UDP] Packet drop detected: expected seq {self._expected_seq}, got {seq} (gap={gap})")
                    # Mark decoder for reset — will be recreated next time we actually decode
                    self._decoder_needs_reset = True
        self._expected_seq = (seq + 1) & 0xFF

        # ─── Extract and dispatch Opus frames ────────────────────────────
        needs_pcm = self.audio_buffer.has_subscribers
        payload = data[self.HEADER_SIZE:]
        offset = 0

        for i in range(frame_count):
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

            # Always push raw Opus frames (cheap, just bytes)
            if self.opus_buffer is not None:
                self.opus_buffer.push(opus_frame)

            # Only decode to PCM when someone is listening on the WAV stream
            if needs_pcm:
                try:
                    decoder = self._get_decoder()
                    pcm = decoder.decode(opus_frame, frame_size=self.FRAME_SIZE)
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
