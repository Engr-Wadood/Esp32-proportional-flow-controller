/*
 * MF30FlowSensor — MF30 Micro Thermal Mass Flow Meter (4-20mA) reader
 *
 * Usage:
 *   #include "MF30FlowSensor.h"
 *
 *   MF30FlowSensor flowSensor(32);          // ADC pin
 *
 *   void setup() {
 *       flowSensor.begin();
 *   }
 *
 *   void loop() {
 *       float flow = flowSensor.readFlow();  // NL/min
 *       float mA   = flowSensor.readCurrent();
 *       bool  ok   = flowSensor.isConnected();
 *   }
 */

#pragma once

#include <Arduino.h>

class MF30FlowSensor {
public:
    // pin        : ADC1 GPIO (32, 33, 34, 35, 36, 39)
    // shuntOhms  : measured shunt resistor value
    // flowFull   : flow at 20 mA (MF30 = 1000 NL/min)
    MF30FlowSensor(uint8_t pin,
                   float shuntOhms = 147.0f,
                   float flowFull  = 1000.0f)
        : _pin(pin), _shunt(shuntOhms), _flowFull(flowFull) {}

    void begin() {
        analogSetPinAttenuation(_pin, ADC_11db);  // 0–3.1V usable range
    }

    // Loop current in mA (averaged, factory-calibrated ADC)
    float readCurrent() const {
        uint32_t sumMv = 0;
        for (int i = 0; i < SAMPLES; i++) {
            sumMv += analogReadMilliVolts(_pin);
            delayMicroseconds(100);
        }
        float volts = (float)sumMv / SAMPLES / 1000.0f;
        return volts / _shunt * 1000.0f;
    }

    // Flow in NL/min. Sensor span: 4 mA = 0, 20 mA = _flowFull.
    // Software zero at MA_ZERO_CUTOFF (4.09 mA) suppresses idle ADC noise;
    // above cutoff, scale with the same 16 mA sensor span so full scale stays ~1000.
    float readFlow() const {
        float mA = readCurrent();
        if (mA <= MA_ZERO_CUTOFF) return 0.0f;

        float flow = (mA - MA_ZERO_CUTOFF) / MA_SPAN * _flowFull;
        return constrain(flow, 0.0f, _flowFull);
    }

    // True when loop current is plausible (sensor powered and wired).
    // Below ~3.5 mA the loop is broken, unpowered, or disconnected.
    bool isConnected() const {
        return readCurrent() >= 3.5f;
    }

private:
    static constexpr float MA_ZERO          = 4.0f;   // sensor live zero (datasheet)
    static constexpr float MA_ZERO_CUTOFF   = 4.09f;  // software zero (idle ADC noise)
    static constexpr float MA_FULL          = 20.0f;
    static constexpr float MA_SPAN          = MA_FULL - MA_ZERO;  // 16 mA
    static constexpr int   SAMPLES = 64;

    uint8_t _pin;
    float   _shunt;
    float   _flowFull;
};
