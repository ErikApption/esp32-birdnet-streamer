# Battery & Solar Panel Monitoring Circuit

ESP32-triggered voltage monitoring for a 3S NiMH battery pack and 7V solar panel, with zero quiescent current when idle.

## Design Goals

1. **Zero standby current** — no power drawn by the monitoring circuit unless the ESP32 actively triggers a reading
2. **Measure two sources** — 3S NiMH battery (3.6V–4.5V) and solar panel (0–7V open circuit)
3. **ESP32-safe** — all ADC inputs must stay within 0–3.1V (ESP32-S3 ADC range with 11dB attenuation)
4. **Minimal parts count** — no ICs beyond what the ESP32 provides

## Voltage Ranges

| Source        | Min Voltage | Nominal | Max Voltage | Notes                       |
|---------------|-------------|---------|-------------|-----------------------------|
| 3S NiMH      | 3.0V        | 3.6V    | 4.5V        | 1.0–1.5V per cell × 3      |
| Solar panel   | 0V          | ~5.5V   | 7.0V (Voc)  | Open-circuit under full sun |


### Simplified Schematic (N-channel approach — recommended)

Using a **FQP30N06L** logic-level N-channel MOSFET on the low side:

```
                   VBAT (+)              VSOLAR (+)
                     │                       │
                     │                       │
                  ┌──┴──┐                 ┌──┴──┐
                  │ R1   │                 │ R3   │
                  │100kΩ │                 │220kΩ │
                  └──┬──┘                 └──┬──┘
                     │                       │
                     ├──── GPIO 8 (ADC)      ├──── GPIO 9 (ADC)
                     │                       │
                     ├──┤C1├──┐              ├──┤C2├──┐
                     │  100nF │              │  100nF │
                     │  (+) (−)│              │  (+) (−)│
                     │        │              │        │
                  ┌──┴──┐     │           ┌──┴──┐     │
                  │ R2   │     │           │ R4   │     │
                  │100kΩ │     │           │100kΩ │     │
                  └──┬──┘     │           └──┬──┘     │
                     │        │              │        │
                     └────┬───┘              └────┬───┘
                          │                       │
                          └───────────┬───────────┘
                                      │
                                      D (Drain)
                                 ┌────┴────┐
                                 │  Q1     │  FQP30N06L
                                 │         │  (TO-220)
                                 └────┬────┘
                                 G    │ S
                                 │    │
                        GPIO 7 ──┘    └─── GND
                          │
                        (10kΩ pull-down to GND)
```

**Capacitor polarity**: C1 and C2 are wired with their positive (+) terminal on the ADC sense node (higher voltage side) and negative (−) terminal on the switched ground rail (SW_GND). This is correct because when the MOSFET is on, the ADC midpoint is always at a higher potential than the drain (which is near 0V).

## How It Works

1. **Idle state**: GPIO 7 is LOW (or Hi-Z during deep sleep). The N-channel MOSFET Q1 is OFF. No current flows through the resistor dividers. **Zero quiescent current.**

2. **Measurement**: The ESP32 drives GPIO 7 HIGH, turning on Q1. Current now flows through both voltage dividers. After a brief settling time (~1ms), the ESP32 reads ADC on GPIO 8 and GPIO 9.

3. **Shutdown**: GPIO 7 is driven LOW again. Q1 turns off. Dividers draw zero current.

## Component Selection

Using parts from available stock. **Selected: FQP30N06L** (N-channel logic-level MOSFET).

