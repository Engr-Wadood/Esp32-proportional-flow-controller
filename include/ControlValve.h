#pragma once

#include <Arduino.h>

// ===========================================================================
//  ControlValve
//
//  Proportional control valve on an L298N H-bridge channel, plus a
//  four-band closed-loop flow controller.
//
//  Hardware (per-channel L298N):
//    IN1/IN2 set rotation direction (one HIGH, one LOW)
//    ENA receives a PWM signal: 0 = coast, 255 = full speed
//    One full 0% -> 100% stroke takes FULL_TRAVEL_MS milliseconds.
//
//  Control strategy (call controlTick() at ~200 ms intervals):
//
//      |err| > 15 L/min -> FAST    1% every 1 tick   (5.00 %/s)
//      |err| > 10 L/min -> MEDIUM  1% every 3 ticks  (1.67 %/s)
//      |err| > 4  L/min -> SLOW    1% every 6 ticks  (0.83 %/s)
//      |err| <= 4 L/min -> HALT    valve frozen (true deadband)
//
//  After LOCK_CONFIRM_SAMPLES (5) consecutive samples inside the halt
//  band the module exposes a `locked` flag - purely informational, the
//  valve has been still since it entered the band.
// ===========================================================================
namespace ControlValve {

// --- tuning ---
constexpr unsigned long FULL_TRAVEL_MS         = 4000;
constexpr int           DRIVE_PWM              = 255;
constexpr int           OPEN_SIGN              = +1;     // -1 if open=vent
constexpr float         BAND_FAST_LPM          = 15.0f;
constexpr float         BAND_MED_LPM           = 10.0f;
constexpr float         BAND_HALT_LPM          = 4.0f;
constexpr int           STEP_PCT               = 1;
constexpr int           TICKS_FAST             = 1;
constexpr int           TICKS_MED              = 3;
constexpr int           TICKS_SLOW             = 6;
constexpr int           LOCK_CONFIRM_SAMPLES   = 5;

enum Speed { IDLE, FAST, MEDIUM, SLOW, HALT };

// Yield callback type. Called periodically during long blocking moves
// (e.g. forceFullClose) so the main loop's web server can still service
// requests. Optional; pass nullptr to disable.
typedef void (*YieldFn)();

// One-time setup: pinModes + coast.
void begin(int pinIn1, int pinIn2, int pinEna);

// Install the yield callback used during long blocking moves.
void setYieldCallback(YieldFn cb);

// --- direct hardware control --------------------------------------------
// Cut the H-bridge: both INs LOW, ENA = 0.
void coast();

// Drive the valve fully closed for one full stroke (FULL_TRAVEL_MS) and
// pin the software position to 0%. This is the only way to be sure of
// the valve's actual position at boot or after a power blip.
void forceFullClose();

// Move the valve to an absolute percent (0-100). Blocking, proportional
// to the delta. Uses the yield callback during the wait.
void setPosition(int percent);

// Current software-tracked valve position in percent (0 = closed).
int  getPosition();

// --- closed-loop control ------------------------------------------------
void  setTarget(float lpm);
float getTarget();

// Enables/disables the closed-loop. Doesn't touch hardware on its own -
// call forceFullClose() first if you need a known zero, and stop the
// pump separately if you want everything off.
void  enableAuto();
void  disableAuto();
bool  isAutoMode();

// Run one control tick. Must be called periodically (~200 ms). Provide
// the latest flow reading and whether the sensor is healthy.
void  controlTick(float flowLpm, bool sensorConnected);

// --- status (refreshed by controlTick) ----------------------------------
Speed       getSpeed();
const char* getSpeedName();
bool        isLocked();

}  // namespace ControlValve
