#pragma once

#include <Arduino.h>

// ===========================================================================
//  WebInterface
//
//  Wi-Fi (STA with AP fallback) + HTTP server + single-page web UI for
//  the flow controller.
//
//  The web layer never touches the sensor, valve or pump directly.
//  main.cpp passes:
//    * a SnapshotProvider - called to build the JSON state for /data
//    * a Handlers struct  - callbacks for the three POST endpoints
//
//  Endpoints:
//    GET  /         -> the HTML page (served from PROGMEM)
//    GET  /data     -> JSON snapshot for the UI to poll
//    POST /target   -> "target=<lpm>"  starts/updates the closed loop
//    POST /pump     -> "pct=<0-100>"   sets pump duty directly
//    POST /stop     -> stops everything
// ===========================================================================
namespace WebInterface {

// Snapshot of the system state, built each time /data is requested.
struct Snapshot {
  float       flow_lpm;     // current flow (cached, L/min)
  float       current_mA;   // raw loop current (cached, mA)
  int         valve_pct;    // 0-100
  float       target_lpm;   // closed-loop setpoint
  int         pump_pct;     // 0-100
  bool        autoMode;     // true if closed loop is running
  bool        connected;    // true if the sensor loop looks alive
  bool        locked;       // true after 5 in-band confirms
  const char* speed;        // "idle"/"fast"/"medium"/"slow"/"halt"
};

// Callbacks invoked by the HTTP POST handlers.
struct Handlers {
  void (*onTarget)(float lpm);   // user pressed "Apply target"
  void (*onPump)(int pct);       // user pressed "Set pump"
  void (*onStop)();              // user pressed "STOP"
};

typedef Snapshot (*SnapshotProvider)();

// Connect to Wi-Fi (STA), or after `connectTimeoutMs` fall back to an
// open AP called "ESP32-FlowCtrl". Then register HTTP routes and start
// the server on port 80.
void begin(const char* ssid, const char* password,
           unsigned long connectTimeoutMs,
           SnapshotProvider provider, Handlers handlers);

// Call frequently from loop() to service HTTP clients. Also suitable
// as a YieldFn callback for ControlValve::setYieldCallback().
void handle();

}  // namespace WebInterface
