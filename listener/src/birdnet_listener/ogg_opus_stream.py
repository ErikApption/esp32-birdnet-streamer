"""
Ogg/Opus stream writer using PyOgg's libogg ctypes bindings.

Wraps pre-encoded Opus frames into a valid Ogg/Opus byte stream suitable
for HTTP streaming. Uses the reference libogg implementation (via PyOgg)
for page framing, CRC, and lacing.

Timestamp handling (RFC 7845):
  The granule position in Ogg/Opus represents the number of PCM samples at
  48kHz that have been encoded, INCLUDING the pre-skip. The first audio page
  must have granule_pos >= pre_skip so that decoders can subtract pre-skip to
  determine the actual playback position.

  For live streaming to VLC, we start granule at pre_skip and increment by
  frame_duration_samples for each frame. This ensures VLC's PCR tracking sees
  a clean, monotonic timeline starting at time=0 (after pre-skip subtraction).
"""

import ctypes
import random
import struct

from pyogg import ogg


class OggOpusStream:
    """
    Wraps raw Opus frames into a valid Ogg/Opus container stream.

    This does NOT re-encode audio — it takes already-encoded Opus frames
    and packages them into Ogg pages with correct headers per RFC 7845.
    """

    def __init__(self, sample_rate: int = 48000, channels: int = 1,
                 pre_skip: int = 3840, frame_duration_samples: int = 2880):
        """
        Args:
            sample_rate: Input sample rate (informational field in OpusHead).
            channels: Number of audio channels (1 or 2).
            pre_skip: Pre-skip value in samples at 48kHz (default 3840 = 80ms).
            frame_duration_samples: Samples per Opus frame at 48kHz (2880 = 60ms).
        """
        self.sample_rate = sample_rate
        self.channels = channels
        self.pre_skip = pre_skip
        self.frame_duration_samples = frame_duration_samples

        # Create a random serial number for this stream
        sizeof_c_int = ctypes.sizeof(ctypes.c_int)
        min_int = -2 ** (sizeof_c_int * 8 - 1)
        max_int = 2 ** (sizeof_c_int * 8 - 1) - 1
        self._serial_no = ctypes.c_int(random.randint(min_int, max_int))

        # Initialize Ogg stream state
        self._stream_state = ogg.ogg_stream_state()
        ogg.ogg_stream_init(
            ctypes.pointer(self._stream_state),
            self._serial_no,
        )

        self._ogg_packet = ogg.ogg_packet()
        self._ogg_page = ogg.ogg_page()
        self._packet_no = 0
        # Per RFC 7845: granule position represents total samples INCLUDING
        # pre-skip. Start at pre_skip so the first audio frame's granule is
        # pre_skip + frame_duration, which decoders interpret as playback
        # position = frame_duration (i.e., time 0 + one frame of audio).
        self._granule_pos = self.pre_skip

    def get_headers(self) -> bytes:
        """
        Generate the two mandatory Ogg/Opus header pages (OpusHead + OpusTags).

        Returns the complete header bytes that must be sent before any audio data.
        """
        output = bytearray()

        # ─── OpusHead identification header (RFC 7845 §5.1) ──────────────
        opus_head = b"OpusHead" + struct.pack(
            "<BBHIhB",
            1,                   # version
            self.channels,       # output channel count
            self.pre_skip,       # pre-skip
            self.sample_rate,    # input sample rate (informational)
            0,                   # output gain (0 dB)
            0,                   # channel mapping family (0 = mono/stereo)
        )

        self._write_packet_data(opus_head, granule_pos=0, bos=True, eos=False)
        output.extend(self._flush_pages())

        # ─── OpusTags comment header (RFC 7845 §5.2) ─────────────────────
        vendor = b"birdnet-listener"
        opus_tags = b"OpusTags"
        opus_tags += struct.pack("<I", len(vendor)) + vendor
        opus_tags += struct.pack("<I", 0)  # 0 user comments

        self._write_packet_data(opus_tags, granule_pos=0, bos=False, eos=False)
        output.extend(self._flush_pages())

        return bytes(output)

    def write_opus_frame(self, opus_frame: bytes, flush: bool = True) -> bytes:
        """
        Wrap a single pre-encoded Opus frame into Ogg page(s).

        Args:
            opus_frame: Raw Opus-encoded frame bytes.
            flush: If True, force-flush after each frame for low-latency streaming.
                   If False, let libogg decide when to emit pages (higher latency).

        Returns:
            Ogg page bytes (may be empty if not flushing and libogg is buffering).
        """
        self._granule_pos += self.frame_duration_samples
        self._write_packet_data(
            opus_frame,
            granule_pos=self._granule_pos,
            bos=False,
            eos=False,
        )
        if flush:
            return self._flush_pages()
        return self._get_pages()

    def write_opus_frames(self, opus_frames: list[bytes]) -> bytes:
        """
        Wrap multiple Opus frames into Ogg pages, flushing once at the end.

        This is more efficient for bursty delivery (ESP32 sends frames in
        bursts) — groups frames into fewer Ogg pages, reducing per-frame
        overhead and giving VLC a single timestamp per burst.

        Args:
            opus_frames: List of raw Opus-encoded frame bytes.

        Returns:
            Ogg page bytes containing all the submitted frames.
        """
        for frame in opus_frames:
            self._granule_pos += self.frame_duration_samples
            self._write_packet_data(
                frame,
                granule_pos=self._granule_pos,
                bos=False,
                eos=False,
            )
        return self._flush_pages()

    def close(self) -> bytes:
        """Clean up the Ogg stream state and return any final pages."""
        output = self._flush_pages()
        ogg.ogg_stream_clear(ctypes.pointer(self._stream_state))
        return output

    def _write_packet_data(self, data: bytes, granule_pos: int,
                           bos: bool, eos: bool) -> None:
        """Submit a packet to the Ogg stream."""
        # Create a ctypes buffer from the data
        buf = (ctypes.c_ubyte * len(data))(*data)
        buf_ptr = ctypes.cast(buf, ctypes.POINTER(ctypes.c_ubyte))

        self._ogg_packet.packet = buf_ptr
        self._ogg_packet.bytes = len(data)
        self._ogg_packet.b_o_s = 1 if bos else 0
        self._ogg_packet.e_o_s = 1 if eos else 0
        self._ogg_packet.granulepos = granule_pos
        self._ogg_packet.packetno = self._packet_no
        self._packet_no += 1

        result = ogg.ogg_stream_packetin(
            ctypes.pointer(self._stream_state),
            ctypes.pointer(self._ogg_packet),
        )
        if result != 0:
            raise RuntimeError("ogg_stream_packetin failed")

    def _get_pages(self) -> bytes:
        """Extract any complete pages from the stream."""
        output = bytearray()
        while ogg.ogg_stream_pageout(
            ctypes.pointer(self._stream_state),
            ctypes.pointer(self._ogg_page),
        ) != 0:
            output.extend(self._read_page())
        return bytes(output)

    def _flush_pages(self) -> bytes:
        """Force-flush all buffered data into pages."""
        output = bytearray()
        while ogg.ogg_stream_flush(
            ctypes.pointer(self._stream_state),
            ctypes.pointer(self._ogg_page),
        ) != 0:
            output.extend(self._read_page())
        return bytes(output)

    def _read_page(self) -> bytes:
        """Read the current ogg_page into Python bytes."""
        # Read header
        HeaderBufferPtr = ctypes.POINTER(
            ctypes.c_ubyte * self._ogg_page.header_len
        )
        header = bytes(HeaderBufferPtr(self._ogg_page.header.contents)[0])

        # Read body
        BodyBufferPtr = ctypes.POINTER(
            ctypes.c_ubyte * self._ogg_page.body_len
        )
        body = bytes(BodyBufferPtr(self._ogg_page.body.contents)[0])

        return header + body
