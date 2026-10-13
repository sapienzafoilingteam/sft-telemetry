#if __has_include("secrets.h")
#include "secrets.h"
#else
#define SFT_WIFI_PASSWORD "change-me-now"
#endif

/**
 * ============================================================================
 * SFT TELEMETRY — SD DEBUG via WiFi (USB staccata)
 * ============================================================================
 * Versione speciale per testare la SD con USB completamente staccata.
 * Tutti i log Serial vengono anche inviati via UDP broadcast e via
 * WebSocket HTTP, visibili nel browser connesso all'hotspot.
 *
 * Come usare:
 *   1. Carica questo sketch con USB collegata
 *   2. STACCA il cavo USB
 *   3. Alimenta l'ESP32 da batteria / LiPo / powerbank
 *   4. Connetti il PC al WiFi "SFT_telemetry"
 *   5. Apri http://192.168.4.1 nel browser
 *   6. Osserva i log in tempo reale nella pagina web
 *
 * Pin SD forzati a 11/12/13 come richiesto (originali PCB ONDA v2.5)
 * ============================================================================
 */

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <Wire.h>
#include <LIS3MDL.h>
#include <LSM6.h>
#include <MadgwickAHRS.h>
#include <TinyGPS++.h>

/* =========================================================================
 * NETWORK
 * ========================================================================= */
const char *ssid      = "SFT_telemetry";
const char *password  = SFT_WIFI_PASSWORD;
const char *targetIP  = "255.255.255.255";
const int   targetPort = 4210;

WiFiUDP   udp;
WebServer server(80);

/* =========================================================================
 * PIN — SD originali PCB ONDA v2.5 (forzati come richiesto)
 * ========================================================================= */
#define I2C_SDA  8
#define I2C_SCL  9
#define LED_1   21
#define LED_2   45
#define BTN_WIFI 5
#define LED_WIFI 4
#define BTN_REC  7
#define LED_REC  6
#define BTN_RST 18
#define LED_RST 17
#define GPS_RX   2
#define GPS_TX   1

// Pin SD ORIGINALI (forzati — test senza USB per verificare interferenze)
#define SD_CS   10
#define SD_MOSI 11
#define SD_SCK  12
#define SD_MISO 13

/* =========================================================================
 * COSTANTI
 * ========================================================================= */
const int SAMPLE_RATE_HZ         = 50;
const unsigned long SAMPLE_PERIOD_MICROS = 1000000 / SAMPLE_RATE_HZ;
const float GYRO_SENSITIVITY     = 0.00875f;
const float ACCEL_SENSITIVITY    = 0.000061f;
const float MAG_SENSITIVITY      = 0.000146f;
const int   SD_BUFFER_LINES      = 50;

/* =========================================================================
 * LOG BUFFER — Circolare, inviato via WebSocket/UDP
 * Raccoglie tutti i messaggi di log che normalmente andrebbero su Serial,
 * così sono visibili nel browser anche senza USB.
 * ========================================================================= */
#define LOG_BUFFER_SIZE 80      // Numero di righe mantenute in memoria
#define LOG_LINE_LEN    160     // Caratteri max per riga

char     logBuffer[LOG_BUFFER_SIZE][LOG_LINE_LEN];
int      logHead  = 0;          // Prossima posizione di scrittura (circolare)
int      logCount = 0;          // Righe totali scritte (max LOG_BUFFER_SIZE)
bool     logDirty = false;      // Nuove righe da inviare al browser

/**
 * Scrive un messaggio nel log circolare, su Serial (se USB collegata),
 * e via UDP broadcast. Il browser lo legge via polling HTTP.
 */
void LOG(const char* msg) {
  // Serial (funziona solo se USB collegata, ma non fa male)
  Serial.println(msg);

  // Buffer circolare per il web viewer
  strncpy(logBuffer[logHead], msg, LOG_LINE_LEN - 1);
  logBuffer[logHead][LOG_LINE_LEN - 1] = '\0';
  logHead = (logHead + 1) % LOG_BUFFER_SIZE;
  if (logCount < LOG_BUFFER_SIZE) logCount++;
  logDirty = true;

  // UDP broadcast (per chi usa il viewer Python originale)
  udp.beginPacket(targetIP, targetPort);
  udp.printf("LOG:%s", msg);
  udp.endPacket();
}

void LOGf(const char* fmt, ...) {
  char buf[LOG_LINE_LEN];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  LOG(buf);
}

/* =========================================================================
 * GLOBAL STATE
 * ========================================================================= */
LSM6        imu;
LIS3MDL     mag;
Madgwick    filter;
TinyGPSPlus gps;
SPIClass    sdSPI(FSPI);

unsigned long lastMicros      = 0;
unsigned long lastGPSUpdate   = 0;
float gx_bias = 0, gy_bias = 0, gz_bias = 0;
bool  imu_available = false;
bool  mag_available = false;
char  udpBuffer[256];

