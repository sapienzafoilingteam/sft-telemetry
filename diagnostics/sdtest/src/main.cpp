#if __has_include("secrets.h")
#include "secrets.h"
#else
#define SFT_WIFI_PASSWORD "change-me-now"
#endif

/**
 * ============================================================================
 * SFT TELEMETRY — SD DEBUG via WiFi (USB staccata)
 * ============================================================================
 * Fix rispetto alla versione precedente:
 *   - WiFi + HTTP server attivi PRIMA di qualsiasi altra cosa
 *   - Calibrazione e test SD eseguiti nel loop() in modo non bloccante
 *   - server.handleClient() chiamato costantemente → pagina sempre
 * raggiungibile
 *   - La pagina mostra "In corso..." mentre i test girano, poi si aggiorna
 *
 * Come usare:
 *   1. Carica con USB collegata
 *   2. Stacca USB, alimenta da batteria/powerbank
 *   3. Connetti al WiFi "SFT_telemetry"
 *   4. Apri http://192.168.4.1 nel browser
 *   5. La pagina si aggiorna ogni 3s automaticamente
 * ============================================================================
 */

#include <Arduino.h>
#include <LIS3MDL.h>
#include <LSM6.h>
#include <MadgwickAHRS.h>
#include <SD.h>
#include <SdFat.h>
#include <SPI.h>
#include <TinyGPS++.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Wire.h>

/* =========================================================================
 * NETWORK
 * ========================================================================= */
const char *ssid = "SFT_telemetry";
const char *password = SFT_WIFI_PASSWORD;
const char *targetIP = "255.255.255.255";
const int targetPort = 4210;

WiFiUDP udp;
WebServer server(80);

/* =========================================================================
 * PIN
 * ========================================================================= */
#define I2C_SDA 8
#define I2C_SCL 9
#define LED_1 21
#define LED_2 45
#define BTN_WIFI 5
#define LED_WIFI 4
#define BTN_REC 7
#define LED_REC 6
#define BTN_RST 18
#define LED_RST 17
#define GPS_RX 2
#define GPS_TX 1

// Pin SD originali PCB ONDA v2.5 — test senza USB
#define SD_CS 10
#define SD_MOSI 11
#define SD_SCK 12
#define SD_MISO 13

/* =========================================================================
 * COSTANTI
 * ========================================================================= */
const int SAMPLE_RATE_HZ = 50;
const unsigned long SAMPLE_PERIOD_MICROS = 1000000 / SAMPLE_RATE_HZ;
const float GYRO_SENSITIVITY = 0.00875f;
const float ACCEL_SENSITIVITY = 0.000061f;
const float MAG_SENSITIVITY = 0.000146f;
const int SD_BUFFER_LINES = 50;

/* =========================================================================
 * LOG BUFFER CIRCOLARE
 * Sostituisce Serial: tutti i messaggi finiscono qui e vengono letti
 * dal browser via /status JSON. Serial resta attivo in parallelo.
 * ========================================================================= */
#define LOG_LINES 100
#define LOG_WIDTH 180

char logBuf[LOG_LINES][LOG_WIDTH];
int logHead = 0;  // prossima posizione di scrittura
int logCount = 0; // righe totali inserite (cap LOG_LINES)

void LOG(const char *msg) {
  Serial.println(msg);
  strncpy(logBuf[logHead], msg, LOG_WIDTH - 1);
  logBuf[logHead][LOG_WIDTH - 1] = '\0';
  logHead = (logHead + 1) % LOG_LINES;
  if (logCount < LOG_LINES)
    logCount++;
}

void LOGf(const char *fmt, ...) {
  char tmp[LOG_WIDTH];
  va_list a;
  va_start(a, fmt);
  vsnprintf(tmp, sizeof(tmp), fmt, a);
  va_end(a);
  LOG(tmp);
}

/* =========================================================================
 * STATO GLOBALE
 * ========================================================================= */
LSM6 imu;
LIS3MDL mag;
Madgwick filter;
TinyGPSPlus gps;
SdFat sdFat;
SPIClass sdSPI(FSPI);

unsigned long lastMicros = 0;
unsigned long lastGPSUpdate = 0;

float gx_bias = 0, gy_bias = 0, gz_bias = 0;
bool imu_available = false;
bool mag_available = false;

char udpBuffer[256];

bool sd_available = false;
bool sd_recording = false;
File sd_file;
int sd_session_number = 0;
char sd_filename[32];
String sd_write_buffer = "";
int sd_buffer_count = 0;
unsigned long sd_last_flush = 0;

double cached_lat = 0, cached_lng = 0;
double cached_alt = 0, cached_speed = 0;
int cached_sats = 0;

char udpRxBuffer[64];
bool wifi_enabled = true;

