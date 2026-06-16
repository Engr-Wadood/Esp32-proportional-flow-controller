#pragma once

#include <Arduino.h>

// MF30 mass flow sensor on a 4-20 mA current loop, read through a shunt
// resistor on an ESP32 ADC1 pin.
//
//   flow [L/min] = 0                                  if mA <= 4.09
//                  (mA - 4.09) / 16 * flowFull + 2    otherwise
//
// The +2 L/min is an empirical correction so the ESP reading matches the
// MFS display. The 4.09 mA cutoff is both the noise floor and the
// reference zero of the linear conversion.
class MF30FlowSensor {
public:
    MF30FlowSensor(uint8_t pin,
                   float shuntOhms = 147.0f,
                   float flowFull  = 1000.0f);

    void  begin();

    // Single ADC sweep (~7 ms, 64 samples).
    float readCurrent() const;

    // Pure conversion from a mA value to L/min.
    float convertToFlow(float mA) const;

    // Convenience: read + convert in one call.
    float readFlow() const;

    // True when the loop looks alive (>= 3.5 mA).
    bool  isConnected() const;

private:
    static constexpr float MA_ZERO        = 4.0f;
    static constexpr float MA_ZERO_CUTOFF = 4.09f;
    static constexpr float MA_FULL        = 20.0f;
    static constexpr float MA_SPAN        = MA_FULL - MA_ZERO;
    static constexpr float CAL_OFFSET_LPM = 2.0f;     // empirical vs MFS display
    static constexpr int   SAMPLES        = 64;

    uint8_t _pin;
    float   _shunt;
    float   _flowFull;
};
