# Battery & Solar Panel Monitoring Circuit

Simple always-on voltage divider monitoring for a 12V battery and 12–16V solar panel. No MOSFET switching — the dividers are permanently connected and draw a negligible ~58 µA total.

## Design Goals

1. **Simple & reliable** — passive resistor dividers, no switching components
2. **Measure two sources** — 12V battery (10–14.8V) and solar panel (0–22V open circuit)
3. **ESP32-safe** — all ADC inputs stay within 0–3.1V (ESP32-S3 ADC range with 11dB attenuation)
4. **Minimal parts count** — just 4 resistors and 2 filter capacitors

## Voltage Ranges

| Source        | Min Voltage | Nominal | Max Voltage | Notes                                  |
|---------------|-------------|---------|-------------|----------------------------------------|
| 12V Battery   | 10.0V       | 12.8V   | 14.8V       | Lead-acid (10.5–14.4V) or LiFePO4 (10–14.6V) |
| Solar panel   | 0V          | ~17V    | 22.0V (Voc) | Typical "12V" panel open-circuit under full sun |

## Schematic

```
                   VBAT (+)              VSOLAR (+)
                     │                       │
                     │                       │
                  ┌──┴──┐                 ┌──┴──┐
                  │ R1   │                 │ R3   │
                  │470kΩ │                 │680kΩ │
                  └──┬──┘                 └──┬──┘
                     │                       │
                     ├──── GPIO 8 (ADC)      ├──── GPIO 9 (ADC)
                     │                       │
                  ┌──┴──┐  ┌──┴──┐        ┌──┴──┐  ┌──┴──┐
                  │ R2   │  │ C1   │        │ R4   │  │ C2   │
                  │100kΩ │  │100nF │        │100kΩ │  │100nF │
                  └──┬──┘  └──┬──┘        └──┬──┘  └──┬──┘
                     │        │              │        │
                     └────┬───┘              └────┬───┘
                          │                       │
                         GND                     GND
```

C1 and C2 are connected from the ADC sense node directly to GND, in parallel with R2 and R4 respectively. They filter high-frequency noise on the ADC input.

## How It Works

The resistor dividers are always connected. Current flows continuously through R1/R2 and R3/R4 to ground, producing a scaled-down voltage at the midpoint that the ESP32 ADC can safely read at any time.

The filter capacitors (C1, C2) smooth out high-frequency noise on the ADC inputs. With the high-impedance dividers (490kΩ, 780kΩ total), these 100nF caps give an RC time constant of ~49ms and ~78ms respectively, which is fine since we only read every 60 seconds.

## Component Selection

| Ref | Part        | Value   | Purpose                        | Notes            |
|-----|-------------|---------|--------------------------------|------------------|
| R1  | Resistor    | 470kΩ   | Battery divider upper          | 1% tolerance     |
| R2  | Resistor    | 100kΩ   | Battery divider lower          | 1% tolerance     |
| R3  | Resistor    | 680kΩ   | Solar divider upper            | 1% tolerance     |
| R4  | Resistor    | 100kΩ   | Solar divider lower            | 1% tolerance     |
| C1  | Capacitor   | 100nF   | ADC filter on GPIO 8           | Ceramic, any     |
| C2  | Capacitor   | 100nF   | ADC filter on GPIO 9           | Ceramic, any     |

## Voltage Divider Calculations

### Battery (12V): R1 = 470kΩ, R2 = 100kΩ → ratio = 100:570

```
V_adc = V_bat × R2 / (R1 + R2) = V_bat × 100k / 570k = V_bat / 5.7

At 10.0V (depleted):  V_adc = 1.75V  ✓ (within 0–3.1V)
At 12.8V (nominal):   V_adc = 2.25V  ✓
At 14.8V (max):       V_adc = 2.60V  ✓
```

### Solar Panel: R3 = 680kΩ, R4 = 100kΩ → ratio = 100:780

```
V_adc = V_solar × R4 / (R3 + R4) = V_solar × 100k / 780k = V_solar / 7.8

At 0V (dark):        V_adc = 0.00V  ✓
At 17V (nominal):    V_adc = 2.18V  ✓
At 22V (Voc max):    V_adc = 2.82V  ✓ (within 0–3.1V)
```

### Continuous Current Draw