| Ref | Part        | Value   | Purpose                        | Notes                                       |
|-----|-------------|---------|--------------------------------|---------------------------------------------|
| R1  | Resistor    | 100kΩ   | Battery divider upper          | 1% tolerance                                |
| R2  | Resistor    | 100kΩ   | Battery divider lower          | 1% tolerance                                |
| R3  | Resistor    | 220kΩ   | Solar divider upper            | 1% tolerance                                |
| R4  | Resistor    | 100kΩ   | Solar divider lower            | 1% tolerance                                |
| R5  | Resistor    | 10kΩ    | Gate pull-down (keeps Q1 off)  | Ensures MOSFET is off during ESP32 boot/sleep |
| Q1  | N-MOSFET    | **FQP30N06L** | Low-side switch            | Logic-level, Vgs(th) 1–2V, fully on at 3.3V |
| C1  | Capacitor   | 100nF   | ADC filter on GPIO 8           | Polarized: (+) to VBAT_SENSE, (−) to SW_GND |
| C2  | Capacitor   | 100nF   | ADC filter on GPIO 9           | Polarized: (+) to VSOL_SENSE, (−) to SW_GND |

### Available MOSFETs — Suitability Assessment

| Part        | Type      | Vgs(th) | Rds(on)   | Works at 3.3V? | Verdict                                |
|-------------|-----------|---------|-----------|----------------|----------------------------------------|
| **FQP30N06L** | N-ch logic| 1–2V  | 0.035Ω    | ✓ Yes          | **Selected** — logic-level, guaranteed on at 3.3V |
| **IRLZ44N** | N-ch logic| 1–2V    | 0.022Ω    | ✓ Yes          | Also excellent, interchangeable        |
| **BS250**   | P-ch      | -1 to -3.5V | 14Ω  | ⚠ Marginal     | Max threshold -3.5V is too close to 3.3V; some units may not fully turn on |
| **VP3203**  | P-ch      | -1 to -3V | 0.6Ω    | ✓ Yes          | Good P-channel option (high-side switch alternative) |
| **LP0701**  | —         | —       | —         | ?              | Unable to verify — uncommon part number |

**Why FQP30N06L**: It's a logic-level N-channel MOSFET. The "L" suffix means it's specifically designed for logic-level gate drive. At Vgs = 3.3V it's well past threshold and has very low on-resistance (35mΩ). Yes, it's rated for 30A which is absurd for 50µA, but that's irrelevant — it works, it's reliable, and the gate leakage is negligible.

**Alternative — VP3203 as high-side switch**: If you prefer to switch the supply rail instead of ground, the VP3203 can work as a P-channel high-side switch. Wire its source to the voltage being measured, gate to GPIO (through a level-shift consideration), and drain to the divider top. However, for simplicity, the N-channel low-side approach is recommended.

## Voltage Divider Calculations

### Battery (3S NiMH): R1 = R2 = 100kΩ → ratio = 1:2

```
V_adc = V_bat × R2 / (R1 + R2) = V_bat × 100k / 200k = V_bat / 2

At 3.0V (depleted):  V_adc = 1.50V  ✓ (within 0–3.1V)
At 3.6V (nominal):   V_adc = 1.80V  ✓
At 4.5V (full):      V_adc = 2.25V  ✓
```

### Solar Panel: R3 = 220kΩ, R4 = 100kΩ → ratio = 100:320

```
V_adc = V_solar × R4 / (R3 + R4) = V_solar × 100k / 320k = V_solar / 3.2

At 0V (dark):        V_adc = 0.00V  ✓
At 5.5V (nominal):   V_adc = 1.72V  ✓
At 7.0V (Voc max):   V_adc = 2.19V  ✓ (within 0–3.1V)
```

### Current Through Dividers (only when enabled)

```
Battery divider:  I = V_bat / (R1 + R2) = 4.5V / 200kΩ = 22.5 µA
Solar divider:    I = V_solar / (R3 + R4) = 7.0V / 320kΩ = 21.9 µA
Total when measuring: ~44 µA (negligible, and only during the brief measurement window)
```

## GPIO Assignment

| GPIO | Function     | Direction | Notes                              |
|------|--------------|-----------|-------------------------------------|
| 7    | MONITOR_EN   | OUTPUT    | HIGH to enable dividers, LOW to disable |
| 8    | VBAT_SENSE   | ADC INPUT | ADC1_CH7, 11dB attenuation         |
| 9    | VSOL_SENSE   | ADC INPUT | ADC1_CH8, 11dB attenuation         |