bool   sd_available      = false;
bool   sd_recording      = false;
File   sd_file;
int    sd_session_number = 0;
char   sd_filename[32];
String sd_write_buffer   = "";
int    sd_buffer_count   = 0;
unsigned long sd_last_flush = 0;

double cached_lat = 0, cached_lng = 0;
double cached_alt = 0, cached_speed = 0;
int    cached_sats = 0;

char udpRxBuffer[64];
bool wifi_enabled = true;

// Risultato test SD (per mostrarlo nella pagina web)
struct SDTestResult {
  bool miso_pullup;        // MISO va HIGH con pull-up
  bool miso_with_usb;      // Stato MISO osservato (indice interferenza USB)
  bool cs_toggle;
  bool sck_toggle;
  bool mosi_toggle;
  bool raw_cmd0;           // CMD0 raw risponde
  uint8_t raw_r1;          // Risposta R1 grezza
  bool init_ok;
  uint32_t init_freq;
  bool write_ok;
  bool read_ok;
  bool verify_ok;
  char diagnosis[256];
} sdTest;

/* =========================================================================
 * BUTTONS
 * ========================================================================= */
struct ButtonState {
  uint8_t pin;
  bool lastState, currentState;
  unsigned long lastDebounceTime;
  ButtonState(uint8_t p) : pin(p), lastState(LOW), currentState(LOW), lastDebounceTime(0) {}
};
ButtonState btnWifi(BTN_WIFI), btnRec(BTN_REC), btnRst(BTN_RST);
const unsigned long debounceDelay = 20;

bool readButton(ButtonState &btn) {
  int reading = digitalRead(btn.pin);
  if (reading != btn.lastState) btn.lastDebounceTime = millis();
  bool pressed = false;
  if ((millis() - btn.lastDebounceTime) > debounceDelay) {
    if (reading != btn.currentState) {
      btn.currentState = reading;
      if (btn.currentState == HIGH) pressed = true;
    }
  }
  btn.lastState = reading;
  return pressed;
}

void sendUDP(const char *msg) {
  if (!wifi_enabled) return;
  udp.beginPacket(targetIP, targetPort);
  udp.print(msg);
  udp.endPacket();
}

/* =========================================================================
 * WEB SERVER — Pagina di debug
 * ========================================================================= */

