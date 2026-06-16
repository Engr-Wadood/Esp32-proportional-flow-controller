// ===========================================================================
//  ESP32 flow controller - main entry point
//
//  Top-level wiring. All hardware logic lives in the three modules:
//
//    * MF30FlowSensor  - reads the 4-20 mA flow loop on an ADC pin
//    * ControlValve    - drives the proportional valve via L298N channel A
//                        and runs the closed-loop flow controller
//    * WebInterface    - Wi-Fi + HTTP server + the single-page web UI
//
//  This file owns:
//    * pin map and Wi-Fi credentials
//    * the air pump (a few lines of L298N channel B PWM)
//    * the serial command UI
//    * the snapshot/handler glue passed to WebInterface
//    * setup() and loop() scheduling
// ===========================================================================

#include <Arduino.h>
#include "MF30FlowSensor.h"
#include "ControlValve.h"
#include "WebInterface.h"

// ---------------------------------------------------------------------------
//  Configuration
// ---------------------------------------------------------------------------

// Wi-Fi (STA). Falls back to open AP "ESP32-FlowCtrl" if STA times out.
static const char*         WIFI_SSID            = "SOGTech";
static const char*         WIFI_PASSWORD        = "D5hWk9Nc$QaLp";
static const unsigned long WIFI_TIMEOUT_MS      = 20000;

// Pin map
#define FLOW_SENSOR_PIN  32    // ADC1, shunt across I+ -> GND

#define VALVE_IN1        25    // L298N channel A (proportional valve)
#define VALVE_IN2        26
#define VALVE_ENA        27    // PWM

#define PUMP_IN3         18    // L298N channel B (air pump)
#define PUMP_IN4         19
#define PUMP_ENB         23    // PWM

// Pump default duty when entering auto mode.
static const int           PUMP_DEFAULT_PCT     = 50;

// Loop scheduling - one ADC sweep and one control evaluation per tick.
static const unsigned long SENSE_INTERVAL_MS    = 200;
static const unsigned long CONTROL_INTERVAL_MS  = 200;
static const unsigned long PRINT_INTERVAL_MS    = 1000;

// ---------------------------------------------------------------------------
//  Globals
// ---------------------------------------------------------------------------
static MF30FlowSensor flowSensor(FLOW_SENSOR_PIN);

// Single source of truth for sensor readings. Refreshed once per
// SENSE_INTERVAL_MS in loop(); every other consumer (control loop,
// status print, web UI snapshot) reads these cached values so the mA
// number and the L/min number always come from the same ADC sweep.
static float cachedFlow_lpm   = 0.0f;
static float cachedCurrent_mA = 0.0f;
static bool  cachedConnected  = false;

// Pump duty (0..100) mirrored here for the web/serial status.
static int   pumpPct          = 0;

// ===========================================================================
//  Pump (L298N channel B - constant-direction PWM)
// ===========================================================================
static void setPumpPercent(int pct) {
  pct = constrain(pct, 0, 100);
  pumpPct = pct;
  if (pct == 0) {
    digitalWrite(PUMP_IN3, LOW);
    digitalWrite(PUMP_IN4, LOW);
    analogWrite(PUMP_ENB, 0);
  } else {
    digitalWrite(PUMP_IN3, HIGH);
    digitalWrite(PUMP_IN4, LOW);
    analogWrite(PUMP_ENB, map(pct, 0, 100, 0, 255));
  }
}

static void pumpBegin() {
  pinMode(PUMP_IN3, OUTPUT);
  pinMode(PUMP_IN4, OUTPUT);
  pinMode(PUMP_ENB, OUTPUT);
  setPumpPercent(0);
}

// ===========================================================================
//  High-level mode helpers (shared by serial + web UIs)
// ===========================================================================

// Stop the closed loop and the pump. The valve is coasted (i.e. left in
// place, no driving signal) - it does NOT force-close, because doing so
// can take 4 s and the user pressed STOP to halt things.
static void stopAll(const char* reason) {
  ControlValve::disableAuto();
  ControlValve::coast();
  setPumpPercent(0);
  Serial.printf("Stopped (%s). Pump off, valve at %d%%.\n",
                reason, ControlValve::getPosition());
}

