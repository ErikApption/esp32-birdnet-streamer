"""
UDP receiver protocol and stream buffer for receiving Opus-encoded audio.

Handles packet parsing, sequence tracking, jitter buffering for reordering,
and pushes raw Opus frames into a buffer. Opus decoding to PCM only happens
when there are active WAV stream subscribers, saving CPU when nobody is listening.
"""

import asyncio
import logging
import struct
import threading
import time
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

    async def stream_from(self, timeout: float = 30.0) -> AsyncGenerator[bytes, None]:
        """
        Yield data chunks as they arrive. Each caller gets its own wakeup event.

        Args:
            timeout: Max seconds to wait for new data before assuming the stream
                     is dead. Prevents zombie subscribers from accumulating.
        """
        event = asyncio.Event()
        self._subscribers.append(event)
        last_seq = self._seq
        try:
            while True:
                try:
                    await asyncio.wait_for(event.wait(), timeout=timeout)
                except asyncio.TimeoutError:
                    # No data arrived within the timeout — stop streaming
                    logger.debug("[StreamBuffer] Subscriber timed out waiting for data, closing")
                    return
                event.clear()

                new_chunks = self._seq - last_seq
                if new_chunks > 0:
                    available = list(self._buffer)
                    start = max(0, len(available) - new_chunks)
                    for chunk in available[start:]:
                        yield chunk
                    last_seq = self._seq
        finally:
            try:
                self._subscribers.remove(event)
            except ValueError:
                pass  # Already removed (shouldn't happen, but be safe)


