#include "WebInterface.h"

#include <WiFi.h>
#include <WebServer.h>

namespace WebInterface {

// ---------------------------------------------------------------------------
//  Module state
// ---------------------------------------------------------------------------
static WebServer        server(80);
static SnapshotProvider snapshotProvider = nullptr;
static Handlers         handlers         = { nullptr, nullptr, nullptr };

// ---------------------------------------------------------------------------
//  HTML page (stored in flash via PROGMEM)
//
//  Auto-polls /data once a second, displays the live state, and POSTs
//  back to /target /pump /stop. No external CSS/JS, no CDN.
// ---------------------------------------------------------------------------
static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8" />
<meta name="viewport" content="width=device-width,initial-scale=1" />
<title>ESP32 Flow Controller</title>
<style>
  :root{
    --bg:#0f1419;--panel:#1b2330;--text:#e8eef5;--muted:#8aa0b8;
    --accent:#4dc4ff;--ok:#4ade80;--warn:#fbbf24;--err:#f87171;--border:#2a3548;
  }
  *{box-sizing:border-box}
  body{margin:0 auto;max-width:980px;padding:1.2rem;color:var(--text);background:var(--bg);
       font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif}
  h1{font-weight:500;font-size:1.1rem;margin:0 0 1rem;color:var(--muted);
     letter-spacing:.08em;text-transform:uppercase}
  .grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:.9rem}
  .card{background:var(--panel);border:1px solid var(--border);border-radius:12px;padding:1rem 1.1rem}
  .label{color:var(--muted);font-size:.75rem;text-transform:uppercase;letter-spacing:.09em}
  .value{font-size:1.9rem;font-weight:600;margin-top:.25rem;line-height:1.1}
  .unit{color:var(--muted);font-size:.95rem;font-weight:400;margin-left:.25rem}
  .controls{margin-top:1.5rem;background:var(--panel);border:1px solid var(--border);
            border-radius:12px;padding:1rem 1.1rem;display:flex;flex-wrap:wrap;gap:.9rem;align-items:end}
  .field{display:flex;flex-direction:column;gap:.3rem}
  input[type=number]{width:8rem;padding:.55rem .7rem;font-size:1rem;background:#0f1620;
                     color:var(--text);border:1px solid var(--border);border-radius:8px;outline:none}
  input[type=number]:focus{border-color:var(--accent)}
  button{padding:.6rem 1.05rem;font-size:.95rem;font-weight:500;background:var(--accent);
         color:#0a1118;border:none;border-radius:8px;cursor:pointer}
  button.secondary{background:transparent;color:var(--text);border:1px solid var(--border)}
  button.danger{background:var(--err);color:#0a1118}
  .status{margin-top:1rem;padding:.55rem .85rem;border-radius:8px;font-size:.9rem;
          background:var(--panel);color:var(--muted);border:1px solid var(--border)}
  .status.ok{color:var(--ok);border-color:rgba(74,222,128,.35)}
  .status.off{color:var(--warn);border-color:rgba(251,191,36,.35)}
  .status.err{color:var(--err);border-color:rgba(248,113,113,.35)}
</style>
</head>
<body>
  <h1>ESP32 Flow Controller</h1>
  <div class="grid">
    <div class="card"><div class="label">Flow</div>        <div class="value"><span id="flow">--</span><span class="unit">L/min</span></div></div>
    <div class="card"><div class="label">Loop current</div><div class="value"><span id="mA">--</span><span class="unit">mA</span></div></div>
    <div class="card"><div class="label">Valve</div>       <div class="value"><span id="valve">--</span><span class="unit">%</span></div></div>
    <div class="card"><div class="label">Target</div>      <div class="value"><span id="target">--</span><span class="unit">L/min</span></div></div>
    <div class="card"><div class="label">Pump</div>        <div class="value"><span id="pump">--</span><span class="unit">%</span></div></div>
  </div>

  <div class="controls">
    <div class="field">
      <span class="label">Target flow (L/min)</span>
      <input id="targetIn" type="number" min="0" step="1" value="0" />
    </div>
    <button onclick="apply()">Apply target</button>

    <div class="field">
      <span class="label">Pump duty (%)</span>
      <input id="pumpIn" type="number" min="0" max="100" step="5" value="50" />
    </div>
    <button class="secondary" onclick="setPump()">Set pump</button>

    <button class="danger" onclick="stopAll()">STOP</button>
  </div>

  <div id="status" class="status">--</div>

<script>
const $ = id => document.getElementById(id);

async function poll(){
  try{
    const d = await (await fetch('/data',{cache:'no-store'})).json();
    $('flow').textContent   = d.flow.toFixed(1);
    $('mA').textContent     = d.mA.toFixed(2);
    $('valve').textContent  = d.valve;
    $('target').textContent = d.target.toFixed(1);
    $('pump').textContent   = d.pump;
    const s = $('status');
    if(!d.connected){ s.textContent = 'Flow sensor: disconnected'; s.className = 'status off'; return; }
    if(!d.auto){      s.textContent = 'Idle';                       s.className = 'status';     return; }
    const err  = d.flow - d.target;
    const sign = err >= 0 ? '+' : '-';
    const tag  = sign + Math.abs(err).toFixed(1) + ' L/min';
    if      (d.locked)            { s.textContent = 'Locked  err ' + tag;                     s.className = 'status ok'; }
    else if (d.speed === 'halt')  { s.textContent = 'Halted (settling)  err ' + tag;          s.className = 'status';    }
    else                          { s.textContent = 'Tracking (' + d.speed + ')  err ' + tag; s.className = 'status';    }
  }catch(e){
    const s = $('status');
    s.textContent = 'Lost connection to ESP32';
    s.className   = 'status err';
  }
}

async function post(url, body){
  await fetch(url, {method:'POST',
                    headers:{'Content-Type':'application/x-www-form-urlencoded'},
                    body: body || ''});
  poll();
}
const apply   = () => post('/target', 'target=' + encodeURIComponent($('targetIn').value));
const setPump = () => post('/pump',   'pct='    + encodeURIComponent($('pumpIn').value));
const stopAll = () => post('/stop');

setInterval(poll, 1000);
poll();
</script>
</body>
</html>
)HTML";

// ---------------------------------------------------------------------------
//  Route handlers
// ---------------------------------------------------------------------------
static void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

static void handleData() {
  if (!snapshotProvider) { server.send(500, "text/plain", "no provider"); return; }
  Snapshot s = snapshotProvider();

  char buf[256];
  snprintf(buf, sizeof(buf),
           "{\"flow\":%.2f,\"mA\":%.3f,\"valve\":%d,\"target\":%.2f,"
            "\"pump\":%d,\"auto\":%s,\"connected\":%s,\"locked\":%s,"
            "\"speed\":\"%s\"}",
           s.flow_lpm, s.current_mA, s.valve_pct, s.target_lpm,
           s.pump_pct,
           s.autoMode  ? "true" : "false",
           s.connected ? "true" : "false",
           s.locked    ? "true" : "false",
           s.speed);
  server.send(200, "application/json", buf);
}

static void handleTarget() {
  if (!server.hasArg("target")) { server.send(400, "text/plain", "missing target"); return; }
  if (handlers.onTarget) handlers.onTarget(server.arg("target").toFloat());
  server.send(200, "text/plain", "ok");
}

static void handlePump() {
  if (!server.hasArg("pct")) { server.send(400, "text/plain", "missing pct"); return; }
  if (handlers.onPump) handlers.onPump(server.arg("pct").toInt());
  server.send(200, "text/plain", "ok");
}

static void handleStop() {
  if (handlers.onStop) handlers.onStop();
  server.send(200, "text/plain", "ok");
}

static void handleNotFound() {
  server.send(404, "text/plain", "not found");
}

// ---------------------------------------------------------------------------
//  Wi-Fi bring-up: STA first, AP fallback after timeout
// ---------------------------------------------------------------------------
static void connectWiFi(const char* ssid, const char* password,
                        unsigned long timeoutMs) {
  Serial.printf("[wifi] connecting to SSID \"%s\"...\n", ssid);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(ssid, password);

  const unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[wifi] connected. Open: http://");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("[wifi] STA failed, falling back to AP \"ESP32-FlowCtrl\"");
    WiFi.mode(WIFI_AP);
    WiFi.softAP("ESP32-FlowCtrl");
    Serial.print("[wifi] AP started. Open: http://");
    Serial.println(WiFi.softAPIP());
  }
}

// ---------------------------------------------------------------------------
//  Public API
// ---------------------------------------------------------------------------
void begin(const char* ssid, const char* password,
           unsigned long connectTimeoutMs,
           SnapshotProvider provider, Handlers h) {
  snapshotProvider = provider;
  handlers         = h;

  connectWiFi(ssid, password, connectTimeoutMs);

  server.on("/",       HTTP_GET,  handleRoot);
  server.on("/data",   HTTP_GET,  handleData);
  server.on("/target", HTTP_POST, handleTarget);
  server.on("/pump",   HTTP_POST, handlePump);
  server.on("/stop",   HTTP_POST, handleStop);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("[web] HTTP server started on port 80");
}

void handle() { server.handleClient(); }

}  // namespace WebInterface