/* =========================================================================
 * MACCHINA A STATI — avvio non bloccante
 *
 * Il setup() fa SOLO WiFi + HTTP. Tutto il resto (IMU, gyro calib, SD test)
 * viene eseguito nel loop() uno step alla volta, chiamando handleClient()
 * tra uno step e l'altro. Così il browser è sempre raggiungibile.
 * ========================================================================= */
enum StartupState {
  SS_WIFI_DONE,  // WiFi già su (fatto in setup)
  SS_INIT_I2C,   // Init Wire + IMU + MAG
  SS_CALIB_GYRO, // Calibrazione giroscopio (3s, non bloccante)
  SS_TEST_SD,    // Test SD completo
  SS_RUNNING     // Tutto pronto, loop normale
};
StartupState startupState = SS_WIFI_DONE;

// Calibrazione giroscopio non bloccante
unsigned long calibStartMs = 0;
long calibSumX = 0, calibSumY = 0, calibSumZ = 0, calibCount = 0;
const unsigned long CALIB_DURATION_MS = 3000;

/* =========================================================================
 * RISULTATI TEST SD
 * ========================================================================= */
struct SDTestResult {
  bool miso_pullup;
  bool cs_toggle, sck_toggle, mosi_toggle;
  bool raw_cmd0;
  uint8_t raw_r1;
  bool init_ok;
  uint32_t init_freq;
  bool write_ok, read_ok, verify_ok;
  char diagnosis[256];
  bool done; // true quando il test è completato
} sdTest;

/* =========================================================================
 * BUTTONS
 * ========================================================================= */
struct ButtonState {
  uint8_t pin;
  bool lastState, currentState;
  unsigned long lastDebounceTime;
  ButtonState(uint8_t p)
      : pin(p), lastState(LOW), currentState(LOW), lastDebounceTime(0) {}
};
ButtonState btnWifi(BTN_WIFI), btnRec(BTN_REC), btnRst(BTN_RST);
const unsigned long DEBOUNCE_MS = 20;

bool readButton(ButtonState &b) {
  int r = digitalRead(b.pin);
  if (r != b.lastState)
    b.lastDebounceTime = millis();
  bool pressed = false;
  if ((millis() - b.lastDebounceTime) > DEBOUNCE_MS && r != b.currentState) {
    b.currentState = r;
    if (r == HIGH)
      pressed = true;
  }
  b.lastState = r;
  return pressed;
}

void sendUDP(const char *msg) {
  if (!wifi_enabled)
    return;
  udp.beginPacket(targetIP, targetPort);
  udp.print(msg);
  udp.endPacket();
}

/* =========================================================================
 * SD FUNCTIONS
 * ========================================================================= */
int findNextSessionNumber() {
  int mx = 0;
  File root = SD.open("/");
  if (!root)
    return 1;
  File e;
  while ((e = root.openNextFile())) {
    String n = e.name();
    if (n.startsWith("session_") && n.endsWith(".csv")) {
      int num = n.substring(8, n.length() - 4).toInt();
      if (num > mx)
        mx = num;
    }
    e.close();
  }
  root.close();
  return mx + 1;
}

bool startSDRecording() {
  if (!sd_available) {
    LOG("SD_REC_ERROR:NO_SD");
    return false;
  }
  snprintf(sd_filename, sizeof(sd_filename), "/session_%04d.csv",
           sd_session_number);
  sd_file = SD.open(sd_filename, FILE_WRITE);
  if (!sd_file) {
    LOGf("SD_REC_ERROR:CANT_OPEN:%s", sd_filename);
    return false;
  }
  sd_file.println("timestamp_ms,roll,pitch,yaw,ax,ay,az,gx,gy,gz,mx,my,mz,"
                  "gps_lat,gps_lng,gps_alt,gps_speed,gps_sats");
  sd_file.flush();
  sd_recording = true;
  sd_buffer_count = 0;
  sd_write_buffer = "";
  sd_write_buffer.reserve(4096);
  sd_last_flush = millis();
  LOGf("SD_REC_STARTED:%s", sd_filename + 1);
  snprintf(udpBuffer, sizeof(udpBuffer), "STATUS:SD_REC_STARTED:%s",
           sd_filename + 1);
  sendUDP(udpBuffer);
  return true;
}

void stopSDRecording() {
  if (!sd_recording)
    return;
  if (sd_write_buffer.length() > 0)
    sd_file.print(sd_write_buffer);
  sd_file.flush();
  sd_file.close();
  sd_recording = false;
  sd_write_buffer = "";
  sd_buffer_count = 0;
  LOGf("SD_REC_STOPPED:%s", sd_filename + 1);
  snprintf(udpBuffer, sizeof(udpBuffer), "STATUS:SD_REC_STOPPED:%s",
           sd_filename + 1);
  sendUDP(udpBuffer);
  sd_session_number++;
}