```
Battery divider:  I = V_bat / (R1 + R2) = 14.8V / 570kΩ = 26.0 µA
Solar divider:    I = V_solar / (R3 + R4) = 22V / 780kΩ = 28.2 µA
Total continuous: ~54 µA (worst case, both at max voltage)
```

At nominal voltages (~12.8V battery, ~17V solar): ~44 µA total. For a 12V battery of any practical size (7Ah+), this is completely negligible — less than 0.02% capacity per day.

## GPIO Assignment

| GPIO | Function     | Direction | Notes                              |
|------|--------------|-----------|-------------------------------------|
| 8    | VBAT_SENSE   | ADC INPUT | ADC1_CH7, 11dB attenuation         |
| 9    | VSOL_SENSE   | ADC INPUT | ADC1_CH8, 11dB attenuation         |

These GPIOs are on ADC1 (which remains available when WiFi is active — ADC2 is not usable with WiFi).

GPIO 7 is no longer used by the power monitor and is free for other purposes.

## Power Budget Impact

| State              | Additional Current Draw |
|--------------------|------------------------|
| Deep sleep         | ~44–54 µA (dividers always on) |
| Active             | ~44–54 µA (same)              |

For context, the ESP32-S3 itself draws ~7 µA in deep sleep. The dividers add ~44 µA. For a 7Ah 12V battery, this totals ~1.2 mAh/day — the battery would last over 15 years from divider draw alone.

## Bill of Materials

| Qty | Part                  | Package | Source   |
|-----|-----------------------|---------|----------|
| 1   | Resistor 470kΩ 1%    | any     | generic  |
| 1   | Resistor 680kΩ 1%    | any     | generic  |
| 2   | Resistor 100kΩ 1%    | any     | generic  |
| 2   | 100nF ceramic capacitor | any  | generic  |

## Wiring Summary (DevKit connections)

```
ESP32-S3 DevKitC              Power Monitor Circuit
─────────────────             ────────────────────
GPIO 8  ──────────────────────  R1/R2 midpoint (battery sense)
GPIO 9  ──────────────────────  R3/R4 midpoint (solar sense)
GND     ──────────────────────  R2 bottom, R4 bottom, C1 bottom, C2 bottom

Battery (+) ──────────────────  R1 top
Solar (+)   ──────────────────  R3 top
Common GND  ──────────────────  Battery (−), Solar (−), ESP32 GND
```

## Validation & Troubleshooting

### Boot Self-Test

The firmware runs a diagnostic self-test at boot (`powerMonitorInit()`). Check serial output for:

```
[Power] ┌─── DIAGNOSTIC SELF-TEST ───────────────────────┐
[Power] │ ADC readings — bat: 2.246V, sol: 0.000V
[Power] │ Calculated  — bat: 12.80V, sol: 0.00V
[Power] │ ✓ OK — battery 12.80V is within 12V range (10.0–14.8V)
[Power] │ ℹ Solar: no voltage detected (panel disconnected or dark)
[Power] └────────────────────────────────────────────────┘
```

### Interpreting Results

| Symptom | Cause | Fix |
|---------|-------|-----|
| Both channels read ~3.1V | ADC pins floating (no divider ground) | Check R2/R4 bottom connections to GND |
| Battery reads 0V | Divider upper leg open or battery disconnected | Check R1 connection to battery (+) |
| Battery reads >2.60V ADC (>14.8V real) | Wrong divider ratio or cross-wired | Verify R1=470kΩ, R2=100kΩ |
| Solar reads same as battery | Pins bridged or R3/R4 miswired | Check for solder bridges between channels |

### HTTP Diagnostic Mode

Use the `/diag/start` endpoint for live validation with faster telemetry:

```bash
# Start diagnostic mode (sends telemetry every 2s, enables signal LED)
curl http://esp32-birdnet.local/diag/start

# Check current readings
curl http://esp32-birdnet.local/status

# Stop diagnostic mode
curl http://esp32-birdnet.local/diag/stop
```

### Multimeter Validation Steps

1. **Verify divider midpoints**:
   - GPIO 8 to GND: should read `V_bat / 5.7` (e.g., 2.25V for a 12.8V battery)
   - GPIO 9 to GND: should read `V_solar / 7.8` (e.g., 2.18V for a 17V panel)

