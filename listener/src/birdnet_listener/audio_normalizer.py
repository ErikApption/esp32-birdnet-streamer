"""
Peak-normalizing audio processor for PCM 16-bit audio streams.
"""

import numpy as np


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
