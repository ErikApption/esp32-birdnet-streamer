# Diagnostic LED — Signal Quality Tests

## Overview

The onboard RGB LED (GPIO 48) indicates audio pipeline health during diagnostic mode.
It uses a two-tier detection system: I2S frame analysis and Opus encoder output analysis.

| LED Color | Meaning |
|-----------|---------|
| **Red** | Mic broken — ≥50% of frames failing, or all Opus frames silent |
| **Yellow** | Degraded — 10–50% of frames failing |
| **Green** | Healthy — audio encoding normally |

---

## Detection Tiers

### Tier 1: I2S Frame Analysis (per DMA read — 1024 samples)

Runs on every `i2s_read()` return. Classifies each frame as "empty" (faulty) or "good."

#### Test 1: Raw Bus Activity

- **What**: Counts how many 32-bit raw samples exceed ±100,000 in magnitude.
- **Threshold**: If ≤10% of samples are above threshold → frame is empty.
- **Detects**: Mic unpowered, SD pin completely disconnected, mic VDD missing.
- **Does NOT detect**: Intermittent connections that still drive the bus most of the time, shorted pins that produce valid-magnitude noise.

#### Test 2: Stuck Line Detection

- **What**: Finds the longest run of consecutive identical 16-bit samples.
- **Threshold**: If longest run > 25% of frame length → frame is empty.
- **Detects**: SCK or SD wire not toggling (e.g., cold solder joint on clock line), data line shorted to a fixed voltage.
- **Does NOT detect**: Random noise from a floating pin (which produces varying samples).

#### Test 3: DC Rail Detection

- **What**: Computes mean (DC offset) of all 16-bit samples in the frame.
- **Threshold**: If |DC offset| > 5000 → frame is empty.
- **Detects**: SD pin shorted directly to VDD or GND (produces large constant bias).
- **Does NOT detect**: SD pin shorted through a resistor (produces smaller bias), intermittent shorts that average out over the frame.

#### Test 4: Totally Flat Signal

- **What**: Checks if peak-to-peak amplitude is exactly 0.
- **Threshold**: peak-to-peak == 0 → frame is empty.
- **Detects**: All samples identical (e.g., bus held at one value, unpowered mic with no noise).
- **Does NOT detect**: Any signal with even minimal variation.

#### Test 5: Uncorrelated Noise Detection (Autocorrelation)

- **What**: Computes lag-1 autocorrelation of the 16-bit samples. This measures whether adjacent samples are related to each other.
- **Threshold**: If `crossSum * 10 < autoSum * 3` (correlation < 0.3) → frame is empty.
- **Detects**: Random noise from floating/shorted data pin, intermittent connection producing garbage. This is the key test for "signal has energy but sounds blank."
- **Does NOT detect**: Structured interference (e.g., 50/60Hz hum from power coupling), which would have high autocorrelation despite not being useful audio.
- **Why it works**: Real audio at 48kHz is band-limited — each sample strongly predicts the next (correlation >0.5). Random electrical noise from a wiring fault has no sample-to-sample structure (correlation <0.1).

---

### Tier 2: Opus Encoder Output Analysis (per encoded frame — 60ms)

After encoding each 60ms frame, checks the compressed output size.

#### Test 6: Opus Frame Size (Silent Detection)

- **What**: Compares encoded frame byte count against `OPUS_SILENT_THRESHOLD` (80 bytes).
- **Threshold**: If encoded bytes ≤ 80 → frame counted as "silent."
- **Detects**: True silence (DTX outputs 2–3 bytes), comfort noise (10–40 bytes), very low energy input.
- **Does NOT detect**: Noise with enough spectral energy to encode above 80 bytes. A floating/shorted data pin can produce broadband noise that encodes to 100–300+ bytes — indistinguishable from real mic audio at the encoder level.

---

## The Gap: What None of These Tests Catch

**Scenario**: I2S SD pin intermittently connected or shorted to ground *through the mic's output stage*.

In this case:
- The mic IC is powered and its output driver is active
- The bus shows valid-magnitude samples (passes Test 1)
- Samples vary randomly from coupling noise (passes Tests 2, 3, 4)
- The noise has enough broadband energy that Opus encodes it to >80 bytes (passes Test 5)
- But the decoded audio sounds "blank" — no useful acoustic content, just hiss/noise floor