2. **Verify ground path**:
   - Confirm continuity between R2 bottom and ESP32 GND
   - Confirm continuity between R4 bottom and ESP32 GND

3. **Verify no-solar condition**:
   - With solar disconnected, GPIO 9 should read ~0V
   - If it reads the same as battery → pins may be bridged

## Calibration

The ESP32-S3 ADC has per-chip variation (typically ±3–6%). Two calibration approaches:

### Option 1: Reference Voltage Calibration (Recommended)

Apply a known voltage and compare to the reported ADC reading.

1. Connect a known, stable voltage source to the battery divider input (e.g., a bench supply set to exactly 12.000V)
2. Read the reported ADC voltage from serial or `/status` endpoint
3. Calculate your chip's actual Vref:

```
actual_vref = known_voltage / 5.7 / reported_adc_voltage × ADC_VREF

Example: Supply = 12.000V, divider output should be 2.105V
         ESP32 reports 2.060V → actual_vref = 12.000 / 5.7 / 2.060 × 3.1 = 3.167V
```

4. Update `ADC_VREF` in `main.cpp`:

```cpp
#define ADC_VREF  3.168f  // Calibrated for this specific ESP32-S3
```

### Option 2: Per-Channel Scale Factors

If both channels have different offsets (due to resistor tolerance), apply individual correction factors:

1. Measure the actual battery voltage with a multimeter: e.g., 12.82V
2. Read the firmware's calculated value from serial: e.g., `bat: 12.55V`
3. Compute scale factor: `12.82 / 12.55 = 1.022`
4. Apply in `readPowerMonitor()`:

```cpp
lastBatteryVoltage = vBatAdc * 1.022f;  // calibrated correction
lastSolarVoltage   = vSolAdc * 1.005f;  // calibrated correction (measure separately)
```

### Option 3: ESP-IDF eFuse Calibration (Most Accurate)

The ESP32-S3 stores factory ADC calibration data in eFuse. For maximum accuracy, use the `esp_adc_cal` API:

```cpp
#include <esp_adc_cal.h>

esp_adc_cal_characteristics_t adcCal;
esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 0, &adcCal);

// Then in readAdcVoltage():
uint32_t voltage_mv;
esp_adc_cal_get_voltage(ADC_CHANNEL_7, &adcCal, &voltage_mv);
return voltage_mv / 1000.0f;
```

This accounts for per-chip Vref variation and non-linearity. It's the most accurate method but requires using the IDF ADC APIs instead of Arduino's `analogRead()`.

### Resistor Tolerance Impact

With 1% resistors, expect up to 2% error from the divider ratio alone:

| Divider | Nominal Ratio | Worst-Case Error |
|---------|---------------|------------------|
| Battery (470k/100k) | 0.1754 | ±1.6% → 0.173–0.178 |
| Solar (680k/100k) | 0.1282 | ±1.7% → 0.126–0.130 |

For the battery channel, this means a 12.80V battery could read anywhere from 12.54V to 13.06V due to resistor tolerance alone, before ADC error is factored in. This is acceptable for a "battery low" warning but not for precise SoC estimation.

## Notes

- **ADC calibration**: The ESP32-S3 ADC has per-chip variation. For more accurate readings, use `esp_adc_cal` APIs with eFuse calibration data, or calibrate with a known reference voltage.
- **Protection**: If there's risk of voltages exceeding 22V on the solar input (e.g., during load dump), add a 3.3V Zener diode from each ADC pin to GND as overvoltage protection.
- **Measurement frequency**: A reading every 30–60 seconds during active mode is plenty. Each ADC read takes <2ms.
- **12V battery SoC**: Lead-acid batteries have a well-defined resting voltage vs SoC curve (12.7V=100%, 12.0V=25%, 11.8V=0%). LiFePO4 4S packs have a flatter curve (13.6V=100%, 12.0V=10%). Voltage-based SoC is more reliable for 12V systems than for NiMH, but load and temperature still affect accuracy. A linear approximation between the empty and full thresholds is adequate for low-battery warnings.
- **Why no MOSFET**: At 12V with high-value divider resistors (570kΩ and 780kΩ total), the continuous draw is only ~54 µA. For any 12V battery of practical size, this is negligible. Removing the MOSFET eliminates one GPIO, one MOSFET, one pull-down resistor, and all the switching/settling logic — a worthwhile simplification.