These GPIOs are free on the ESP32-S3-DevKitC-1 and are on ADC1 (which remains available when WiFi is active — ADC2 is not usable with WiFi).



### Deep sleep compatibility

During deep sleep, all GPIOs default to Hi-Z. The 10kΩ pull-down on Q1's gate ensures the MOSFET stays OFF, so the dividers consume **zero current** during deep sleep — no RTC GPIO configuration needed.

## Power Budget Impact

| State              | Additional Current Draw |
|--------------------|------------------------|
| Deep sleep         | 0 µA (MOSFET off)      |
| Active, not reading| 0 µA (MOSFET off)      |
| During measurement | ~50 µA for ~5ms        |
| Effective average  | negligible             |

## Bill of Materials

| Qty | Part                  | Package | Source   |
|-----|-----------------------|---------|----------|
| 3   | Resistor 100kΩ 1%    | any     | generic  |
| 1   | Resistor 220kΩ 1%    | any     | generic  |
| 1   | Resistor 10kΩ        | any     | generic  |
| 1   | **FQP30N06L** N-MOSFET | TO-220  | from stock |
| 2   | 100nF capacitor (polarized) | radial  | generic — (+) to sense node, (−) to SW_GND |

### FQP30N06L Pinout (TO-220, facing label)

```
    ┌──────────┐
    │ FQP30N06L│
    │          │
    └─┬──┬──┬──┘
      │  │  │
      G  D  S
      │  │  │
      │  │  └─── GND
      │  └────── Divider common ground (R2 + R4 bottom)
      └───────── GPIO 7 (+ 10kΩ pull-down to GND)
```

## Wiring Summary (DevKit connections)

```
ESP32-S3 DevKitC              Power Monitor Circuit
─────────────────             ────────────────────
GPIO 7  ──────────────────────  Q1 Gate (+ 10kΩ to GND)
GPIO 8  ──────────────────────  R1/R2 midpoint (battery sense)
GPIO 9  ──────────────────────  R3/R4 midpoint (solar sense)
GND     ──────────────────────  Q1 Source, R5 to GND

Battery (+) ──────────────────  R1 top
Solar (+)   ──────────────────  R3 top
Common GND  ──────────────────  Battery (−), Solar (−), ESP32 GND

Capacitor Polarity:
  C1: (+) → GPIO 8 / R1-R2 midpoint,  (−) → SW_GND (Q1 drain)
  C2: (+) → GPIO 9 / R3-R4 midpoint,  (−) → SW_GND (Q1 drain)
```

## Notes

- **Why FQP30N06L is fine here**: Yes, it's a 30A/60V power MOSFET for a circuit drawing 50µA. But what matters is the guaranteed logic-level threshold (1–2V) — at 3.3V Vgs it's hard on with negligible Rds (35mΩ). Gate leakage is in the nanoamp range. The TO-220 package is physically large but electrically perfect.
- **IRLZ44N is interchangeable**: Same pinout (G-D-S facing label), same logic-level threshold. Use whichever you grab first.
- **ADC calibration**: The ESP32-S3 ADC has per-chip variation. For more accurate readings, use `esp_adc_cal` APIs with eFuse calibration data, or calibrate with a known reference voltage.
- **Protection**: If there's risk of voltages exceeding 7V on the solar input (e.g., during load dump), add a 3.3V Zener diode from each ADC pin to GND as overvoltage protection.
- **Measurement frequency**: A reading every 30–60 seconds during active mode is plenty. Each measurement takes <10ms total.
- **NiMH discharge curve**: NiMH AA cells have a very flat discharge curve (~1.2V for most of their capacity). Voltage-based SoC is inherently imprecise for NiMH — expect ±20% accuracy. The linear approximation (3.0V empty → 4.2V full) is acceptable for a "battery low" warning but not for precise capacity tracking.
