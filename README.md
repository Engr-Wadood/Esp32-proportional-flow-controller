# ESP32 ↔ MF30 Mass Flow Sensor (4-20mA) Integration

Read flow values from a **JUJIE MF30 Micro Thermal Mass Flow Meter** (model JJMF30-20GW3AAPD, with display module) on an ESP32 over its **4-20mA current loop output**.

> **Important:** the MF30 units in this project were ordered with the **4-20mA output option only** — they do **not** have RS485 fitted (4-20mA and RS485 are mutually exclusive factory options on this meter). The MF50 units have both.

---

## How the communication works

The sensor does not send digital data. It regulates a **current** (not a voltage) on its output loop, proportional to the measured flow:

| Loop current | Flow (MF30, DN20) |
|--------------|--------------------|
| 4 mA         | 0 NL/min ("live zero") |
| 20 mA        | 1000 NL/min (full scale) |

Linear in between (sensor datasheet):

```
flow [NL/min] = (I_mA − 4) / 16 × 1000
```

In software, idle ADC noise near 4.0 mA is handled with a **4.09 mA cutoff** — at or below that reads as 0. Above the cutoff:

```
flow [NL/min] = (I_mA − 4.09) / 16 × 1000
```

The denominator stays 16 mA (4 → 20) so full scale at 20 mA remains ~1000 NL/min.

The ESP32 cannot measure current directly, so a **shunt resistor** converts the loop current into a voltage that an ADC pin can read:

```
V_adc = I × R_shunt          (e.g. 4 mA × 147 Ω = 0.588 V,  20 mA × 147 Ω = 2.94 V)
```

Because current loops are immune to wire resistance and noise pickup, this works reliably even with long cables.

---

## Components required

| Qty | Component | Notes |
|-----|-----------|-------|
| 1 | ESP32 dev board (NodeMCU-32S or similar) | Any board with ADC1 pins free |
| 1 | MF30 mass flow meter with display module | 4-20mA output version |
| 1 | 24 V DC power supply, ≥ 2.5 W per sensor | Powers the meter |
| 1 | **147 Ω – 150 Ω resistor, ≥ ¼ W** (shunt) | Sets 4-20 mA → 0.59-2.94 V |
| — | Hookup wire | — |

**Shunt value rule:** at 20 mA, the voltage must stay below the ESP32 ADC's usable limit (~3.1 V with 11 dB attenuation):
`R_max = 3.1 V / 20 mA = 155 Ω`. 147-150 Ω is ideal. Do **not** use 250 Ω (would give 5 V).

---

## Wiring

The sensor's aviation cable has 4 wires:

| Wire | Colour | Function | Connect to |
|------|--------|----------|------------|
| 1 | Red | 24 V + | 24 V PSU **+** |
| 2 | Black | 24 V − | 24 V PSU **−** **and** ESP32 **GND** |
| 3 | White | I+ (4-20mA out) | ESP32 **GPIO32** and one leg of shunt |
| 4 | Green/Yellow | I− (loop return) | ESP32 **GND** |

```
 24V PSU (+) ───────────────── Red ──────► MF30 24V+
 24V PSU (−) ──┬────────────── Black ────► MF30 24V−
               │
             ESP32 GND ◄────── Green/Yellow (I−) ◄── MF30
               │
             [147 Ω]
               │
 ESP32 GPIO32 ─┴────────────── White (I+) ◄──────── MF30
```

**Three points must share the same GND:** 24 V PSU −, the I− wire, and the bottom leg of the shunt resistor. Without the common ground the loop is open and the ADC reads 0.

### ADC pin choice

Use **ADC1** pins only (GPIO32, 33, 34, 35, 36, 39) — ADC2 pins stop working when Wi-Fi is on. GPIO34/35/36/39 are input-only, which is fine for this.

---

## Software

### Files

| File | Purpose |
|------|---------|
| `src/MF30FlowSensor.h` | Self-contained reader class — drop into any project |
| `src/main.cpp` | Minimal example |

### Usage in your own code

```cpp
#include "MF30FlowSensor.h"

MF30FlowSensor flowSensor(32);            // ADC pin (shunt = 147 Ω default)

void setup() {
    flowSensor.begin();
}

void loop() {
    float flow = flowSensor.readFlow();      // NL/min, clamped 0-1000
    float mA   = flowSensor.readCurrent();   // raw loop current
    bool  ok   = flowSensor.isConnected();   // false if loop broken (< 3.5 mA)
}
```

Constructor parameters if your hardware differs:

```cpp
MF30FlowSensor sensor(pin, shuntOhms, flowFullScale);
MF30FlowSensor sensor(33, 150.0f, 1000.0f);   // example
```

Multiple sensors — one instance per ADC pin:

```cpp
MF30FlowSensor s1(32), s2(33), s3(34);
```

### Implementation notes

- Uses `analogReadMilliVolts()` — applies each chip's **factory eFuse calibration**. Raw `analogRead()` has a large offset at low voltages (we measured 3.03 mA reported vs 3.95 mA actual before switching).
- Averages **64 samples** per reading to suppress noise (~7 ms per call).
- `readFlow()` returns 0 at or below 4.09 mA; above that uses `(I − 4.09) / 16 × 1000` NL/min.
- `isConnected()` returns false below 3.5 mA — a healthy loop never goes below 4 mA, so this detects unpowered/disconnected sensors.

---

## Calibration check (verified with multimeter)

| Sensor display | Voltage across 147 Ω shunt | Loop current | Expected |
|----------------|---------------------------|--------------|----------|
| 0 L/min | 0.580 V | 3.95 mA | 4.0 mA ✓ |
| 193 L/min | 1.030 V | 7.01 mA | 7.09 mA ✓ |

Resolution: ≈ 2.35 mV per NL/min at the ADC — the ESP32 resolves flow changes of ~1-2 NL/min.

---

## Build & run

PlatformIO (NodeMCU-32S, Arduino framework):

```bash
pio run -t upload
pio device monitor        # 115200 baud
```

Expected output:

```
=== MF30 Flow Sensor (GPIO32) ===
4.06 mA   3.8 NL/min
6.12 mA   132.8 NL/min
...
```

---

## Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| Always 0.00 mA | Loop open — no return path | Connect I− (green/yellow) and 24 V− to ESP32 GND |
| Reads ~1-2 mA noise, never 4 mA | Wrong ADC pin / sensor on different pin | Verify which GPIO the white wire goes to |
| mA reading ~25 % low at zero flow | Raw `analogRead()` used instead of calibrated read | Use `analogReadMilliVolts()` (already done in this class) |
| Flow stuck at 1000 NL/min | Shunt too large or short on ADC pin | Use ≤ 150 Ω shunt |
| Sensor display works but ESP32 reads nothing | Common GND missing | Tie all three GND points together |
