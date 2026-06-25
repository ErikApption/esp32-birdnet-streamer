"""
Spectral gating noise reducer for real-time streaming PCM 16-bit audio.

Processes audio in overlapping STFT frames, estimates a noise profile from
the initial seconds of audio, and applies a spectral gate combined with
partial spectral subtraction to suppress background noise.
"""

import logging

import numpy as np
from scipy.signal import windows as scipy_windows

logger = logging.getLogger("birdnet-listener")


class NoiseReducer:
    """
    Real-time spectral gating noise reducer for streaming PCM 16-bit audio.

    Accumulates incoming audio into an internal buffer and processes it in
    overlapping frames using STFT-based spectral gating. The noise profile
    is estimated from the first few seconds of audio (assumed to be mostly
    ambient noise) and then used to gate subsequent frames.
    """

    def __init__(
        self,
        sample_rate: int = 48000,
        frame_size: int = 2048,
        hop_size: int = 512,
        noise_estimation_duration: float = 2.0,
        gate_threshold_db: float = -12.0,
        smoothing_factor: float = 0.85,
        noise_floor_margin: float = 2.5,
    ):
        """
        Args:
            sample_rate: Audio sample rate in Hz.
            frame_size: FFT frame size in samples.
            hop_size: Hop size between frames (controls overlap).
            noise_estimation_duration: Seconds of initial audio used to estimate noise floor.
            gate_threshold_db: How many dB above the noise floor a frequency bin must be
                               to be kept (higher = more aggressive noise removal).
            smoothing_factor: Temporal smoothing for the spectral gate (0-1, higher = smoother).
            noise_floor_margin: Multiplier applied to the noise profile before gating.
                                Higher values = more aggressive removal (default: 2.5).
        """
        self.sample_rate = sample_rate
        self.frame_size = frame_size
        self.hop_size = hop_size
        self.noise_estimation_samples = int(noise_estimation_duration * sample_rate)
        self.gate_threshold = 10 ** (gate_threshold_db / 20.0)
        self.smoothing_factor = smoothing_factor
        self.noise_floor_margin = noise_floor_margin

        # Analysis window
        self.window = scipy_windows.hann(frame_size, sym=False).astype(np.float32)

        # Internal state
        self._input_buffer = np.array([], dtype=np.float32)
        self._output_buffer = np.array([], dtype=np.float32)
        self._noise_profile: np.ndarray | None = None
        self._noise_samples_collected = 0
        self._noise_accumulator: np.ndarray | None = None
        self._noise_frame_count = 0
        self._prev_gain = np.ones(frame_size // 2 + 1, dtype=np.float32)
        self._overlap_buffer = np.zeros(frame_size, dtype=np.float32)

    def _estimate_noise_frame(self, magnitude: np.ndarray) -> None:
        """Accumulate a frame's magnitude spectrum for noise estimation."""
        if self._noise_accumulator is None:
            self._noise_accumulator = np.zeros_like(magnitude)
        self._noise_accumulator += magnitude
        self._noise_frame_count += 1

    def _finalize_noise_profile(self) -> None:
        """Compute the average noise profile from accumulated frames."""
        if self._noise_frame_count > 0 and self._noise_accumulator is not None:
            self._noise_profile = self._noise_accumulator / self._noise_frame_count
            logger.info(
                f"[NoiseReduce] Noise profile estimated from {self._noise_frame_count} frames "
                f"({self._noise_samples_collected / self.sample_rate:.1f}s of audio)"
            )

    def _process_frame(self, frame: np.ndarray) -> np.ndarray:
        """Process a single frame through spectral gating."""
        windowed = frame * self.window
        spectrum = np.fft.rfft(windowed)
        magnitude = np.abs(spectrum)
        phase = np.angle(spectrum)

        # Still estimating noise profile
        if self._noise_samples_collected < self.noise_estimation_samples:
            self._estimate_noise_frame(magnitude)
            self._noise_samples_collected += self.hop_size
            if self._noise_samples_collected >= self.noise_estimation_samples:
                self._finalize_noise_profile()
            # Pass through unmodified during estimation
            return frame

        # Apply spectral gate
        if self._noise_profile is not None:
            # Scale noise profile by margin for stronger removal
            noise_floor = self._noise_profile * self.noise_floor_margin

            # Compute SNR-based gain mask
            snr = magnitude / (noise_floor + 1e-10)
            gain = np.clip((snr - self.gate_threshold) / (1.0 - self.gate_threshold + 1e-10), 0.0, 1.0)

            # Apply a power curve to make the gate sharper (more on/off)
            gain = gain ** 2.0

            # Smooth gain over time to reduce musical noise artifacts
            gain = self.smoothing_factor * self._prev_gain + (1 - self.smoothing_factor) * gain
            self._prev_gain = gain

            # Spectral subtraction: also subtract a portion of the noise floor
            filtered_magnitude = np.maximum(magnitude - noise_floor * 0.5, magnitude * gain)
            filtered_magnitude = np.minimum(filtered_magnitude, magnitude)  # never amplify

            # Apply gain mask and reconstruct
            filtered_spectrum = filtered_magnitude * np.exp(1j * phase)
            result = np.fft.irfft(filtered_spectrum, n=self.frame_size)
            return result.astype(np.float32)

        return frame

    def reduce(self, data: bytes) -> bytes:
        """
        Process a chunk of PCM 16-bit audio through noise reduction.
        Returns the processed audio as PCM 16-bit bytes.

        Note: Due to buffering, output may be shorter or longer than input
        for any individual call, but will be consistent over time.
        """
        samples = np.frombuffer(data, dtype=np.int16).astype(np.float32)

        if len(samples) == 0:
            return data

        # Append to input buffer
        self._input_buffer = np.concatenate([self._input_buffer, samples])

        # Process complete frames with overlap-add
        output_samples = []
        while len(self._input_buffer) >= self.frame_size:
            frame = self._input_buffer[:self.frame_size]

            processed = self._process_frame(frame)

            # Overlap-add
            self._overlap_buffer += processed * self.window
            output_samples.append(self._overlap_buffer[:self.hop_size].copy())
            self._overlap_buffer = np.roll(self._overlap_buffer, -self.hop_size)
            self._overlap_buffer[-self.hop_size:] = 0.0

            # Advance by hop_size
            self._input_buffer = self._input_buffer[self.hop_size:]

        if not output_samples:
            return b""

        output = np.concatenate(output_samples)
        output = np.clip(output, -32768, 32767)
        return output.astype(np.int16).tobytes()