This is indistinguishable from "microphone in a quiet room picking up ambient noise" from the ESP32's perspective alone. The encoder cannot differentiate between:
- 40 dB SPL ambient room noise (real audio, legitimate)
- Noise from a floating data line (garbage, wiring fault)

---

## Possible Additional Detection Strategies

### Strategy A: Spectral Flatness Check

Real audio (even quiet ambient) has spectral shape — it's not white noise. Noise from a floating/shorted pin is spectrally flat (equal energy across all frequencies).

- **Implementation**: Compute energy in low-band (0–2kHz) vs high-band (2kHz–Nyquist) using a simple running sum. Real audio is weighted toward lower frequencies. Flat noise has roughly equal energy in both.
- **Threshold**: If `high_band_energy / low_band_energy > 0.8` for sustained periods → likely a wiring fault.
- **Pros**: Directly detects the "sounds blank but has energy" case.
- **Cons**: Adds CPU cost (frequency analysis per frame). Birds have high-frequency content that could approach flat. Rain/wind can also be spectrally broad.

### Strategy B: Temporal Correlation Check

Real audio has temporal structure — adjacent samples are correlated. White/random noise from a fault has near-zero autocorrelation at lag 1.

- **Implementation**: Compute `Σ(sample[i] * sample[i-1]) / Σ(sample[i]²)` — normalized lag-1 autocorrelation.
- **Threshold**: If autocorrelation < 0.1 consistently → likely not real audio.
- **Pros**: Cheap to compute (one pass over samples), very effective at detecting random noise.
- **Cons**: Some real audio (percussive sounds, insects) can have low autocorrelation in short windows.

### Strategy C: Signal Consistency Over Time

Real audio varies in level over time (quiet moments, loud moments). A wiring fault produces a constant noise floor.

- **Implementation**: Track peak-to-peak or RMS across multiple frames over several seconds. If the variance of the RMS is near-zero → fault.
- **Threshold**: If RMS standard deviation < X over 5 seconds → suspect fault.
- **Pros**: Very robust — real outdoor audio always has level variation.
- **Cons**: Requires accumulating history (RAM), slower to react (seconds not frames).

### Strategy D: Comparison Against Known-Good Baseline

Record the Opus frame sizes during a known-good state (first boot with confirmed working mic). If the distribution of frame sizes changes dramatically → fault.

- **Pros**: Learns the specific device's "normal."
- **Cons**: Requires calibration step, NVS storage, complex logic.

---

## Implemented Solution

**Strategy B (temporal correlation)** is the best fit:
- Cheapest to compute (single pass, integer math)
- Most discriminating for the specific failure mode (random noise vs real audio)
- Low false-positive risk (real audio is always correlated at the sample rate we're using)
- Can be evaluated per-frame with no history needed

A floating or shorted data pin produces samples that are essentially random — each sample is independent of the previous one. Real audio from any source (birds, wind, traffic, silence with self-noise) has strong sample-to-sample correlation because acoustic signals are band-limited.

### Strategy B (temporal correlation)** is the best fit:
- Cheapest to compute (single pass, integer math)
- Most discriminating for the specific failure mode (random noise vs real audio)
- Low false-positive risk (real audio is always correlated at the sample rate we're using)
- Can be evaluated per-frame with no history needed

**This strategy has been implemented as Test 5 (Uncorrelated Noise Detection) in `streamAudio()`.**

### Implementation (current code)

```cpp
// Lag-1 autocorrelation (integer approximation)
int64_t crossSum = 0;
int64_t autoSum = 0;
for (int i = 1; i < samplesRead32; i++) {
    crossSum += (int64_t)i2sBuffer[i] * (int64_t)i2sBuffer[i - 1];
    autoSum  += (int64_t)i2sBuffer[i] * (int64_t)i2sBuffer[i];
}
// correlation < 0.3 means the signal lacks temporal structure
// Real audio at 48kHz: typically > 0.5 (often > 0.9)
// Random noise from wiring fault: typically < 0.1
bool uncorrelatedNoise = (autoSum > 0 && crossSum * 10 < autoSum * 3);
```