void writeSDData(float r, float p, float y, float ax, float ay, float az,
                 float gx, float gy, float gz, float mx, float my, float mz) {
  if (!sd_recording)
    return;
  char line[256];
  snprintf(line, sizeof(line),
           "%lu,%.2f,%.2f,%.2f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,"
           "%.6f,%.6f,%.1f,%.1f,%d\n",
           millis(), r, p, y, ax, ay, az, gx, gy, gz, mx, my, mz, cached_lat,
           cached_lng, cached_alt, cached_speed, cached_sats);
  sd_write_buffer += line;
  sd_buffer_count++;
  if (sd_buffer_count >= SD_BUFFER_LINES ||
      (millis() - sd_last_flush) >= 1000) {
    sd_file.print(sd_write_buffer);
    sd_file.flush();
    sd_write_buffer = "";
    sd_buffer_count = 0;
    sd_last_flush = millis();
  }
}

void listSDFiles() {
  if (!sd_available)
    return;
  File root = SD.open("/");
  if (!root)
    return;
  LOG("SD_LIST_START");
  File e;
  while ((e = root.openNextFile())) {
    if (!e.isDirectory())
      LOGf("FILE:%s|%llu", e.name(), e.size());
    e.close();
  }
  root.close();
  LOG("SD_LIST_END");
}

/* =========================================================================
 * TEST SD — eseguito una volta, non bloccante perché chiamato dal loop
 * con server.handleClient() già chiamato prima
 * ========================================================================= */
bool runFullSDTest() {
  memset(&sdTest, 0, sizeof(sdTest));
  sdTest.done = false;
  strcpy(sdTest.diagnosis, "In corso...");

  LOG("=== TEST SD AVVIATO ===");
  LOGf("Pin: CS=%d MOSI=%d SCK=%d MISO=%d", SD_CS, SD_MOSI, SD_SCK, SD_MISO);
  server.handleClient(); // Serve una richiesta HTTP tra uno step e l'altro

  // --- STEP 1: Diagnostica Avanzata SdFat ---
  LOG("--- STEP 1: Diagnostica SdFat ---");
  
  sdTest.init_ok = false;
  sdTest.verify_ok = false;
  
  // Test vari setup SPI
  struct SPISetup {
      const char* name;
      uint8_t pullup_mode; // 0=none, 1=pullup, 2=pulldown
      uint32_t speed;
  };
  
  SPISetup setups[] = {
      {"1MHz, PullUp MISO", 1, SD_SCK_MHZ(1)},
      {"400kHz, PullUp MISO", 1, 400000UL},
      {"4MHz, PullUp MISO", 1, SD_SCK_MHZ(4)},
      {"1MHz, No Pull", 0, SD_SCK_MHZ(1)},
      {"100kHz, PullUp", 1, 100000UL}
  };
  
  bool success = false;
  
  for (int i=0; i<5 && !success; i++) {
      LOGf("-> Tentativo %d: %s", i+1, setups[i].name);
      server.handleClient();
      
      sdSPI.end();
      delay(50);
      sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, -1);
      
      if (setups[i].pullup_mode == 1) {
          pinMode(SD_MISO, INPUT_PULLUP);
      } else if (setups[i].pullup_mode == 2) {
          pinMode(SD_MISO, INPUT_PULLDOWN);
      }
      
      delay(50);
      
      if (!sdFat.cardBegin(SdSpiConfig(SD_CS, SHARED_SPI, setups[i].speed, &sdSPI))) {
          uint8_t eCode = sdFat.card()->errorCode();
          uint8_t eData = sdFat.card()->errorData();
          LOGf("[FAIL] Error: 0x%02X Data: 0x%02X", eCode, eData);
          if (eCode == SD_CARD_ERROR_CMD0) {
             LOG("  Diagnosi: CMD0 no-reply. SD assente, pin errati, o no 5V.");
          } else if (eCode == SD_CARD_ERROR_CMD8) {
             LOG("  Diagnosi: CMD8 fail. SD v1, o voltaggio incompatibile.");
          } else if (eCode == SD_CARD_ERROR_ACMD41) {
             LOG("  Diagnosi: ACMD41 timeout. SD non esce da idle.");
          }
      } else {
          LOG("[PASS] cardBegin OK!");
          sdTest.init_ok = true;
          
          if (!sdFat.volumeBegin()) {
             LOG("[WARN] volumeBegin fail! Forse partizione exFAT non supportata da config, o corrotta.");
             strcpy(sdTest.diagnosis, "SD hardware OK, ma file system corrotto/formato errato (exFAT?).");
          } else {
             LOG("[PASS] File System montato!");
             success = true;
          }
      }
      server.handleClient();
      delay(200);
  }
  
  if (!success) {
      if (!sdTest.init_ok) {
          strcpy(sdTest.diagnosis, "Nessuna configurazione SPI ha funzionato. Verifica pin e VCC.");
      }
      sdTest.done = true;
      return false;
  }
  
  // --- STEP 2: SD.begin() Fallback per le vecchie funzioni ---
  LOG("--- STEP 2: Riconfiguro per libreria SD standard ---");
  sdFat.end();
  delay(100);
  
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, -1);
  pinMode(SD_MISO, INPUT_PULLUP);
  delay(50);
  
  if (!SD.begin(SD_CS, sdSPI, 4000000)) {
      LOG("[FAIL] La libreria SD standard ha fallito, ma SdFat andava!");
      strcpy(sdTest.diagnosis, "Incompatibilita libreria SD, usare SdFat.");
      sdTest.done = true;
      return false;
  }
  
  LOG("[PASS] SD.begin OK!");

  // --- STEP 4: Read/Write ---
  LOG("--- STEP 4: Lettura/scrittura ---");
  const char *TF = "/diag_test.txt";
  const char *TC = "SFT_DIAG_OK_1234567890";
  const int TCL = strlen(TC);

  if (SD.exists(TF))
    SD.remove(TF);
  File wf = SD.open(TF, FILE_WRITE);
  if (!wf) {
    LOG("[FAIL] open write");
    strcpy(sdTest.diagnosis,
           "Scrittura fallita — scheda read-only o corrotta.");
    sdTest.done = true;
    return false;
  }
  size_t wr = wf.print(TC);
  wf.flush();
  wf.close();
  sdTest.write_ok = (wr == (size_t)TCL);
  LOGf("[%s] Scrittura %d/%d bytes", sdTest.write_ok ? "PASS" : "FAIL", wr,
       TCL);
  server.handleClient();

  File rf = SD.open(TF, FILE_READ);
  if (!rf) {
    LOG("[FAIL] open read");
    sdTest.done = true;
    return false;
  }
  char rb[64] = {0};
  int nr = rf.readBytes(rb, sizeof(rb) - 1);
  rf.close();
  sdTest.read_ok = (nr > 0);
  LOGf("[%s] Lettura %d bytes: \"%s\"", sdTest.read_ok ? "PASS" : "FAIL", nr,
       rb);

  sdTest.verify_ok = (strcmp(rb, TC) == 0);
  LOGf("[%s] Verifica contenuto", sdTest.verify_ok ? "PASS" : "FAIL");
  SD.remove(TF);
  server.handleClient();

  if (sdTest.verify_ok) {
    strcpy(sdTest.diagnosis, "SD completamente funzionante!");
    sd_session_number = findNextSessionNumber();
    LOGf("Prossima sessione: %04d", sd_session_number);
  } else {
    strcpy(sdTest.diagnosis, "Dati corrotti — problema alimentazione.");
  }

  sdTest.done = true;
  LOG("=== TEST SD COMPLETATO ===");
  return sdTest.init_ok && sdTest.verify_ok;
}