// Pagina HTML con auto-refresh ogni 2s e colori per PASS/FAIL/WARN
const char HTML_PAGE[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="it">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta http-equiv="refresh" content="3">
<title>SFT — SD Debug WiFi</title>
<style>
  body { background:#0d1117; color:#c9d1d9; font-family:monospace;
         font-size:13px; margin:0; padding:10px; }
  h1   { color:#58a6ff; margin:0 0 6px; font-size:18px; }
  .sub { color:#8b949e; font-size:11px; margin-bottom:12px; }
  .card { background:#161b22; border:1px solid #30363d; border-radius:6px;
          padding:10px; margin-bottom:10px; }
  .card h2 { color:#58a6ff; margin:0 0 8px; font-size:13px; text-transform:uppercase; }
  table { width:100%; border-collapse:collapse; }
  td    { padding:3px 6px; border-bottom:1px solid #21262d; }
  td:first-child { color:#8b949e; width:55%; }
  .pass { color:#3fb950; font-weight:bold; }
  .fail { color:#f85149; font-weight:bold; }
  .warn { color:#d29922; font-weight:bold; }
  .info { color:#58a6ff; }
  .log  { background:#0d1117; padding:8px; border-radius:4px;
          height:260px; overflow-y:auto; white-space:pre-wrap;
          word-break:break-all; font-size:12px; }
  .log .L { color:#c9d1d9; }
  .log .P { color:#3fb950; }
  .log .F { color:#f85149; }
  .log .W { color:#d29922; }
  .btn { background:#21262d; border:1px solid #30363d; color:#c9d1d9;
         padding:6px 14px; border-radius:6px; cursor:pointer;
         font-family:monospace; font-size:12px; margin:3px; }
  .btn:hover { background:#30363d; }
  .status-bar { display:flex; gap:10px; flex-wrap:wrap; margin-bottom:10px; }
  .badge { padding:3px 10px; border-radius:20px; font-size:11px; font-weight:bold; }
  .badge.ok  { background:#238636; color:#fff; }
  .badge.err { background:#da3633; color:#fff; }
  .badge.na  { background:#30363d; color:#8b949e; }
</style>
</head>
<body>
<h1>&#x1F4E1; SFT — SD Card Debug (WiFi, USB scollegata)</h1>
<div class="sub">Auto-refresh ogni 3s &mdash; IP: 192.168.4.1 &mdash; Pin SD: CS=10 MOSI=11 SCK=12 MISO=13</div>

<div class="status-bar" id="badges">
  <span class="badge" id="b-sd">SD ...</span>
  <span class="badge" id="b-imu">IMU ...</span>
  <span class="badge" id="b-mag">MAG ...</span>
  <span class="badge" id="b-gps">GPS ...</span>
  <span class="badge" id="b-rec">REC ...</span>
</div>

<div class="card">
  <h2>&#x1F50D; Risultati Test SD</h2>
  <table id="tbl"></table>
</div>

<div class="card">
  <h2>&#x1F4CB; Log Sistema</h2>
  <div class="log" id="log"></div>
</div>

<div class="card">
  <h2>&#x1F4BE; Controllo SD</h2>
  <button class="btn" onclick="cmd('START_RECORDING')">&#x23FA; Inizia Registrazione</button>
  <button class="btn" onclick="cmd('STOP_RECORDING')">&#x23F9; Stop Registrazione</button>
  <button class="btn" onclick="cmd('LS')">&#x1F4C1; Lista File</button>
  <button class="btn" onclick="cmd('INIT_SD')">&#x1F504; Re-init SD</button>
</div>

<script>
async function fetchStatus() {
  try {
    const r = await fetch('/status');
    const d = await r.json();

    // Badges
    const setB = (id, ok, txt) => {
      const el = document.getElementById(id);
      el.textContent = txt;
      el.className = 'badge ' + (ok === true ? 'ok' : ok === false ? 'err' : 'na');
    };
    setB('b-sd',  d.sd_ok,      d.sd_ok  ? 'SD OK'  : 'SD FAIL');
    setB('b-imu', d.imu_ok,     d.imu_ok ? 'IMU OK' : 'IMU FAIL');
    setB('b-mag', d.mag_ok,     d.mag_ok ? 'MAG OK' : 'MAG FAIL');
    setB('b-gps', d.gps_fix,    d.gps_fix? 'GPS FIX': 'GPS NO FIX');
    setB('b-rec', d.recording,  d.recording ? 'REC ●' : 'REC ○');

    // Test table
    const rows = d.tests || [];
    let html = '';
    for (const row of rows) {
      const cls = row.status === 'PASS' ? 'pass' : row.status === 'FAIL' ? 'fail' : row.status === 'WARN' ? 'warn' : 'info';
      html += `<tr><td>${row.name}</td><td class="${cls}">${row.status} ${row.value || ''}</td></tr>`;
    }
    document.getElementById('tbl').innerHTML = html;

    // Log
    const lines = d.log || [];
    let logHtml = '';
    for (const l of lines) {
      let cls = 'L';
      if (l.includes('[PASS]') || l.includes('_OK') || l.includes('READY')) cls = 'P';
      else if (l.includes('[FAIL]') || l.includes('ERROR') || l.includes('FAIL')) cls = 'F';
      else if (l.includes('[WARN]') || l.includes('WARN')) cls = 'W';
      logHtml += `<span class="${cls}">${escHtml(l)}\n</span>`;
    }
    const logEl = document.getElementById('log');
    logEl.innerHTML = logHtml;
    logEl.scrollTop = logEl.scrollHeight;

  } catch(e) { console.warn('fetch error', e); }
}

function escHtml(s) {
  return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');
}

async function cmd(c) {
  try { await fetch('/cmd?c=' + c); } catch(e) {}
}

fetchStatus();
setInterval(fetchStatus, 3000);
</script>
</body>
</html>
)rawhtml";

// Costruisce il JSON di stato per il browser
void handleStatus() {
  // Stato sistema
  String json = "{";
  json += "\"sd_ok\":"      + String(sd_available ? "true" : "false") + ",";
  json += "\"imu_ok\":"     + String(imu_available ? "true" : "false") + ",";
  json += "\"mag_ok\":"     + String(mag_available ? "true" : "false") + ",";
  json += "\"gps_fix\":"    + String((cached_lat != 0 || cached_lng != 0) ? "true" : "false") + ",";
  json += "\"recording\":"  + String(sd_recording ? "true" : "false") + ",";

  // Test results table
  json += "\"tests\":[";
  auto row = [&](const char* name, const char* status, const char* value) {
    json += "{\"name\":\"" + String(name) + "\",\"status\":\"" + String(status) +
            "\",\"value\":\"" + String(value) + "\"},";
  };

  char valBuf[64];

  row("Pin MISO=13 pull-up HIGH",
      sdTest.miso_pullup ? "PASS" : "FAIL",
      sdTest.miso_pullup ? "OK — nessun cortocircuito" : "LOW — conflitto bus!");

  row("MISO stato rilevato (raw)",
      "INFO",
      sdTest.miso_with_usb ? "HIGH (1)" : "LOW (0)");

  row("CS=10 toggle",    sdTest.cs_toggle  ? "PASS" : "FAIL", "");
  row("SCK=12 toggle",   sdTest.sck_toggle ? "PASS" : "FAIL", "");
  row("MOSI=11 toggle",  sdTest.mosi_toggle? "PASS" : "FAIL", "");

  snprintf(valBuf, sizeof(valBuf), "R1=0x%02X", sdTest.raw_r1);
  row("CMD0 raw (bitbang)",
      sdTest.raw_cmd0 ? "PASS" : "FAIL",
      valBuf);

  if (sdTest.init_ok) {
    snprintf(valBuf, sizeof(valBuf), "@ %lu kHz", sdTest.init_freq / 1000);
    row("SD.begin()", "PASS", valBuf);
  } else {
    row("SD.begin()", "FAIL", "nessuna frequenza funziona");
  }

  row("Scrittura file", sdTest.write_ok  ? "PASS" : (sdTest.init_ok ? "FAIL" : "N/A"), "");
  row("Lettura file",   sdTest.read_ok   ? "PASS" : (sdTest.init_ok ? "FAIL" : "N/A"), "");
  row("Verifica dati",  sdTest.verify_ok ? "PASS" : (sdTest.init_ok ? "FAIL" : "N/A"), "");
  row("Diagnosi", "INFO", sdTest.diagnosis);

  // Rimuovi ultima virgola
  if (json.endsWith(",")) json.remove(json.length() - 1);
  json += "],";

  // Log circolare (ultime LOG_BUFFER_SIZE righe in ordine cronologico)
  json += "\"log\":[";
  int start = (logCount >= LOG_BUFFER_SIZE) ? logHead : 0;
  int count = min(logCount, LOG_BUFFER_SIZE);
  for (int i = 0; i < count; i++) {
    int idx = (start + i) % LOG_BUFFER_SIZE;
    String line = logBuffer[idx];
    line.replace("\\", "\\\\");
    line.replace("\"", "\\\"");
    json += "\"" + line + "\"";
    if (i < count - 1) json += ",";
  }
  json += "]}";

  server.send(200, "application/json", json);
}

void handleRoot() {
  server.send_P(200, "text/html", HTML_PAGE);
}

void handleCmd() {
  if (!server.hasArg("c")) { server.send(400, "text/plain", "Missing c"); return; }
  String cmd = server.arg("c");
  server.send(200, "text/plain", "OK");

  if      (cmd == "START_RECORDING" && !sd_recording) startSDRecording();
  else if (cmd == "STOP_RECORDING"  &&  sd_recording) stopSDRecording();
  else if (cmd == "LS")     listSDFiles();
  else if (cmd == "INIT_SD") { sd_available = runFullSDTest(); }
}

/* =========================================================================
 * SD FUNCTIONS
 * ========================================================================= */
int findNextSessionNumber() {
  int maxFound = 0;
  File root = SD.open("/");
  if (!root) return 1;
  File entry;
  while ((entry = root.openNextFile())) {
    String name = entry.name();
    if (name.startsWith("session_") && name.endsWith(".csv")) {
      int num = name.substring(8, name.length() - 4).toInt();
      if (num > maxFound) maxFound = num;
    }
    entry.close();
  }
  root.close();
  return maxFound + 1;
}

bool startSDRecording() {
  if (!sd_available) { LOG("SD_REC_ERROR:NO_SD"); return false; }
  snprintf(sd_filename, sizeof(sd_filename), "/session_%04d.csv", sd_session_number);
  sd_file = SD.open(sd_filename, FILE_WRITE);
  if (!sd_file) { LOGf("SD_REC_ERROR:CANT_OPEN:%s", sd_filename); return false; }
  sd_file.println("timestamp_ms,roll,pitch,yaw,ax,ay,az,gx,gy,gz,mx,my,mz,"
                   "gps_lat,gps_lng,gps_alt,gps_speed,gps_sats");
  sd_file.flush();
  sd_recording = true; sd_buffer_count = 0;
  sd_write_buffer = ""; sd_write_buffer.reserve(4096);
  sd_last_flush = millis();
  LOGf("SD_REC_STARTED:%s", sd_filename + 1);
  snprintf(udpBuffer, sizeof(udpBuffer), "STATUS:SD_REC_STARTED:%s", sd_filename + 1);
  sendUDP(udpBuffer);
  return true;
}

void stopSDRecording() {
  if (!sd_recording) return;
  if (sd_write_buffer.length() > 0) { sd_file.print(sd_write_buffer); }
  sd_file.flush(); sd_file.close();
  sd_recording = false; sd_write_buffer = ""; sd_buffer_count = 0;
  LOGf("SD_REC_STOPPED:%s", sd_filename + 1);
  snprintf(udpBuffer, sizeof(udpBuffer), "STATUS:SD_REC_STOPPED:%s", sd_filename + 1);
  sendUDP(udpBuffer);
  sd_session_number++;
}

void writeSDData(float r, float p, float y,
                 float ax, float ay, float az,
                 float gx, float gy, float gz,
                 float mx, float my, float mz) {
  if (!sd_recording) return;
  char line[256];
  snprintf(line, sizeof(line),
    "%lu,%.2f,%.2f,%.2f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,"
    "%.6f,%.6f,%.1f,%.1f,%d\n",
    millis(), r, p, y, ax, ay, az, gx, gy, gz, mx, my, mz,
    cached_lat, cached_lng, cached_alt, cached_speed, cached_sats);
  sd_write_buffer += line;
  sd_buffer_count++;
  if (sd_buffer_count >= SD_BUFFER_LINES || (millis() - sd_last_flush) >= 1000) {
    sd_file.print(sd_write_buffer); sd_file.flush();
    sd_write_buffer = ""; sd_buffer_count = 0; sd_last_flush = millis();
  }
}

void listSDFiles() {
  if (!sd_available) return;
  File root = SD.open("/");
  if (!root) return;
  LOG("SD_LIST_START");
  File entry;
  while ((entry = root.openNextFile())) {
    if (!entry.isDirectory()) LOGf("FILE:%s|%llu", entry.name(), entry.size());
    entry.close();
  }
  root.close();
  LOG("SD_LIST_END");
}

/* =========================================================================
 * TEST SD COMPLETO — eseguito all'avvio e su richiesta
 * Restituisce true se la SD è pronta all'uso.
 * ========================================================================= */
bool runFullSDTest() {
  memset(&sdTest, 0, sizeof(sdTest));
  strcpy(sdTest.diagnosis, "In corso...");

  LOG("=== TEST SD AVVIATO ===");
  LOGf("Pin: CS=%d MOSI=%d SCK=%d MISO=%d", SD_CS, SD_MOSI, SD_SCK, SD_MISO);

  // ------------------------------------------------------------------
  // STEP 1 — Test pin logici
  // ------------------------------------------------------------------
  LOG("--- STEP 1: Test pin logici ---");

  // MISO pull-up
  pinMode(SD_MISO, INPUT_PULLUP);
  delay(10);
  int miso_val = digitalRead(SD_MISO);
  sdTest.miso_with_usb = (miso_val == HIGH);
  sdTest.miso_pullup   = (miso_val == HIGH);
  LOGf("[%s] MISO pull-up: letto %d (atteso 1)",
       sdTest.miso_pullup ? "PASS" : "FAIL", miso_val);

  if (!sdTest.miso_pullup) {
    LOG("[WARN] MISO=LOW => conflitto bus ancora presente!");
    LOG("[WARN] USB probabilmente ancora collegata o pin sbagliato.");
  } else {
    LOG("[PASS] MISO=HIGH => nessun conflitto, USB staccata ha effetto!");
  }

  // CS toggle
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH); delay(1);
  int h = digitalRead(SD_CS);
  digitalWrite(SD_CS, LOW);  delay(1);
  int l = digitalRead(SD_CS);
  digitalWrite(SD_CS, HIGH);
  sdTest.cs_toggle = (h == HIGH && l == LOW);
  LOGf("[%s] CS toggle H=%d L=%d", sdTest.cs_toggle ? "PASS" : "FAIL", h, l);

  // SCK toggle
  pinMode(SD_SCK, OUTPUT);
  digitalWrite(SD_SCK, LOW); delay(1); int sl = digitalRead(SD_SCK);
  digitalWrite(SD_SCK, HIGH);delay(1); int sh = digitalRead(SD_SCK);
  digitalWrite(SD_SCK, LOW);
  sdTest.sck_toggle = (sl == LOW && sh == HIGH);
  LOGf("[%s] SCK toggle L=%d H=%d", sdTest.sck_toggle ? "PASS" : "FAIL", sl, sh);

  // MOSI toggle
  pinMode(SD_MOSI, OUTPUT);
  digitalWrite(SD_MOSI, HIGH); delay(1); int mh = digitalRead(SD_MOSI);
  digitalWrite(SD_MOSI, LOW);  delay(1); int ml = digitalRead(SD_MOSI);
  sdTest.mosi_toggle = (mh == HIGH && ml == LOW);
  LOGf("[%s] MOSI toggle H=%d L=%d", sdTest.mosi_toggle ? "PASS" : "FAIL", mh, ml);

  // ------------------------------------------------------------------
  // STEP 2 — CMD0 raw bitbang
  // ------------------------------------------------------------------
  LOG("--- STEP 2: CMD0 raw (bitbang) ---");

  // Reset pin
  pinMode(SD_CS,   OUTPUT); digitalWrite(SD_CS,   HIGH);
  pinMode(SD_MOSI, OUTPUT); digitalWrite(SD_MOSI, HIGH);
  pinMode(SD_SCK,  OUTPUT); digitalWrite(SD_SCK,  LOW);
  pinMode(SD_MISO, INPUT_PULLUP);
  delay(10);

  // 80 cicli power-up con CS=HIGH
  for (int i = 0; i < 80; i++) {
    digitalWrite(SD_SCK, HIGH); delayMicroseconds(5);
    digitalWrite(SD_SCK, LOW);  delayMicroseconds(5);
  }
  LOG("80 cicli power-up inviati");

  // Funzione SPI bitbang locale
  auto bbByte = [&](uint8_t data) -> uint8_t {
    uint8_t rx = 0;
    for (int b = 7; b >= 0; b--) {
      digitalWrite(SD_MOSI, (data >> b) & 1);
      digitalWrite(SD_SCK, HIGH); delayMicroseconds(2);
      rx = (rx << 1) | digitalRead(SD_MISO);
      digitalWrite(SD_SCK, LOW);  delayMicroseconds(2);
    }
    return rx;
  };

  // Invia CMD0
  uint8_t cmd0[] = {0x40, 0x00, 0x00, 0x00, 0x00, 0x95};
  digitalWrite(SD_CS, LOW); delayMicroseconds(10);
  for (int i = 0; i < 6; i++) bbByte(cmd0[i]);

  // Leggi risposta R1
  uint8_t r1 = 0xFF;
  for (int i = 0; i < 8; i++) {
    r1 = bbByte(0xFF);
    if (r1 != 0xFF) break;
    delayMicroseconds(10);
  }
  digitalWrite(SD_CS, HIGH);
  bbByte(0xFF); // Extra clock

  sdTest.raw_r1   = r1;
  sdTest.raw_cmd0 = (r1 == 0x01 || r1 == 0x00);
  LOGf("[%s] CMD0 R1=0x%02X (%s)",
       sdTest.raw_cmd0 ? "PASS" : "FAIL", r1,
       r1 == 0x01 ? "IDLE OK" :
       r1 == 0x00 ? "già init" :
       r1 == 0xFF ? "nessuna risposta" : "risposta anomala");

  // ------------------------------------------------------------------
  // STEP 3 — SD.begin() sweep frequenze
  // ------------------------------------------------------------------
  LOG("--- STEP 3: SD.begin() sweep frequenze ---");

  const uint32_t freqs[] = {100000, 400000, 1000000, 4000000};
  const int nFreqs = 4;
  sdTest.init_ok = false;

  for (int f = 0; f < nFreqs; f++) {
    uint32_t freq = freqs[f];
    LOGf("Tentativo @ %lu kHz ...", freq / 1000);

    SD.end(); sdSPI.end(); delay(100);
    pinMode(SD_CS, OUTPUT); digitalWrite(SD_CS, HIGH); delay(10);
    sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS); delay(20);

    if (SD.begin(SD_CS, sdSPI, freq)) {
      uint8_t ct = SD.cardType();
      if (ct != CARD_NONE) {
        const char* ts = ct==CARD_MMC?"MMC":ct==CARD_SD?"SD":ct==CARD_SDHC?"SDHC":"UNK";
        uint64_t mb = SD.cardSize() / (1024*1024);
        LOGf("[PASS] SD OK! Tipo:%s %lluMB @ %lu kHz", ts, mb, freq/1000);
        sdTest.init_ok   = true;
        sdTest.init_freq = freq;
        break;
      } else {
        LOG("[FAIL] CARD_NONE dopo begin");
      }
    } else {
      LOG("[FAIL] SD.begin false");
    }
    delay(300);
  }

  if (!sdTest.init_ok) {
    LOG("[FAIL] Nessuna frequenza funziona");

    // Diagnosi basata su MISO
    if (!sdTest.miso_pullup) {
      strcpy(sdTest.diagnosis,
             "MISO ancora LOW — USB ancora collegata? Prova con USB staccata.");
    } else if (!sdTest.raw_cmd0) {
      strcpy(sdTest.diagnosis,
             "CMD0 no response — problema alimentazione. Aggiungi 100uF su 3.3V.");
    } else {
      strcpy(sdTest.diagnosis, "Init fallita nonostante CMD0 OK — scheda o libreria.");
    }
    return false;
  }

  // ------------------------------------------------------------------
  // STEP 4 — Read/Write test
  // ------------------------------------------------------------------
  LOG("--- STEP 4: Test lettura/scrittura ---");

  const char* TF      = "/diag_test.txt";
  const char* TC      = "SFT_DIAG_OK_1234567890";
  const int   TC_LEN  = strlen(TC);

  if (SD.exists(TF)) SD.remove(TF);
  File wf = SD.open(TF, FILE_WRITE);
  if (!wf) {
    LOG("[FAIL] Apertura scrittura fallita");
    sdTest.write_ok = false;
    strcpy(sdTest.diagnosis, "Scrittura fallita — scheda read-only o danneggiata.");
    return false;
  }
  size_t written = wf.print(TC);
  wf.flush(); wf.close();
  sdTest.write_ok = (written == (size_t)TC_LEN);
  LOGf("[%s] Scrittura: %d/%d bytes", sdTest.write_ok ? "PASS":"FAIL", written, TC_LEN);

  File rf = SD.open(TF, FILE_READ);
  if (!rf) {
    LOG("[FAIL] Apertura lettura fallita");
    sdTest.read_ok = false;
    return false;
  }
  char rb[64] = {0};
  int  nread  = rf.readBytes(rb, sizeof(rb)-1);
  rf.close();
  sdTest.read_ok = (nread > 0);
  LOGf("[%s] Lettura: %d bytes: \"%s\"", sdTest.read_ok ? "PASS":"FAIL", nread, rb);

  sdTest.verify_ok = (strcmp(rb, TC) == 0);
  LOGf("[%s] Verifica contenuto", sdTest.verify_ok ? "PASS":"FAIL");

  SD.remove(TF);

  // ------------------------------------------------------------------
  // Diagnosi finale
  // ------------------------------------------------------------------
  if (sdTest.verify_ok) {
    strcpy(sdTest.diagnosis, "SD completamente funzionante!");
    sd_session_number = findNextSessionNumber();
    LOGf("SD OK — prossima sessione: %04d", sd_session_number);
  } else {
    strcpy(sdTest.diagnosis, "Scrittura corrotta — possibile problema alimentazione.");
  }

  LOG("=== TEST SD COMPLETATO ===");
  return sdTest.init_ok && sdTest.verify_ok;
}

/* =========================================================================
 * SETUP
 * ========================================================================= */
void setup() {
  Serial.begin(115200);

  // LED e pulsanti
  pinMode(LED_1,    OUTPUT); digitalWrite(LED_1, HIGH);
  pinMode(LED_2,    OUTPUT); digitalWrite(LED_2, HIGH);
  pinMode(LED_WIFI, OUTPUT); digitalWrite(LED_WIFI, LOW);
  pinMode(LED_REC,  OUTPUT); digitalWrite(LED_REC,  LOW);
  pinMode(LED_RST,  OUTPUT); digitalWrite(LED_RST,  LOW);
  pinMode(BTN_WIFI, INPUT_PULLDOWN);
  pinMode(BTN_REC,  INPUT_PULLDOWN);
  pinMode(BTN_RST,  INPUT_PULLDOWN);

  // GPS
  Serial1.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);

  // WiFi hotspot — priorità massima, deve essere su prima di tutto
  LOG("Avvio WiFi hotspot...");
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid, password);
  delay(1500);
  digitalWrite(LED_WIFI, HIGH);
  LOGf("Hotspot attivo: SSID=%s IP=%s", ssid, WiFi.softAPIP().toString().c_str());
  LOGf("Apri browser su: http://%s", WiFi.softAPIP().toString().c_str());

  // UDP
  udp.begin(targetPort);

  // HTTP routes
  server.on("/",       handleRoot);
  server.on("/status", handleStatus);
  server.on("/cmd",    handleCmd);

  // Download file SD
  server.on("/download", HTTP_GET, []() {
    if (!sd_available) { server.send(503,"text/plain","SD N/A"); return; }
    if (!server.hasArg("file")) { server.send(400,"text/plain","Missing file"); return; }
    String fn = server.arg("file");
    if (!fn.startsWith("/")) fn = "/" + fn;
    if (!SD.exists(fn)) { server.send(404,"text/plain","Not found"); return; }
    File df = SD.open(fn, FILE_READ);
    if (!df) { server.send(500,"text/plain","Open failed"); return; }
    server.sendHeader("Content-Disposition","attachment; filename=\""+fn.substring(1)+"\"");
    server.streamFile(df, "application/octet-stream");
    df.close();
  });

  server.begin();
  LOG("HTTP server avviato su porta 80");

  // IMU
  Wire.begin(I2C_SDA, I2C_SCL, 400000);
  delay(300);
  if (!imu.init()) { LOG("ERROR: LSM6 non trovato"); imu_available = false; }
  else             { LOG("IMU OK (LSM6)"); imu.enableDefault(); imu_available = true; }

  if (!mag.init()) { LOG("WARN: LIS3MDL non trovato"); digitalWrite(LED_2, LOW); mag_available = false; }
  else             { LOG("MAG OK (LIS3MDL)"); mag.enableDefault(); digitalWrite(LED_2, HIGH); mag_available = true; }

  filter.begin(SAMPLE_RATE_HZ);

  // Calibrazione giroscopio
  LOG("Calibrazione giroscopio (3s)...");
  long cnt=0, sx=0, sy=0, sz=0;
  unsigned long tc = millis();
  while (millis()-tc < 3000) {
    if (imu_available) imu.read();
    if (imu_available && imu.last_status==0) { sx+=imu.g.x; sy+=imu.g.y; sz+=imu.g.z; cnt++; }
    delay(5);
  }
  if (cnt > 0) { gx_bias=sx/(float)cnt; gy_bias=sy/(float)cnt; gz_bias=sz/(float)cnt; }
  LOGf("Gyro bias: %.2f %.2f %.2f (%ld campioni)", gx_bias, gy_bias, gz_bias, cnt);

  // TEST SD COMPLETO
  LOG("Avvio test SD completo...");
  sd_available = runFullSDTest();

  LOG("=== SISTEMA PRONTO ===");
  LOGf("SD:%s IMU:%s MAG:%s",
       sd_available ? "OK" : "FAIL",
       imu_available ? "OK" : "FAIL",
       mag_available ? "OK" : "FAIL");

  lastMicros = micros();
}

/* =========================================================================
 * LOOP
 * ========================================================================= */
void loop() {

  // Pulsanti
  if (readButton(btnWifi)) {
    wifi_enabled = !wifi_enabled;
    if (wifi_enabled) { WiFi.mode(WIFI_AP); WiFi.softAP(ssid, password); digitalWrite(LED_WIFI, HIGH); LOG("WIFI ON"); }
    else              { WiFi.softAPdisconnect(true); WiFi.mode(WIFI_OFF); digitalWrite(LED_WIFI, LOW); LOG("WIFI OFF"); }
  }
  if (readButton(btnRec)) { sd_recording ? stopSDRecording() : startSDRecording(); }
  if (readButton(btnRst)) {
    LOG("RIAVVIO..."); digitalWrite(LED_RST, HIGH); delay(500); ESP.restart();
  }

  digitalWrite(LED_REC, sd_recording ? HIGH : LOW);

  // HTTP server
  server.handleClient();

  // UDP comandi
  if (wifi_enabled) {
    int ps = udp.parsePacket();
    if (ps > 0) {
      int len = udp.read(udpRxBuffer, sizeof(udpRxBuffer)-1);
      if (len > 0) {
        udpRxBuffer[len] = '\0';
        String cmd = String(udpRxBuffer); cmd.trim();
        if      (cmd == "START_RECORDING" && !sd_recording) startSDRecording();
        else if (cmd == "STOP_RECORDING"  &&  sd_recording) stopSDRecording();
        else if (cmd == "LS")    listSDFiles();
        else if (cmd == "INIT_SD") sd_available = runFullSDTest();
      }
    }
  }

  // GPS
  while (Serial1.available() > 0) {
    char c = Serial1.read();
    gps.encode(c);
    static unsigned long lb = 0;
    if (millis()-lb > 10) { digitalWrite(LED_1, !digitalRead(LED_1)); lb = millis(); }
  }

  // IMU @ 50Hz
  if (micros() - lastMicros >= SAMPLE_PERIOD_MICROS) {
    lastMicros = micros();
    if (imu_available) imu.read();
    if (mag_available) mag.read();

    if (imu_available && imu.last_status == 0) {
      float gx = (imu.g.x - gx_bias) * GYRO_SENSITIVITY;
      float gy = (imu.g.y - gy_bias) * GYRO_SENSITIVITY;
      float gz = (imu.g.z - gz_bias) * GYRO_SENSITIVITY;
      float ax = imu.a.x * ACCEL_SENSITIVITY;
      float ay = imu.a.y * ACCEL_SENSITIVITY;
      float az = imu.a.z * ACCEL_SENSITIVITY;
      float mx=0, my=0, mz=0;

      if (mag_available) {
        mx = mag.m.x * MAG_SENSITIVITY;
        my = mag.m.y * MAG_SENSITIVITY;
        mz = mag.m.z * MAG_SENSITIVITY;
        filter.update(gx, gy, gz, ax, ay, az, mx, my, mz);
      } else {
        filter.updateIMU(gx, gy, gz, ax, ay, az);
      }

      float roll  = filter.getRoll();
      float pitch = filter.getPitch();
      float yaw   = filter.getYaw();

      Serial.printf("DATA:%.2f,%.2f,%.2f\n", roll, pitch, yaw);
      snprintf(udpBuffer, sizeof(udpBuffer), "IMU,%.2f,%.2f,%.2f", roll, pitch, yaw);
      sendUDP(udpBuffer);
      writeSDData(roll, pitch, yaw, ax, ay, az, gx, gy, gz, mx, my, mz);
    }
  }

  // GPS @ 1Hz
  if (millis() - lastGPSUpdate >= 1000) {
    lastGPSUpdate = millis();
    if (gps.location.isValid()) {
      cached_lat=gps.location.lat(); cached_lng=gps.location.lng();
      cached_alt=gps.altitude.meters(); cached_speed=gps.speed.kmph();
      cached_sats=gps.satellites.value();
      Serial.printf("GPS:%.6f,%.6f,%.1f,%.1f,%d\n",
                    cached_lat, cached_lng, cached_alt, cached_speed, cached_sats);
      snprintf(udpBuffer, sizeof(udpBuffer), "GPS,%d,%.6f,%.6f",
               cached_sats, cached_lat, cached_lng);
    } else {
      cached_sats = gps.satellites.value();
      snprintf(udpBuffer, sizeof(udpBuffer), "GPS,%d,NO_FIX,NO_FIX", cached_sats);
    }
    sendUDP(udpBuffer);
  }

  // Status broadcast @ 3s
  static unsigned long lastSB = 0;
  if (millis() - lastSB >= 3000) {
    lastSB = millis();
    if (sd_available && !sd_recording) {
      sendUDP("STATUS:SD_OK:In_Attesa");
      snprintf(udpBuffer, sizeof(udpBuffer), "STATUS:SD_NEXT_SESSION:%04d", sd_session_number);
      sendUDP(udpBuffer);
    } else if (!sd_available) {
      sendUDP("STATUS:SD_NOT_FOUND");
    }
  }
}
