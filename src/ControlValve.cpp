#include "ControlValve.h"

namespace ControlValve {

// --- hardware pin assignments (filled in by begin()) ---
static int PIN_IN1 = -1;
static int PIN_IN2 = -1;
static int PIN_ENA = -1;

// --- runtime state ---
static int     valvePct          = 0;        // 0..100, 0 = closed
static bool    autoMode          = false;
static float   targetFlow        = 0.0f;
static int     inBandCount       = 0;        // consecutive halt-band samples
static int     ticksSinceAction  = 0;        // gates medium/slow speeds
static Speed   speedNow          = IDLE;

static YieldFn yieldCb = nullptr;

// ---------------------------------------------------------------------------
//  Helpers
// ---------------------------------------------------------------------------
// Like delay() but periodically calls the yield callback so the rest of the
// firmware (web server, etc.) can still run during a 4-second forceFullClose.
static void yieldDelay(unsigned long ms) {
  const unsigned long start = millis();
  while (millis() - start < ms) {
    if (yieldCb) yieldCb();
    delay(2);
  }
}

// Energize the H-bridge in one direction. Direction = true -> opening.
static void drive(bool opening) {
  digitalWrite(PIN_IN1, opening ? LOW  : HIGH);
  digitalWrite(PIN_IN2, opening ? HIGH : LOW);
  analogWrite(PIN_ENA, DRIVE_PWM);
}

// ---------------------------------------------------------------------------
//  Public API
// ---------------------------------------------------------------------------
void begin(int in1, int in2, int ena) {
  PIN_IN1 = in1;
  PIN_IN2 = in2;
  PIN_ENA = ena;
  pinMode(PIN_IN1, OUTPUT);
  pinMode(PIN_IN2, OUTPUT);
  pinMode(PIN_ENA, OUTPUT);
  coast();
}

void setYieldCallback(YieldFn cb) { yieldCb = cb; }

void coast() {
  digitalWrite(PIN_IN1, LOW);
  digitalWrite(PIN_IN2, LOW);
  analogWrite(PIN_ENA, 0);
}

void forceFullClose() {
  Serial.printf("[valve] force-closing for %lu ms (-> 0%%)\n", FULL_TRAVEL_MS);
  drive(false);
  yieldDelay(FULL_TRAVEL_MS);
  coast();
  valvePct = 0;
}

void setPosition(int percent) {
  percent = constrain(percent, 0, 100);
  const int delta = percent - valvePct;
  if (delta == 0) return;

  // Run time is proportional to the requested change.
  const unsigned long runMs =
      ((unsigned long)abs(delta) * FULL_TRAVEL_MS) / 100UL;
  drive(delta > 0);
  yieldDelay(runMs);
  coast();
  valvePct = percent;
}

int getPosition() { return valvePct; }

void setTarget(float lpm) {
  targetFlow      = lpm;
  inBandCount     = 0;
  ticksSinceAction = 0;
}
float getTarget()   { return targetFlow; }

void enableAuto() {
  autoMode         = true;
  inBandCount      = 0;
  ticksSinceAction = 0;
}

void disableAuto() {
  autoMode         = false;
  inBandCount      = 0;
  ticksSinceAction = 0;
  speedNow         = IDLE;
}

bool isAutoMode() { return autoMode; }

// ---------------------------------------------------------------------------
//  Controller
//
//  Called periodically by main loop. Picks a speed band from |err|,
//  maintains the in-band confirm counter, and steps the valve at the
//  cadence appropriate for the band.
// ---------------------------------------------------------------------------
void controlTick(float flowLpm, bool connected) {
  if (!autoMode || !connected) { speedNow = IDLE; return; }

  ticksSinceAction++;

  const float err    = targetFlow - flowLpm;
  const float absErr = fabsf(err);

  // Halt band: don't move the valve, just count for the lock indicator.
  if (absErr <= BAND_HALT_LPM) {
    speedNow = HALT;
    if (inBandCount < LOCK_CONFIRM_SAMPLES) inBandCount++;
    return;
  }
  inBandCount = 0;   // left the halt band; reset confirmation

  // Otherwise pick the band's pacing.
  int requiredTicks;
  if (absErr > BAND_FAST_LPM)      { speedNow = FAST;   requiredTicks = TICKS_FAST; }
  else if (absErr > BAND_MED_LPM)  { speedNow = MEDIUM; requiredTicks = TICKS_MED;  }
  else                              { speedNow = SLOW;   requiredTicks = TICKS_SLOW; }

  // Wait until we've allowed enough ticks since the previous nudge.
  if (ticksSinceAction < requiredTicks) return;
  ticksSinceAction = 0;

  const int sign = (err > 0) ? +1 : -1;
  const int next = constrain(valvePct + sign * OPEN_SIGN * STEP_PCT, 0, 100);
  if (next != valvePct) setPosition(next);
}

Speed getSpeed() { return speedNow; }

const char* getSpeedName() {
  switch (speedNow) {
    case FAST:   return "fast";
    case MEDIUM: return "medium";
    case SLOW:   return "slow";
    case HALT:   return "halt";
    default:     return "idle";
  }
}

bool isLocked() { return autoMode && inBandCount >= LOCK_CONFIRM_SAMPLES; }

}  // namespace ControlValve
