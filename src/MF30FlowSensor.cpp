#include "MF30FlowSensor.h"

MF30FlowSensor::MF30FlowSensor(uint8_t pin, float shuntOhms, float flowFull)
    : _pin(pin), _shunt(shuntOhms), _flowFull(flowFull) {}

void MF30FlowSensor::begin() {
    analogSetPinAttenuation(_pin, ADC_11db);
}

float MF30FlowSensor::readCurrent() const {
    uint32_t sumMv = 0;
    for (int i = 0; i < SAMPLES; i++) {
        sumMv += analogReadMilliVolts(_pin);
        delayMicroseconds(100);
    }
    float volts = (float)sumMv / SAMPLES / 1000.0f;
    return volts / _shunt * 1000.0f;
}

float MF30FlowSensor::convertToFlow(float mA) const {
    if (mA <= MA_ZERO_CUTOFF) return 0.0f;
    float flow = (mA - MA_ZERO_CUTOFF) / MA_SPAN * _flowFull + CAL_OFFSET_LPM;
    return constrain(flow, 0.0f, _flowFull);
}

float MF30FlowSensor::readFlow() const {
    return convertToFlow(readCurrent());
}

bool MF30FlowSensor::isConnected() const {
    return readCurrent() >= 3.5f;
}