// Begin (or update) closed-loop control. If we're entering from idle
// we force-close the valve to a known 0% reference and turn the pump on
// at the default duty. If auto is already running, we just update the
// setpoint - no re-zero, no pump kick.
static void startAuto(float targetLpm) {
  if (targetLpm <= 0.0f) { stopAll("target <= 0"); return; }

  ControlValve::setTarget(targetLpm);

  if (!ControlValve::isAutoMode()) {
    Serial.printf("Starting auto control. Target = %.1f L/min\n", targetLpm);
    ControlValve::forceFullClose();         // -100 recovery
    setPumpPercent(PUMP_DEFAULT_PCT);       // pump on at 50 %
    ControlValve::enableAuto();
    Serial.println("Auto mode ON.");
  } else {
    Serial.printf("Target updated to %.1f L/min (auto still ON).\n", targetLpm);
  }
}

// ===========================================================================
//  Web glue
//
//  WebInterface knows nothing about the rest of the system - we feed it
//  a snapshot provider (for /data) and three callbacks (for the POSTs).
// ===========================================================================
static WebInterface::Snapshot buildSnapshot() {
  WebInterface::Snapshot s;
  s.flow_lpm   = cachedFlow_lpm;
  s.current_mA = cachedCurrent_mA;
  s.valve_pct  = ControlValve::getPosition();
  s.target_lpm = ControlValve::getTarget();
  s.pump_pct   = pumpPct;
  s.autoMode   = ControlValve::isAutoMode();
  s.connected  = cachedConnected;
  s.locked     = ControlValve::isLocked();
  s.speed      = ControlValve::getSpeedName();
  return s;
}

static void webOnTarget(float lpm) { startAuto(lpm); }
static void webOnPump(int pct) {
  setPumpPercent(pct);
  Serial.printf("[web] pump set to %d%%\n", pumpPct);
}
static void webOnStop() { stopAll("web"); }

// ===========================================================================
//  Serial UI (fallback / debugging)
// ===========================================================================
static void printHelp() {
  Serial.println(F("Enter a number = target flow in L/min. 0 stops everything."));
  Serial.println(F("Commands: auto <lpm> | stop | pump <0-100> | valve <0-100> | -100 | r | ?"));
}

static void printStatus() {
  const char* state =
        !ControlValve::isAutoMode() ? "off"
      :  ControlValve::isLocked()   ? "LOCKED"
                                    : ControlValve::getSpeedName();
  Serial.printf("auto=%s  target=%.1f  flow=%.1f L/min  mA=%.2f  valve=%d%%  pump=%d%%\n",
                state, ControlValve::getTarget(),
                cachedFlow_lpm, cachedCurrent_mA,
                ControlValve::getPosition(), pumpPct);
}

// "<name> <arg>" -> two trimmed strings, name lower-cased.
static void splitCommand(const String& cmd, String& name, String& arg) {
  const int sep = cmd.indexOf(' ');
  if (sep < 0) { name = cmd; arg = ""; }
  else         { name = cmd.substring(0, sep); arg = cmd.substring(sep + 1); }
  name.toLowerCase();
  arg.trim();
}

// True only if the whole string is a valid number literal (sign, digits, '.').
static bool isNumericToken(const String& s) {
  if (s.length() == 0) return false;
  bool ok = (s[0] == '-' || s[0] == '+' || isDigit(s[0]));
  for (size_t i = 1; i < s.length() && ok; i++) {
    const char c = s[i];
    if (!isDigit(c) && c != '.') ok = false;
  }
  return ok;
}