class UDPReceiverProtocol(asyncio.DatagramProtocol):
    """
    Asyncio UDP protocol that receives Opus audio packets.

    Raw Opus frames are always pushed to the opus_buffer (cheap passthrough).
    Opus-to-PCM decoding only occurs when the audio_buffer has active subscribers
    (i.e., someone is connected to the WAV stream), saving CPU otherwise.

    Also handles telemetry packets (magic "TL") containing battery/solar voltage.
    """

    LOG_INTERVAL = 5.0  # seconds between status logs
    PACKET_MAGIC = b"OP"
    TELEMETRY_MAGIC = b"TL"
    HEADER_SIZE = 4  # 2 bytes magic + 1 byte frame_count + 1 byte sequence_number
    TELEMETRY_SIZE = 6  # 2 magic + 2 bat_adc_mV + 2 sol_adc_mV
    FRAME_SIZE = 2880  # 60ms at 48kHz mono (samples per frame)
    LARGE_GAP_THRESHOLD = 128  # gaps larger than this are treated as reordering

    # Voltage divider ratios (applied to raw ADC values to recover real voltage)
    VBAT_DIVIDER_RATIO = 2.0   # R1=R2=100kΩ → Vreal = Vadc × 2
    VSOL_DIVIDER_RATIO = 3.2   # R3=220kΩ, R4=100kΩ → Vreal = Vadc × 3.2

    # Battery SoC estimation — 3S NiMH AA (3 × 1.2V nominal)
    # NiMH discharge curve is fairly flat between 1.1–1.3V/cell.
    # Below 1.0V/cell the cell is considered empty (risk of damage).
    # After a full charge, resting OCV is ~1.4V/cell.
    VBAT_EMPTY = 3.0  # 3 × 1.0V/cell — deep discharge cutoff
    VBAT_FULL  = 4.2  # 3 × 1.4V/cell — resting OCV after full charge
    VBAT_ABS_MAX = 4.5  # Anything above this means charger is pushing voltage

    # Solar voltage threshold — above this, the charge module is likely active
    # and the battery sense reading includes charger output voltage, not true SoC.
    VSOL_CHARGING_THRESHOLD = 1.0  # Volts (panel producing meaningful power)

    # Audio level thresholds for silence/quiet detection (16-bit PCM)
    SILENCE_THRESHOLD = 50         # peak below this = digital silence (dead mic / DTX)
    QUIET_THRESHOLD = 500          # peak below this = suspiciously quiet
    CLIPPING_THRESHOLD = 32000     # peak above this = clipping

    # Jitter buffer configuration — holds packets briefly to allow reordering
    # The ESP32 sends bursts of ~4 packets every ~1000ms. Reordering within a
    # burst is common on WiFi. We hold packets for up to JITTER_BUFFER_MS before
    # dispatching them in sequence order.
    JITTER_BUFFER_SIZE = 16        # max packets to hold (covers 2+ bursts)
    JITTER_BUFFER_MS = 60.0        # max ms to wait for a missing packet before skipping
    JITTER_BUFFER_ENABLED = True   # can be disabled for zero-latency passthrough

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

        # Telemetry state (updated from "TL" packets)
        self.battery_voltage: float = 0.0
        self.solar_voltage: float = 0.0
        self.battery_percent: int = 0
        self.is_charging: bool = False
        self._telemetry_received: int = 0

        # ─── Audio diagnostics state (per log interval) ──────────────────
        self._diag_frames_decoded: int = 0       # Opus frames successfully decoded
        self._diag_frames_silent: int = 0        # frames with peak < SILENCE_THRESHOLD
        self._diag_frames_quiet: int = 0         # frames with peak < QUIET_THRESHOLD
        self._diag_frames_clipping: int = 0      # frames with peak > CLIPPING_THRESHOLD
        self._diag_peak_max: int = 0             # max peak sample this interval
        self._diag_opus_errors: int = 0          # decode errors this interval
        self._diag_dtx_frames: int = 0           # DTX (discontinuous tx) empty frames
        self._diag_dropped_this_interval: int = 0  # packets dropped this interval
        self._diag_gap_events: list[int] = []    # sequence gaps this interval
        self._diag_last_packet_time: float = 0.0  # timestamp of last received packet
        self._diag_max_inter_packet_ms: float = 0.0  # max gap between packets
        self._diag_total_silent_intervals: int = 0  # consecutive log intervals with all-silent audio
        self._diag_total_interruptions: int = 0   # total count of detected audio interruptions
        self._diag_reordered_packets: int = 0    # packets received out of order but recovered
        self._diag_reordered_this_interval: int = 0

        # ─── Jitter buffer state ─────────────────────────────────────────
        # Holds (seq, data, arrival_time) tuples; dispatched in order once the
        # next expected seq arrives or the wait timeout expires.
        self._jitter_buffer: dict[int, tuple[bytes, float]] = {}  # seq -> (data, arrival_time)
        self._jitter_next_seq: int | None = None  # next seq we want to dispatch
        self._jitter_flush_handle: asyncio.TimerHandle | None = None

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

    def _reset_diag_counters(self) -> None:
        """Reset per-interval diagnostic counters."""
        self._diag_frames_decoded = 0
        self._diag_frames_silent = 0
        self._diag_frames_quiet = 0
        self._diag_frames_clipping = 0
        self._diag_peak_max = 0
        self._diag_opus_errors = 0
        self._diag_dtx_frames = 0
        self._diag_dropped_this_interval = 0
        self._diag_gap_events.clear()
        self._diag_max_inter_packet_ms = 0.0
        self._diag_reordered_this_interval = 0

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

            # ─── Audio health diagnostics ─────────────────────────────────
            self._log_audio_diagnostics()
        else:
            if self._receiving:
                logger.info("[UDP] Stream stopped — no packets received")
                self._receiving = False
                self._diag_total_interruptions += 1

        self._packets_since_last_log = 0
        self._bytes_since_last_log = 0
        self._reset_diag_counters()
        self._schedule_log()

    def _log_audio_diagnostics(self) -> None:
        """Log detailed audio health metrics for the current interval."""
        decoded = self._diag_frames_decoded
        if decoded == 0 and not self.audio_buffer.has_subscribers:
            return  # No decoding happening, nothing to report

        # ─── Packet timing ────────────────────────────────────────────────
        if self._diag_max_inter_packet_ms > 1500:
            logger.warning(
                f"[Audio:Timing] Max inter-packet gap: {self._diag_max_inter_packet_ms:.0f}ms "
                f"(expected ~1000ms for burst mode) — possible network stall or sender hiccup"
            )

        # ─── Packet loss detail ───────────────────────────────────────────
        if self._diag_dropped_this_interval > 0:
            gap_summary = ", ".join(str(g) for g in self._diag_gap_events[:10])
            extra = f" (+{len(self._diag_gap_events) - 10} more)" if len(self._diag_gap_events) > 10 else ""
            logger.warning(
                f"[Audio:Loss] {self._diag_dropped_this_interval} packets lost this interval "
                f"— seq gaps: [{gap_summary}{extra}]"
            )
            self._diag_total_interruptions += 1

        # ─── Opus decode errors this interval ─────────────────────────────
        if self._diag_opus_errors > 0:
            logger.warning(
                f"[Audio:Opus] {self._diag_opus_errors} decode errors this interval "
                f"(total: {self._decode_errors}) — audio will have gaps"
            )
            self._diag_total_interruptions += 1

        # ─── DTX (silence from encoder) ──────────────────────────────────
        if self._diag_dtx_frames > 0:
            logger.info(
                f"[Audio:DTX] {self._diag_dtx_frames} DTX frames (encoder-detected silence, ≤2 bytes)"
            )

        # ─── Reordering recovery ─────────────────────────────────────────
        if self._diag_reordered_this_interval > 0:
            logger.info(
                f"[Audio:Jitter] {self._diag_reordered_this_interval} packets arrived out of order "
                f"and were recovered (total: {self._diag_reordered_packets})"
            )

        # ─── Audio level analysis ─────────────────────────────────────────
        if decoded > 0:
            silent_pct = (self._diag_frames_silent / decoded) * 100
            quiet_pct = (self._diag_frames_quiet / decoded) * 100

            level_msg = (
                f"[Audio:Level] decoded={decoded} frames | "
                f"peak_max={self._diag_peak_max} | "
                f"silent={self._diag_frames_silent} ({silent_pct:.0f}%) | "
                f"quiet={self._diag_frames_quiet} ({quiet_pct:.0f}%)"
            )
            if self._diag_frames_clipping > 0:
                level_msg += f" | CLIPPING={self._diag_frames_clipping}"

            # Log at appropriate level based on severity
            if silent_pct >= 90:
                logger.warning(level_msg + " ⚠ MOSTLY SILENT — check mic/I2S connection")
                self._diag_total_silent_intervals += 1
                if self._diag_total_silent_intervals >= 3:
                    logger.error(
                        f"[Audio:Level] {self._diag_total_silent_intervals} consecutive silent intervals "
                        "— microphone may be dead or disconnected"
                    )
            elif quiet_pct >= 80:
                logger.warning(level_msg + " ⚠ VERY QUIET — check mic gain / placement")
                self._diag_total_silent_intervals = 0
            else:
                logger.debug(level_msg)
                self._diag_total_silent_intervals = 0

        # ─── Summary health indicator (periodic) ─────────────────────────
        if self._diag_total_interruptions > 0 and self.packets_received % 100 == 0:
            logger.info(
                f"[Audio:Health] Total interruptions detected: {self._diag_total_interruptions} "
                f"(drops: {self._dropped_packets}, decode_errors: {self._decode_errors})"
            )

    def datagram_received(self, data: bytes, addr: tuple) -> None:
        self.packets_received += 1
        self._packets_since_last_log += 1
        self._bytes_since_last_log += len(data)

        # ─── Inter-packet timing ─────────────────────────────────────────
        now = time.monotonic()
        if self._diag_last_packet_time > 0:
            gap_ms = (now - self._diag_last_packet_time) * 1000.0
            if gap_ms > self._diag_max_inter_packet_ms:
                self._diag_max_inter_packet_ms = gap_ms
        self._diag_last_packet_time = now

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

        # ─── Handle telemetry packets ────────────────────────────────────
        if data[0:2] == self.TELEMETRY_MAGIC:
            if len(data) >= self.TELEMETRY_SIZE:
                # Raw ADC millivolts from ESP32
                bat_adc_mv = int.from_bytes(data[2:4], "big")
                sol_adc_mv = int.from_bytes(data[4:6], "big")

                # Apply divider ratios to recover actual voltages
                self.battery_voltage = (bat_adc_mv / 1000.0) * self.VBAT_DIVIDER_RATIO
                self.solar_voltage = (sol_adc_mv / 1000.0) * self.VSOL_DIVIDER_RATIO

                # Determine if charger is active — when solar is producing power,
                # the charge module pushes its own voltage onto the battery terminals,
                # making the reading unreliable for SoC estimation.
                charging = (
                    self.solar_voltage > self.VSOL_CHARGING_THRESHOLD
                    or self.battery_voltage > self.VBAT_ABS_MAX
                )
                self.is_charging = charging

                if charging:
                    # Can't estimate SoC while charger is active — report -1
                    self.battery_percent = -1
                else:
                    pct = (self.battery_voltage - self.VBAT_EMPTY) / (self.VBAT_FULL - self.VBAT_EMPTY) * 100.0
                    self.battery_percent = int(max(0, min(100, pct)))

                self._telemetry_received += 1
                if self._telemetry_received <= 3 or self._telemetry_received % 60 == 0:
                    if charging:
                        status = f"charging (bat sense: {self.battery_voltage:.2f}V — charger voltage, not true SoC)"
                    else:
                        status = f"{self.battery_voltage:.2f}V ({self.battery_percent}%)"
                    logger.info(
                        f"[Telemetry] Battery: {status}, "
                        f"Solar: {self.solar_voltage:.2f}V "
                        f"(raw ADC: bat={bat_adc_mv}mV, sol={sol_adc_mv}mV)"
                    )
            return

        if data[0:2] != self.PACKET_MAGIC:
            self._invalid_packets += 1
            if self._invalid_packets <= 3:
                logger.warning(f"[UDP] Invalid magic: {data[0:2].hex()} (expected 4f50), first 16 bytes: {data[:16].hex()}")
            return

        frame_count = data[2]
        seq = data[3]

        # ─── Jitter buffer: hold and reorder ─────────────────────────────
        if self.JITTER_BUFFER_ENABLED:
            self._jitter_insert(seq, data)
        else:
            # Direct dispatch (legacy path, no reordering)
            self._dispatch_packet(seq, data)

    # ─── Jitter Buffer Logic ─────────────────────────────────────────────────

    def _jitter_insert(self, seq: int, data: bytes) -> None:
        """Insert a packet into the jitter buffer and try to flush in order."""
        now = time.monotonic()

        # Initialize on first packet
        if self._jitter_next_seq is None:
            self._jitter_next_seq = seq

        # Calculate distance from expected next seq (wrapping 0-255)
        distance = (seq - self._jitter_next_seq) & 0xFF

        # If distance is huge (> LARGE_GAP_THRESHOLD), it's either a very late
        # duplicate or the sender restarted. For a restart, accept it as new base.
        if distance > self.LARGE_GAP_THRESHOLD:
            # Check if it's behind us (late arrival, already dispatched)
            behind = (self._jitter_next_seq - seq) & 0xFF
            if behind <= self.LARGE_GAP_THRESHOLD:
                # This is a late duplicate — already dispatched past this seq
                logger.debug(f"[Jitter] Discarding late duplicate seq {seq} (expected >= {self._jitter_next_seq})")
                return
            else:
                # Sender likely restarted — flush everything and reset
                logger.info(f"[Jitter] Seq jump detected ({self._jitter_next_seq} -> {seq}), resetting jitter buffer")
                self._jitter_flush_all()
                self._jitter_next_seq = seq

        # Don't buffer if we already have this seq (duplicate)
        if seq in self._jitter_buffer:
            return

        # Overflow protection — if buffer is full, force-flush oldest
        if len(self._jitter_buffer) >= self.JITTER_BUFFER_SIZE:
            self._jitter_flush_up_to_oldest()

        # Store the packet
        self._jitter_buffer[seq] = (data, now)

        # Track reordering: if this packet's seq is before what we'd have dispatched next
        # without buffering, it was out of order but we caught it
        if distance > 0 and seq != self._jitter_next_seq:
            # Check if there are packets after this one already buffered
            pass  # Tracked at dispatch time below

        # Try to flush consecutive packets starting from _jitter_next_seq
        self._jitter_try_flush()

        # Schedule a timeout flush in case the next expected packet never arrives
        self._jitter_schedule_timeout()

    def _jitter_try_flush(self) -> None:
        """Dispatch all consecutive packets from _jitter_next_seq."""
        while self._jitter_next_seq in self._jitter_buffer:
            seq = self._jitter_next_seq
            data, arrival_time = self._jitter_buffer.pop(seq)

            # Check if this packet arrived after later-sequenced ones (reordered but recovered)
            # We can detect this if there were packets with higher seq already in the buffer
            # at the time we're dispatching this one — but simpler: if we held it for > 0ms
            # and it's not the most recently arrived, it was reordered.
            self._dispatch_packet(seq, data)
            self._jitter_next_seq = (seq + 1) & 0xFF

        # Cancel timeout if buffer is empty
        if not self._jitter_buffer and self._jitter_flush_handle is not None:
            self._jitter_flush_handle.cancel()
            self._jitter_flush_handle = None

    def _jitter_schedule_timeout(self) -> None:
        """Schedule a flush for when the oldest buffered packet exceeds JITTER_BUFFER_MS."""
        if self._jitter_flush_handle is not None:
            self._jitter_flush_handle.cancel()
            self._jitter_flush_handle = None

        if not self._jitter_buffer:
            return

        # Find the oldest packet's arrival time
        oldest_time = min(t for _, t in self._jitter_buffer.values())
        elapsed_ms = (time.monotonic() - oldest_time) * 1000.0
        remaining_ms = max(0, self.JITTER_BUFFER_MS - elapsed_ms)

        loop = asyncio.get_event_loop()
        self._jitter_flush_handle = loop.call_later(
            remaining_ms / 1000.0, self._jitter_timeout_flush
        )

    def _jitter_timeout_flush(self) -> None:
        """Called when the jitter buffer timeout expires — skip missing packets and flush."""
        self._jitter_flush_handle = None

        if not self._jitter_buffer:
            return

        now = time.monotonic()

        # Find how far ahead the buffer has packets
        # Skip forward to the lowest seq we actually have
        buffered_seqs = sorted(self._jitter_buffer.keys(), key=lambda s: (s - self._jitter_next_seq) & 0xFF)

        if not buffered_seqs:
            return

        next_available = buffered_seqs[0]
        gap = (next_available - self._jitter_next_seq) & 0xFF

        if gap > 0 and gap <= self.LARGE_GAP_THRESHOLD:
            # The missing packets have timed out — mark them as dropped
            self._dropped_packets += gap
            self._diag_dropped_this_interval += gap
            self._diag_gap_events.append(gap)
            self._decoder_needs_reset = True
            logger.debug(
                f"[Jitter] Timeout: skipping {gap} missing packets "
                f"(seq {self._jitter_next_seq}..{(next_available - 1) & 0xFF})"
            )
            self._jitter_next_seq = next_available

        # Now flush what we can
        self._jitter_try_flush()

        # If there are still buffered packets, schedule another timeout
        if self._jitter_buffer:
            self._jitter_schedule_timeout()

    def _jitter_flush_all(self) -> None:
        """Force-flush all buffered packets in sequence order (used on reset)."""
        if not self._jitter_buffer:
            return

        buffered_seqs = sorted(
            self._jitter_buffer.keys(),
            key=lambda s: (s - self._jitter_next_seq) & 0xFF
        )

        for seq in buffered_seqs:
            data, _ = self._jitter_buffer.pop(seq)
            self._dispatch_packet(seq, data)

        if self._jitter_flush_handle is not None:
            self._jitter_flush_handle.cancel()
            self._jitter_flush_handle = None

    def _jitter_flush_up_to_oldest(self) -> None:
        """When buffer is full, flush from next_seq up to make room."""
        if not self._jitter_buffer:
            return

        # Sort by distance from next_seq
        buffered_seqs = sorted(
            self._jitter_buffer.keys(),
            key=lambda s: (s - self._jitter_next_seq) & 0xFF
        )

        # Advance next_seq to the first buffered packet, marking skipped as dropped
        first_buffered = buffered_seqs[0]
        gap = (first_buffered - self._jitter_next_seq) & 0xFF
        if gap > 0 and gap <= self.LARGE_GAP_THRESHOLD:
            self._dropped_packets += gap
            self._diag_dropped_this_interval += gap
            self._diag_gap_events.append(gap)
            self._decoder_needs_reset = True
            logger.debug(f"[Jitter] Buffer full: skipping {gap} missing packets")
            self._jitter_next_seq = first_buffered

        # Flush consecutive from new next_seq
        self._jitter_try_flush()

    # ─── Packet Processing (after reordering) ────────────────────────────────

    def _dispatch_packet(self, seq: int, data: bytes) -> None:
        """Process a validated, reordered audio packet — extract and decode Opus frames."""
        # Track reordering: if seq differs from the simple expected counter
        if self._expected_seq is not None and seq != self._expected_seq:
            # If we got here via jitter buffer, this means the packet was
            # recovered out of order (not dropped)
            reorder_distance = (seq - self._expected_seq) & 0xFF
            if reorder_distance <= self.LARGE_GAP_THRESHOLD and reorder_distance > 0:
                # This was a forward jump not handled by jitter — actual drop
                pass  # drops already counted by jitter timeout
            elif seq != self._expected_seq:
                self._diag_reordered_packets += 1
                self._diag_reordered_this_interval += 1

        self._expected_seq = (seq + 1) & 0xFF

        # ─── Extract and dispatch Opus frames ────────────────────────────
        frame_count = data[2]
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

            # Detect DTX frames (encoder sends ≤2 bytes for silence)
            if frame_len <= 2:
                self._diag_dtx_frames += 1

            # Always push raw Opus frames (cheap, just bytes)
            if self.opus_buffer is not None:
                self.opus_buffer.push(opus_frame)

            # Only decode to PCM when someone is listening on the WAV stream
            if needs_pcm:
                try:
                    decoder = self._get_decoder()
                    pcm = decoder.decode(opus_frame, frame_size=self.FRAME_SIZE)
                    self.audio_buffer.push(pcm)
                    self._diag_frames_decoded += 1

                    # ─── Analyze PCM levels for diagnostics ───────────────
                    self._analyze_pcm_frame(pcm)

                    if self.packets_received <= 3:
                        logger.info(f"[UDP] Decoded frame: {len(opus_frame)} bytes Opus -> {len(pcm)} bytes PCM")
                except opuslib.OpusError as e:
                    self._decode_errors += 1
                    self._diag_opus_errors += 1
                    if self._decode_errors <= 5:
                        logger.warning(f"[UDP] Opus decode error (frame {i+1}/{frame_count}): {e}")
                    elif self._decode_errors == 6:
                        logger.warning("[UDP] Suppressing further Opus decode errors")

    def _analyze_pcm_frame(self, pcm: bytes) -> None:
        """Analyze a decoded PCM frame for silence, quiet, or clipping."""
        # PCM is 16-bit signed little-endian mono
        num_samples = len(pcm) // 2
        if num_samples == 0:
            return

        # Find peak absolute sample value (fast path using struct)
        peak = 0
        # Sample a subset for large frames to keep overhead low
        step = max(1, num_samples // 480)  # ~480 checks per frame max
        for i in range(0, num_samples, step):
            sample = abs(struct.unpack_from("<h", pcm, i * 2)[0])
            if sample > peak:
                peak = sample

        if peak > self._diag_peak_max:
            self._diag_peak_max = peak

        if peak < self.SILENCE_THRESHOLD:
            self._diag_frames_silent += 1
        elif peak < self.QUIET_THRESHOLD:
            self._diag_frames_quiet += 1

        if peak > self.CLIPPING_THRESHOLD:
            self._diag_frames_clipping += 1

    def error_received(self, exc: Exception) -> None:
        logger.error(f"UDP error: {exc}")