/* =========================================================================
 * WEB SERVER — Pagina di debug
 * ========================================================================= */
const char HTML_PAGE[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="it">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>SFT SD Debug</title>
<style>
  *{box-sizing:border-box;margin:0;padding:0}
  body{background:#0d1117;color:#c9d1d9;font-family:monospace;font-size:13px;padding:12px}
  h1{color:#58a6ff;font-size:17px;margin-bottom:4px}
  .sub{color:#8b949e;font-size:11px;margin-bottom:12px}
  .badges{display:flex;flex-wrap:wrap;gap:6px;margin-bottom:12px}
  .badge{padding:3px 12px;border-radius:20px;font-size:11px;font-weight:bold}
  .ok{background:#238636;color:#fff}
  .err{background:#da3633;color:#fff}
  .na{background:#30363d;color:#8b949e}
  .warn{background:#9e6a03;color:#fff}
  .card{background:#161b22;border:1px solid #30363d;border-radius:6px;padding:10px;margin-bottom:10px}
  .card h2{color:#58a6ff;font-size:12px;text-transform:uppercase;margin-bottom:8px}
  table{width:100%;border-collapse:collapse}
  td{padding:3px 6px;border-bottom:1px solid #21262d;vertical-align:top}
  td:first-child{color:#8b949e;width:52%}
  .P{color:#3fb950;font-weight:bold}
  .F{color:#f85149;font-weight:bold}
  .W{color:#d29922;font-weight:bold}
  .I{color:#58a6ff}
  .log{background:#0d1117;padding:8px;border-radius:4px;height:280px;
       overflow-y:auto;white-space:pre-wrap;word-break:break-all;font-size:11px;line-height:1.5}
  .lP{color:#3fb950}.lF{color:#f85149}.lW{color:#d29922}.lI{color:#8b949e}
  .btns{display:flex;flex-wrap:wrap;gap:6px}
  button{background:#21262d;border:1px solid #30363d;color:#c9d1d9;
         padding:6px 14px;border-radius:6px;cursor:pointer;font-family:monospace;font-size:12px}
  button:hover{background:#30363d}
  .spin{display:inline-block;animation:spin 1s linear infinite}
  @keyframes spin{to{transform:rotate(360deg)}}
  #uptime{color:#8b949e;font-size:11px}
</style>
</head>
<body>
<h1>&#x1F4E1; SFT — SD Debug WiFi (USB staccata)</h1>
<div class="sub">
  IP: 192.168.4.1 &nbsp;|&nbsp; Pin SD: CS=10 MOSI=11 SCK=12 MISO=13 &nbsp;|&nbsp;
  <span id="uptime">---</span>
</div>

<div class="badges" id="badges"></div>

<div class="card">
  <h2>&#x1F50D; Risultati Test SD <span id="spin" class="spin" style="display:none">&#x21BB;</span></h2>
  <table id="tbl"><tr><td colspan="2" class="I">In attesa dei risultati...</td></tr></table>
</div>

<div class="card">
  <h2>&#x1F4CB; Log Sistema</h2>
  <div class="log" id="logbox"></div>
</div>

<div class="card">
  <h2>&#x1F4BE; Controllo</h2>
  <div class="btns">
    <button onclick="cmd('START_RECORDING')">&#x23FA; Rec</button>
    <button onclick="cmd('STOP_RECORDING')">&#x23F9; Stop</button>
    <button onclick="cmd('LS')">&#x1F4C1; File</button>
    <button onclick="cmd('INIT_SD')">&#x1F504; Re-test SD</button>
  </div>
</div>

<script>
let lastLogCount = 0;

function cls(s){
  if(s==='PASS')return 'P';if(s==='FAIL')return 'F';
  if(s==='WARN')return 'W';return 'I';
}
function esc(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;')}

async function poll(){
  try{
    const r=await fetch('/status?t='+Date.now());
    const d=await r.json();

    // Uptime
    const s=Math.floor(d.uptime_ms/1000);
    document.getElementById('uptime').textContent=
      'uptime '+Math.floor(s/60)+'m'+('0'+s%60).slice(-2)+'s';

    // Spin se startup non finito
    document.getElementById('spin').style.display=d.startup_done?'none':'inline-block';

    // Badges
    const bdata=[
      {id:'bsd', ok:d.sd_ok,    t: d.sd_ok?'SD OK':'SD FAIL'},
      {id:'bim', ok:d.imu_ok,   t: d.imu_ok?'IMU OK':'IMU FAIL'},
      {id:'bmg', ok:d.mag_ok,   t: d.mag_ok?'MAG OK':'MAG FAIL'},
      {id:'bgp', ok:d.gps_fix,  t: d.gps_fix?'GPS FIX':'GPS NO FIX'},
      {id:'brc', ok:d.recording,t: d.recording?'● REC':'○ REC'},
      {id:'bst', ok:d.startup_done, t: d.startup_done?'PRONTO':'AVVIO...'},
    ];
    let bh='';
    for(const b of bdata){
      const c=b.ok===true?'ok':b.ok===false?'err':'na';
      bh+=`<span class="badge ${c}">${b.t}</span>`;
    }
    document.getElementById('badges').innerHTML=bh;

    // Test table
    const rows=d.tests||[];
    if(rows.length>0){
      let th='';
      for(const row of rows){
        th+=`<tr><td>${esc(row.name)}</td>
             <td class="${cls(row.status)}">${row.status} ${esc(row.value||'')}</td></tr>`;
      }
      document.getElementById('tbl').innerHTML=th;
    }

    // Log — aggiungi solo righe nuove
    const lines=d.log||[];
    if(lines.length!==lastLogCount){
      lastLogCount=lines.length;
      let lh='';
      for(const l of lines){
        let c='lI';
        if(l.includes('[PASS]')||l.includes('OK!')||l.includes('READY')||l.includes('PRONTO')) c='lP';
        else if(l.includes('[FAIL]')||l.includes('ERROR')||l.includes('FAIL')) c='lF';
        else if(l.includes('[WARN]')||l.includes('WARN')) c='lW';
        lh+=`<span class="${c}">${esc(l)}\n</span>`;
      }
      const lb=document.getElementById('logbox');
      lb.innerHTML=lh;
      lb.scrollTop=lb.scrollHeight;
    }
  }catch(e){console.warn(e)}
}

async function cmd(c){
  try{await fetch('/cmd?c='+c)}catch(e){}
  setTimeout(poll,500);
}

poll();
setInterval(poll,3000);
</script>
</body>
</html>
)rawhtml";

void handleRoot() { server.send_P(200, "text/html", HTML_PAGE); }

void handleStatus() {
  // Costruisce il JSON di stato — nessun blocco, solo lettura variabili
  String j = "{";
  j += "\"uptime_ms\":" + String(millis()) + ",";
  j += "\"startup_done\":" +
       String(startupState == SS_RUNNING ? "true" : "false") + ",";
  j += "\"sd_ok\":" + String(sd_available ? "true" : "false") + ",";
  j += "\"imu_ok\":" + String(imu_available ? "true" : "false") + ",";
  j += "\"mag_ok\":" + String(mag_available ? "true" : "false") + ",";
  j += "\"gps_fix\":" +
       String((cached_lat != 0 || cached_lng != 0) ? "true" : "false") + ",";
  j += "\"recording\":" + String(sd_recording ? "true" : "false") + ",";

  // Tabella test SD
  j += "\"tests\":[";
  auto row = [&](const char *name, const char *status, const char *val) {
    j += "{\"name\":\"";
    j += name;
    j += "\",\"status\":\"";
    j += status;
    j += "\",\"value\":\"";
    j += val;
    j += "\"},";
  };
  char vb[64];

  if (sdTest.done || startupState == SS_RUNNING) {
    row("MISO=13 HIGH con pull-up", sdTest.miso_pullup ? "PASS" : "FAIL",
        sdTest.miso_pullup ? "nessun conflitto USB/flash"
                           : "LOW = conflitto bus!");
    snprintf(vb, sizeof(vb), "R1=0x%02X", sdTest.raw_r1);
    row("CMD0 raw (bitbang)", sdTest.raw_cmd0 ? "PASS" : "FAIL", vb);
    if (sdTest.init_ok) {
      snprintf(vb, sizeof(vb), "@ %lu kHz", sdTest.init_freq / 1000);
      row("SD.begin()", "PASS", vb);
    } else {
      row("SD.begin()", "FAIL", "nessuna freq funziona");
    }
    row("Scrittura file",
        sdTest.write_ok ? "PASS" : (sdTest.init_ok ? "FAIL" : "N/A"), "");
    row("Lettura file",
        sdTest.read_ok ? "PASS" : (sdTest.init_ok ? "FAIL" : "N/A"), "");
    row("Verifica dati",
        sdTest.verify_ok ? "PASS" : (sdTest.init_ok ? "FAIL" : "N/A"), "");
    row("Diagnosi", "INFO", sdTest.diagnosis);
  } else {
    // Ancora in avvio
    const char *phase = startupState == SS_INIT_I2C ? "Init IMU..."
                        : startupState == SS_CALIB_GYRO
                            ? "Calibrazione giroscopio..."
                        : startupState == SS_TEST_SD ? "Test SD in corso..."
                                                     : "...";
    row("Stato avvio", "INFO", phase);
  }
  // Rimuovi virgola finale
  if (j.endsWith(","))
    j.remove(j.length() - 1);
  j += "],";

  // Log
  j += "\"log\":[";
  int start = (logCount >= LOG_LINES) ? logHead : 0;
  int cnt = min(logCount, LOG_LINES);
  for (int i = 0; i < cnt; i++) {
    int idx = (start + i) % LOG_LINES;
    String line = logBuf[idx];
    line.replace("\\", "\\\\");
    line.replace("\"", "\\\"");
    j += "\"" + line + "\"";
    if (i < cnt - 1)
      j += ",";
  }
  j += "]}";

  server.send(200, "application/json", j);
}

void handleCmd() {
  if (!server.hasArg("c")) {
    server.send(400, "text/plain", "Missing c");
    return;
  }
  String c = server.arg("c");
  server.send(200, "text/plain", "OK");
  if (c == "START_RECORDING" && !sd_recording)
    startSDRecording();
  else if (c == "STOP_RECORDING" && sd_recording)
    stopSDRecording();
  else if (c == "LS")
    listSDFiles();
  else if (c == "INIT_SD") {
    startupState = SS_TEST_SD;
    sd_available = false;
    memset(&sdTest, 0, sizeof(sdTest));
    strcpy(sdTest.diagnosis, "Re-test in corso...");
  }
}

/* =========================================================================
 * SETUP — SOLO WiFi + HTTP, tutto il resto nel loop
 * ========================================================================= */
void setup() {
  Serial.begin(115200);

  // Pin LED e pulsanti
  pinMode(LED_1, OUTPUT);
  digitalWrite(LED_1, HIGH);
  pinMode(LED_2, OUTPUT);
  digitalWrite(LED_2, HIGH);
  pinMode(LED_WIFI, OUTPUT);
  digitalWrite(LED_WIFI, LOW);
  pinMode(LED_REC, OUTPUT);
  digitalWrite(LED_REC, LOW);
  pinMode(LED_RST, OUTPUT);
  digitalWrite(LED_RST, LOW);
  pinMode(BTN_WIFI, INPUT_PULLDOWN);
  pinMode(BTN_REC, INPUT_PULLDOWN);
  pinMode(BTN_RST, INPUT_PULLDOWN);

  // GPS UART
  Serial1.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);

  // *** WIFI PRIMA DI TUTTO ***
  // Nessun delay lungo, nessun altra init prima di qui
  Serial.println("Avvio WiFi...");
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid, password);
  delay(500); // minimo indispensabile per radio AP
  digitalWrite(LED_WIFI, HIGH);

  LOG("=== SFT TELEMETRY AVVIATO ===");
  LOGf("WiFi AP: %s", ssid);
  LOGf("IP: %s", WiFi.softAPIP().toString().c_str());
  LOGf("Apri: http://%s", WiFi.softAPIP().toString().c_str());

  // UDP
  udp.begin(targetPort);

  // HTTP routes — tutte registrate prima di server.begin()
  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/cmd", handleCmd);
  server.on("/download", HTTP_GET, []() {
    if (!sd_available) {
      server.send(503, "text/plain", "SD N/A");
      return;
    }
    if (!server.hasArg("file")) {
      server.send(400, "text/plain", "Missing file");
      return;
    }
    String fn = server.arg("file");
    if (!fn.startsWith("/"))
      fn = "/" + fn;
    if (!SD.exists(fn)) {
      server.send(404, "text/plain", "Not found");
      return;
    }
    File df = SD.open(fn, FILE_READ);
    if (!df) {
      server.send(500, "text/plain", "Open failed");
      return;
    }
    server.sendHeader("Content-Disposition",
                      "attachment; filename=\"" + fn.substring(1) + "\"");
    server.streamFile(df, "application/octet-stream");
    df.close();
  });
  server.begin();

  LOG("HTTP server attivo su porta 80");
  LOG("Il resto dell'init avviene nel loop...");

  // Avvia la macchina a stati — setup() finisce QUI, velocissimo
  startupState = SS_INIT_I2C;
}

/* =========================================================================
 * LOOP — gestisce sia l'avvio non bloccante che il loop normale
 * ========================================================================= */
void loop() {

  // *** HTTP SEMPRE PRIMO — risponde ad ogni iterazione ***
  server.handleClient();

  // --- MACCHINA A STATI: avvio non bloccante ---
  if (startupState != SS_RUNNING) {

    switch (startupState) {

    // -----------------------------------------------------------------
    case SS_INIT_I2C:
      LOG("Init I2C + sensori...");
      Wire.begin(I2C_SDA, I2C_SCL, 400000);
      delay(200);
      server.handleClient();

      if (!imu.init()) {
        LOG("WARN: LSM6 non trovato");
        imu_available = false;
      } else {
        LOG("IMU OK (LSM6)");
        imu.enableDefault();
        imu_available = true;
      }
      server.handleClient();

      if (!mag.init()) {
        LOG("WARN: LIS3MDL non trovato");
        digitalWrite(LED_2, LOW);
        mag_available = false;
      } else {
        LOG("MAG OK (LIS3MDL)");
        mag.enableDefault();
        digitalWrite(LED_2, HIGH);
        mag_available = true;
      }
      server.handleClient();

      filter.begin(SAMPLE_RATE_HZ);
      LOG("Filtro Madgwick inizializzato");

      // Avvia calibrazione
      calibStartMs = millis();
      calibSumX = calibSumY = calibSumZ = calibCount = 0;
      LOG("Calibrazione giroscopio (3s)... tieni fermo!");
      startupState = SS_CALIB_GYRO;
      break;

    // -----------------------------------------------------------------
    case SS_CALIB_GYRO:
      // Campiona il giroscopio senza bloccare
      if (imu_available) {
        imu.read();
        if (imu.last_status == 0) {
          calibSumX += imu.g.x;
          calibSumY += imu.g.y;
          calibSumZ += imu.g.z;
          calibCount++;
        }
      }
      // Controlla se i 3 secondi sono passati
      if (millis() - calibStartMs >= CALIB_DURATION_MS) {
        if (calibCount > 0) {
          gx_bias = calibSumX / (float)calibCount;
          gy_bias = calibSumY / (float)calibCount;
          gz_bias = calibSumZ / (float)calibCount;
        }
        LOGf("Gyro bias: %.2f %.2f %.2f (%ld campioni)", gx_bias, gy_bias,
             gz_bias, calibCount);
        startupState = SS_TEST_SD;
      }
      break;

    // -----------------------------------------------------------------
    case SS_TEST_SD:
      // Il test SD chiama server.handleClient() internamente ad ogni step
      LOG("Avvio test SD completo...");
      sd_available = runFullSDTest();
      LOGf("=== SISTEMA PRONTO === SD:%s IMU:%s MAG:%s",
           sd_available ? "OK" : "FAIL", imu_available ? "OK" : "FAIL",
           mag_available ? "OK" : "FAIL");
      lastMicros = micros();
      startupState = SS_RUNNING;
      break;

    default:
      break;
    }

    return; // Durante l'avvio non eseguire il loop normale
  }

  // --- LOOP NORMALE (startupState == SS_RUNNING) ---

  // Pulsanti
  if (readButton(btnWifi)) {
    wifi_enabled = !wifi_enabled;
    if (wifi_enabled) {
      WiFi.mode(WIFI_AP);
      WiFi.softAP(ssid, password);
      digitalWrite(LED_WIFI, HIGH);
      LOG("WIFI ON");
    } else {
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_OFF);
      digitalWrite(LED_WIFI, LOW);
      LOG("WIFI OFF");
    }
  }
  if (readButton(btnRec)) {
    if (sd_recording) {
      stopSDRecording();
    } else {
      startSDRecording();
    }
  }
  if (readButton(btnRst)) {
    LOG("RIAVVIO...");
    digitalWrite(LED_RST, HIGH);
    delay(500);
    ESP.restart();
  }

  digitalWrite(LED_REC, sd_recording ? HIGH : LOW);

  // Comandi UDP
  if (wifi_enabled) {
    int ps = udp.parsePacket();
    if (ps > 0) {
      int len = udp.read(udpRxBuffer, sizeof(udpRxBuffer) - 1);
      if (len > 0) {
        udpRxBuffer[len] = '\0';
        String cmd = String(udpRxBuffer);
        cmd.trim();
        if (cmd == "START_RECORDING" && !sd_recording)
          startSDRecording();
        else if (cmd == "STOP_RECORDING" && sd_recording)
          stopSDRecording();
        else if (cmd == "LS")
          listSDFiles();
        else if (cmd == "INIT_SD") {
          startupState = SS_TEST_SD;
          sd_available = false;
          memset(&sdTest, 0, sizeof(sdTest));
          strcpy(sdTest.diagnosis, "Re-test in corso...");
        }
      }
    }
  }

  // GPS
  while (Serial1.available() > 0) {
    char c = Serial1.read();
    gps.encode(c);
    static unsigned long lb = 0;
    if (millis() - lb > 10) {
      digitalWrite(LED_1, !digitalRead(LED_1));
      lb = millis();
    }
  }

  // IMU @ 50Hz
  if (micros() - lastMicros >= SAMPLE_PERIOD_MICROS) {
    lastMicros = micros();
    if (imu_available)
      imu.read();
    if (mag_available)
      mag.read();

    if (imu_available && imu.last_status == 0) {
      float gx = (imu.g.x - gx_bias) * GYRO_SENSITIVITY;
      float gy = (imu.g.y - gy_bias) * GYRO_SENSITIVITY;
      float gz = (imu.g.z - gz_bias) * GYRO_SENSITIVITY;
      float ax = imu.a.x * ACCEL_SENSITIVITY;
      float ay = imu.a.y * ACCEL_SENSITIVITY;
      float az = imu.a.z * ACCEL_SENSITIVITY;
      float mx = 0, my = 0, mz = 0;

      if (mag_available) {
        mx = mag.m.x * MAG_SENSITIVITY;
        my = mag.m.y * MAG_SENSITIVITY;
        mz = mag.m.z * MAG_SENSITIVITY;
        filter.update(gx, gy, gz, ax, ay, az, mx, my, mz);
      } else {
        filter.updateIMU(gx, gy, gz, ax, ay, az);
      }

      float roll = filter.getRoll(), pitch = filter.getPitch(),
            yaw = filter.getYaw();
      Serial.printf("DATA:%.2f,%.2f,%.2f\n", roll, pitch, yaw);
      snprintf(udpBuffer, sizeof(udpBuffer), "IMU,%.2f,%.2f,%.2f", roll, pitch,
               yaw);
      sendUDP(udpBuffer);
      writeSDData(roll, pitch, yaw, ax, ay, az, gx, gy, gz, mx, my, mz);
    }
  }

  // GPS @ 1Hz
  if (millis() - lastGPSUpdate >= 1000) {
    lastGPSUpdate = millis();
    if (gps.location.isValid()) {
      cached_lat = gps.location.lat();
      cached_lng = gps.location.lng();
      cached_alt = gps.altitude.meters();
      cached_speed = gps.speed.kmph();
      cached_sats = gps.satellites.value();
      snprintf(udpBuffer, sizeof(udpBuffer), "GPS,%d,%.6f,%.6f", cached_sats,
               cached_lat, cached_lng);
    } else {
      cached_sats = gps.satellites.value();
      snprintf(udpBuffer, sizeof(udpBuffer), "GPS,%d,NO_FIX,NO_FIX",
               cached_sats);
    }
    sendUDP(udpBuffer);
  }

  // Status broadcast @ 3s
  static unsigned long lastSB = 0;
  if (millis() - lastSB >= 3000) {
    lastSB = millis();
    if (sd_available && !sd_recording) {
      sendUDP("STATUS:SD_OK:In_Attesa");
      snprintf(udpBuffer, sizeof(udpBuffer), "STATUS:SD_NEXT_SESSION:%04d",
               sd_session_number);
      sendUDP(udpBuffer);
    } else if (!sd_available) {
      sendUDP("STATUS:SD_NOT_FOUND");
    }
  }
}