static void handleSerialCommand(String cmd) {
  cmd.trim();
  if (cmd.length() == 0) return;

  String name, arg;
  splitCommand(cmd, name, arg);

  if (name == "?" || name == "status") { printStatus(); return; }
  if (name == "help")                  { printHelp();   return; }
  if (name == "stop")                  { stopAll("user"); return; }

  if (name == "r") {
    ControlValve::coast();
    Serial.println("Valve coast (position counter unchanged).");
    return;
  }

  if (name == "auto") {
    if (arg.length() == 0) { Serial.println("Usage: auto <lpm>"); return; }
    startAuto(arg.toFloat());
    return;
  }
  if (name == "pump") {
    if (arg.length() == 0) { Serial.println("Usage: pump <0-100>"); return; }
    setPumpPercent(arg.toInt());
    Serial.printf("Pump set to %d%%\n", pumpPct);
    return;
  }
  if (name == "valve") {
    if (arg.length() == 0) { Serial.println("Usage: valve <0-100>"); return; }
    ControlValve::disableAuto();
    ControlValve::setPosition(arg.toInt());
    Serial.printf("Valve at %d%% (auto disabled)\n", ControlValve::getPosition());
    return;
  }

  // Bare numeric -> use as target flow.
  if (isNumericToken(cmd)) {
    const float n = cmd.toFloat();
    if (n == -100.0f) {                          // legacy force-close shortcut
      ControlValve::disableAuto();
      ControlValve::forceFullClose();
      Serial.println("Done. Valve closed and position reset to 0%.");
      return;
    }
    if (n <= 0.0f) { stopAll("target 0"); return; }
    startAuto(n);
    return;
  }

  Serial.println("Unknown command. Type 'help'.");
}

// ===========================================================================
//  Arduino entry points
// ===========================================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println(F("=== MFS + L298N proportional valve + pump ==="));

  // 1. Initialize subsystems.
  flowSensor.begin();
  ControlValve::begin(VALVE_IN1, VALVE_IN2, VALVE_ENA);
  pumpBegin();

  // 2. Bring up Wi-Fi + HTTP server. WebInterface only sees getters and
  //    callbacks, not the underlying objects.
  WebInterface::Handlers handlers = { webOnTarget, webOnPump, webOnStop };
  WebInterface::begin(WIFI_SSID, WIFI_PASSWORD, WIFI_TIMEOUT_MS,
                      buildSnapshot, handlers);

  // 3. Let the valve yield to the web server during long blocking moves
  //    (force-close is 4 s and would otherwise freeze the UI).
  ControlValve::setYieldCallback(WebInterface::handle);

  // 4. Known starting state: valve fully closed, pump off, idle.
  ControlValve::forceFullClose();
  Serial.println("Valve at 0%, pump off. Idle.");

  printHelp();
  Serial.print("Enter target flow (L/min) > ");
}

void loop() {
  // Service the HTTP server on every iteration so the page stays snappy.
  WebInterface::handle();

  // Handle one serial line if one is waiting.
  if (Serial.available()) {
    handleSerialCommand(Serial.readStringUntil('\n'));
  }

  const unsigned long now = millis();

  // --- sensor refresh ----------------------------------------------------
  // One ADC sweep gives us mA; the cached L/min comes from the same sample.
  static unsigned long lastSense = 0;
  if (now - lastSense >= SENSE_INTERVAL_MS) {
    lastSense = now;
    cachedCurrent_mA = flowSensor.readCurrent();
    cachedConnected  = (cachedCurrent_mA >= 3.5f);
    cachedFlow_lpm   = cachedConnected
                         ? flowSensor.convertToFlow(cachedCurrent_mA)
                         : 0.0f;
  }

  // --- control tick ------------------------------------------------------
  static unsigned long lastCtrl = 0;
  if (now - lastCtrl >= CONTROL_INTERVAL_MS) {
    lastCtrl = now;
    ControlValve::controlTick(cachedFlow_lpm, cachedConnected);
  }

  // --- periodic serial status -------------------------------------------
  static unsigned long lastPrint = 0;
  if (now - lastPrint >= PRINT_INTERVAL_MS) {
    lastPrint = now;
    if (!cachedConnected) Serial.println("Sensor disconnected");
    else                  printStatus();
  }
}
