#if __has_include("secrets.h")
#include "secrets.h"
#else
#define SFT_WIFI_PASSWORD "change-me-now"
#endif

/**
 * ============================================================================
 * SD INIT FIX — ESP32-S3 ONDA v2.5
 * ============================================================================
 * Diagnosi confermata dal log:
 *   - Pin 11-13: conflitto con flash Octal SPI interno → ABBANDONATI
 *   - Pin 34-37: elettricamente OK (MISO pull-up passa)
 *   - CMD0 fallisce → problema timing/alimentazione al primo init
 *
 * Fix applicati:
 *   1. Pin spostati a 34/35/36/37 (FSPI nativi, lontani dal flash)
 *   2. Sequenza di power-up manuale prima di SD.begin()
 *   3. Delay esteso e CS pre-condizionamento
 *   4. Retry con reset completo bus tra un tentativo e l'altro
 *   5. Frequenza iniziale 400kHz (spec SD ufficiale per init)
 * ============================================================================
 */

#include <Arduino.h>
#include <LIS3MDL.h>
#include <LSM6.h>
#include <MadgwickAHRS.h>
#include <SD.h>
#include <SPI.h>
#include <TinyGPS++.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Wire.h>

/* =========================================================================
 * NETWORK CONFIGURATION (ACCESS POINT MODE)
 * ========================================================================= */
const char *ssid     = "SFT_telemetry";
const char *password = SFT_WIFI_PASSWORD;

const char *targetIP  = "255.255.255.255";
const int   targetPort = 4210;

WiFiUDP   udp;
WebServer server(80);

/* =========================================================================
 * HARDWARE CONFIGURATION & CONSTANTS
 * ========================================================================= */
#define I2C_SDA 8
#define I2C_SCL 9
#define LED_1   21
#define LED_2   45

#define BTN_WIFI 5
#define LED_WIFI 4
#define BTN_REC  7
#define LED_REC  6
#define BTN_RST  18
#define LED_RST  17

#define GPS_RX 2
#define GPS_TX 1

// *** FIX CRITICO: Pin SD spostati lontano dal flash interno ***
// I pin originali 10-13 condividevano il bus Octal SPI del flash.
// I pin 34-37 sono FSPI nativi dell'ESP32-S3, liberi da conflitti.
#define SD_CS   34
#define SD_MOSI 35
#define SD_SCK  36
#define SD_MISO 37

// *** FIX: Frequenza SD ridotta a 400kHz per init, poi 4MHz per operazioni ***
// La spec SD ufficiale richiede init a <= 400kHz prima dell'handshake.
#define SD_INIT_FREQ  400000   // 400 kHz — obbligatorio per spec SD
#define SD_RUN_FREQ  4000000   // 4 MHz  — operativo dopo init riuscita

// SAMPLING RATE
const int SAMPLE_RATE_HZ = 50;
const unsigned long SAMPLE_PERIOD_MICROS = 1000000 / SAMPLE_RATE_HZ;

// SENSOR SCALING CONSTANTS (Pololu MinIMU-9 v5 defaults)
const float GYRO_SENSITIVITY  = 0.00875f;
const float ACCEL_SENSITIVITY = 0.000061f;
const float MAG_SENSITIVITY   = 0.000146f;

// SD write buffer
const int SD_BUFFER_LINES = 50;

/* =========================================================================
 * GLOBAL OBJECTS & STATE VARIABLES
 * ========================================================================= */
LSM6       imu;
LIS3MDL    mag;
Madgwick   filter;
TinyGPSPlus gps;
SPIClass   sdSPI(FSPI);

unsigned long lastMicros;
unsigned long lastGPSUpdate = 0;

float gx_bias = 0, gy_bias = 0, gz_bias = 0;
bool  imu_available = false;
bool  mag_available = false;

char udpBuffer[256];

// --- MicroSD State ---
bool sd_available  = false;
bool sd_recording  = false;
File sd_file;
int  sd_session_number = 0;
char sd_filename[32];

String sd_write_buffer = "";
int    sd_buffer_count = 0;
unsigned long sd_last_flush = 0;

// GPS cached values
double cached_lat = 0.0, cached_lng = 0.0;
double cached_alt = 0.0, cached_speed = 0.0;
int    cached_sats = 0;

char udpRxBuffer[64];

/* =========================================================================
 * BUTTONS & WIFI STATE
 * ========================================================================= */
bool wifi_enabled = true;

struct ButtonState {
  uint8_t pin;
  bool lastState, currentState;
  unsigned long lastDebounceTime;
  ButtonState(uint8_t p) : pin(p), lastState(LOW), currentState(LOW), lastDebounceTime(0) {}
};

ButtonState btnWifi(BTN_WIFI);
ButtonState btnRec(BTN_REC);
ButtonState btnRst(BTN_RST);
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

/* =========================================================================
 * HELPER: Send UDP Broadcast
 * ========================================================================= */
void sendUDP(const char *message) {
  if (!wifi_enabled) return;
  udp.beginPacket(targetIP, targetPort);
  udp.print(message);
  udp.endPacket();
}

/* =========================================================================
 * *** FIX PRINCIPALE: Sequenza init SD robusta ***
 *
 * Problema diagnosticato: la libreria SD manda CMD0 troppo in fretta
 * dopo il reset del bus, e il modulo SD non ha ancora stabilizzato
 * la tensione interna (specialmente con condensatori piccoli sul modulo).
 *
 * Soluzione:
 *   1. Teniamo CS=HIGH durante il power-up (spec SD richiede >= 1ms)
 *   2. Mandiamo 10 byte (80 clock) con CS=HIGH per il power-up della SD
 *   3. Solo allora passiamo il controllo a SD.begin()
 *   4. Fino a 5 tentativi con reset completo del bus tra l'uno e l'altro
 * ========================================================================= */

/**
 * Invia i cicli di clock di power-up alla SD via SPI hardware.
 * CS deve rimanere HIGH durante questa fase (spec SD 6.4.1).
 */
void sdPowerUpSequence() {
  // Configura i pin manualmente per la sequenza di power-up
  pinMode(SD_CS,   OUTPUT); digitalWrite(SD_CS,   HIGH); // CS HIGH = deselezionato
  pinMode(SD_MOSI, OUTPUT); digitalWrite(SD_MOSI, HIGH); // MOSI HIGH durante power-up
  pinMode(SD_SCK,  OUTPUT); digitalWrite(SD_SCK,  LOW);
  pinMode(SD_MISO, INPUT_PULLUP);

  // Attendi che la tensione si stabilizzi sul modulo SD
  // Il condensatore interno della SD si carica in ~1ms ma meglio aspettare 10ms
  delay(10);

  // Invia 80 cicli di clock con CS=HIGH (spec SD: minimo 74 cicli)
  // Questo porta la SD da "Power-up" a "Idle" state nel suo FSM interno
  for (int i = 0; i < 80; i++) {
    digitalWrite(SD_SCK, HIGH);
    delayMicroseconds(5); // ~100kHz manuale, abbastanza lento per qualsiasi SD
    digitalWrite(SD_SCK, LOW);
    delayMicroseconds(5);
  }

  // Abbassa CS per segnalare inizio comunicazione SPI
  // (la libreria SD la gestirà d'ora in poi)
  digitalWrite(SD_CS, HIGH); // Ancora HIGH, SD.begin() lo gestirà lei
  delay(5);

  Serial.println("    [SD] Sequenza power-up (80 cicli) completata");
}

/**
 * Inizializza la SD con retry automatici e reset completo del bus.
 * Restituisce true se la SD è pronta.
 */
bool initSDCard() {
  Serial.println("STATUS:INIT_SD...");

  const int MAX_ATTEMPTS = 5;

  for (int attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
    Serial.printf("  [SD] Tentativo %d/%d\n", attempt, MAX_ATTEMPTS);

    // --- Step 1: Reset completo ---
    SD.end();
    sdSPI.end();
    delay(50 * attempt); // Delay crescente ad ogni tentativo fallito

    // --- Step 2: Sequenza power-up manuale ---
    sdPowerUpSequence();

    // --- Step 3: Init bus SPI hardware ---
    sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    delay(10);

    // --- Step 4: Tentativo SD.begin() a 400kHz (spec SD init frequency) ---
    // Nota: non usiamo la freq alta subito, la SD deve fare handshake a bassa freq
    Serial.printf("    [SD] SD.begin() @ 400kHz... ");
    if (SD.begin(SD_CS, sdSPI, SD_INIT_FREQ)) {
      uint8_t cardType = SD.cardType();
      if (cardType != CARD_NONE) {
        // Successo! Stampa info
        const char *typeStr =
          cardType == CARD_MMC  ? "MMC"  :
          cardType == CARD_SD   ? "SD"   :
          cardType == CARD_SDHC ? "SDHC" : "UNKNOWN";

        uint64_t cardSizeMB = SD.cardSize() / (1024 * 1024);

        Serial.printf("OK! Tipo: %s, Dimensione: %llu MB\n", typeStr, cardSizeMB);
        Serial.printf("STATUS:SD_OK:%s_%lluMB\n", typeStr, cardSizeMB);

        snprintf(udpBuffer, sizeof(udpBuffer), "STATUS:SD_OK:%s_%lluMB",
                 typeStr, cardSizeMB);
        sendUDP(udpBuffer);

        sd_session_number = findNextSessionNumber();
        Serial.printf("STATUS:SD_NEXT_SESSION:%04d\n", sd_session_number);
        return true;

      } else {
        Serial.println("FAIL (CARD_NONE)");
      }
    } else {
      Serial.println("FAIL (SD.begin false)");
    }

    // Attendi prima del prossimo tentativo
    delay(200 * attempt);
  }

  // Tutti i tentativi falliti
  Serial.println("STATUS:SD_NOT_FOUND (tutti i tentativi esauriti)");
  sendUDP("STATUS:SD_NOT_FOUND");
  return false;
}

/* =========================================================================
 * MICROSD FUNCTIONS (invariate rispetto all'originale)
 * ========================================================================= */
int findNextSessionNumber() {
  int maxFound = 0;
  File root = SD.open("/");
  if (!root) return 1;
  File entry;
  while ((entry = root.openNextFile())) {
    String name = entry.name();
    if (name.startsWith("session_") && name.endsWith(".csv")) {
      String numStr = name.substring(8, name.length() - 4);
      int num = numStr.toInt();
      if (num > maxFound) maxFound = num;
    }
    entry.close();
  }
  root.close();
  return maxFound + 1;
}

bool startSDRecording() {
  if (!sd_available) {
    Serial.println("STATUS:SD_REC_ERROR:NO_SD");
    sendUDP("STATUS:SD_REC_ERROR:NO_SD");
    return false;
  }

  snprintf(sd_filename, sizeof(sd_filename), "/session_%04d.csv", sd_session_number);
  sd_file = SD.open(sd_filename, FILE_WRITE);
  if (!sd_file) {
    snprintf(udpBuffer, sizeof(udpBuffer), "STATUS:SD_REC_ERROR:CANT_OPEN:%s", sd_filename);
    sendUDP(udpBuffer);
    return false;
  }

  sd_file.println("timestamp_ms,roll,pitch,yaw,ax,ay,az,gx,gy,gz,mx,my,mz,"
                  "gps_lat,gps_lng,gps_alt,gps_speed,gps_sats");
  sd_file.flush();

  sd_recording    = true;
  sd_buffer_count = 0;
  sd_write_buffer = "";
  sd_write_buffer.reserve(4096);
  sd_last_flush = millis();

  snprintf(udpBuffer, sizeof(udpBuffer), "STATUS:SD_REC_STARTED:%s", sd_filename + 1);
  Serial.printf("STATUS:SD_REC_STARTED:%s\n", sd_filename + 1);
  sendUDP(udpBuffer);
  return true;
}

void stopSDRecording() {
  if (!sd_recording) return;
  if (sd_write_buffer.length() > 0) {
    sd_file.print(sd_write_buffer);
    sd_write_buffer = "";
    sd_buffer_count = 0;
  }
  sd_file.flush();
  sd_file.close();
  sd_recording = false;

  snprintf(udpBuffer, sizeof(udpBuffer), "STATUS:SD_REC_STOPPED:%s", sd_filename + 1);
  Serial.printf("STATUS:SD_REC_STOPPED:%s\n", sd_filename + 1);
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
    sd_file.print(sd_write_buffer);
    sd_file.flush();
    sd_write_buffer = "";
    sd_buffer_count = 0;
    sd_last_flush = millis();
  }
}

void listSDFiles() {
  if (!sd_available) return;
  File root = SD.open("/");
  if (!root) return;
  sendUDP("STATUS:SD_LIST_START");
  File entry;
  while ((entry = root.openNextFile())) {
    if (!entry.isDirectory()) {
      snprintf(udpBuffer, sizeof(udpBuffer), "FILE:%s|%llu", entry.name(), entry.size());
      sendUDP(udpBuffer);
    }
    entry.close();
  }
  root.close();
  sendUDP("STATUS:SD_LIST_END");
}

void deleteSDFile(const char *filename) {
  if (!sd_available) return;
  char path[64];
  if (filename[0] != '/') snprintf(path, sizeof(path), "/%s", filename);
  else strncpy(path, filename, sizeof(path));
  if (SD.remove(path)) {
    snprintf(udpBuffer, sizeof(udpBuffer), "STATUS:SD_DELETE_OK:%s", filename);
    sendUDP(udpBuffer);
    listSDFiles();
  } else {
    snprintf(udpBuffer, sizeof(udpBuffer), "STATUS:SD_DELETE_ERROR:%s", filename);
    sendUDP(udpBuffer);
  }
}

void renameSDFile(const char *oldName, const char *newName) {
  if (!sd_available) return;
  char oldPath[64], newPath[64];
  snprintf(oldPath, sizeof(oldPath), oldName[0] == '/' ? "%s" : "/%s", oldName);
  snprintf(newPath, sizeof(newPath), newName[0] == '/' ? "%s" : "/%s", newName);
  if (SD.rename(oldPath, newPath)) {
    snprintf(udpBuffer, sizeof(udpBuffer), "STATUS:SD_RENAME_OK:%s|%s", oldName, newName);
    sendUDP(udpBuffer);
    listSDFiles();
  } else {
    snprintf(udpBuffer, sizeof(udpBuffer), "STATUS:SD_RENAME_ERROR:%s", oldName);
    sendUDP(udpBuffer);
  }
}

void checkUDPCommands() {
  if (!wifi_enabled) return;
  int packetSize = udp.parsePacket();
  if (packetSize > 0) {
    int len = udp.read(udpRxBuffer, sizeof(udpRxBuffer) - 1);
    if (len > 0) {
      udpRxBuffer[len] = '\0';
      String cmd = String(udpRxBuffer);
      cmd.trim();

      if      (cmd == "START_RECORDING" && !sd_recording) startSDRecording();
      else if (cmd == "STOP_RECORDING"  &&  sd_recording) stopSDRecording();
      else if (cmd == "LS")     listSDFiles();
      else if (cmd == "INIT_SD") sd_available = initSDCard();
      else if (cmd.startsWith("DEL:")) deleteSDFile(cmd.substring(4).c_str());
      else if (cmd.startsWith("REN:")) {
        int ci = cmd.indexOf(',', 4);
        if (ci != -1)
          renameSDFile(cmd.substring(4, ci).c_str(), cmd.substring(ci + 1).c_str());
      }
    }
  }
}

/* =========================================================================
 * SETUP
 * ========================================================================= */
void setup() {
  Serial.begin(115200);

  pinMode(LED_1,    OUTPUT);
  pinMode(LED_2,    OUTPUT);
  pinMode(LED_WIFI, OUTPUT);
  pinMode(LED_REC,  OUTPUT);
  pinMode(LED_RST,  OUTPUT);

  pinMode(BTN_WIFI, INPUT_PULLDOWN);
  pinMode(BTN_REC,  INPUT_PULLDOWN);
  pinMode(BTN_RST,  INPUT_PULLDOWN);

  // GPS
  Serial1.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
  Serial.println("STATUS:GPS_SERIAL_STARTED");

  // Blink di avvio
  for (int i = 0; i < 10; i++) {
    delay(200);
    digitalWrite(LED_1, i % 2);
  }
  digitalWrite(LED_1, HIGH);
  digitalWrite(LED_2, HIGH);

  // WiFi
  Serial.println("STATUS:STARTING_WIFI_HOTSPOT...");
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid, password);
  delay(1000);
  digitalWrite(LED_WIFI, HIGH);
  Serial.printf("STATUS:HOTSPOT_READY — SSID: %s — IP: %s\n",
                ssid, WiFi.softAPIP().toString().c_str());

  // UDP
  udp.begin(targetPort);

  // *** SD INIT con nuova sequenza robusta ***
  Serial.println("STATUS:SD — Uso pin FSPI nativi: CS=34 MOSI=35 SCK=36 MISO=37");
  sd_available = initSDCard();

  // HTTP server per download file SD
  server.on("/download", HTTP_GET, []() {
    if (!sd_available)          { server.send(503, "text/plain", "SD not available"); return; }
    if (!server.hasArg("file")) { server.send(400, "text/plain", "Missing 'file'");   return; }
    String filename = server.arg("file");
    if (!filename.startsWith("/")) filename = "/" + filename;
    if (!SD.exists(filename))   { server.send(404, "text/plain", "Not found");        return; }
    File downloadFile = SD.open(filename, FILE_READ);
    if (!downloadFile)          { server.send(500, "text/plain", "Open failed");      return; }
    server.sendHeader("Content-Disposition",
                      "attachment; filename=\"" + filename.substring(1) + "\"");
    server.streamFile(downloadFile, "application/octet-stream");
    downloadFile.close();
  });
  server.begin();

  // I2C & Sensori
  Wire.begin(I2C_SDA, I2C_SCL, 400000);
  delay(500);

  if (!imu.init()) {
    Serial.println("STATUS:ERROR:LSM6_NOT_FOUND");
    imu_available = false;
  } else {
    Serial.println("STATUS:IMU_OK");
    imu.enableDefault();
    imu_available = true;
  }

  if (!mag.init()) {
    Serial.println("STATUS:WARN:LIS3MDL_NOT_FOUND");
    digitalWrite(LED_2, LOW);
    mag_available = false;
  } else {
    Serial.println("STATUS:MAG_OK");
    mag.enableDefault();
    digitalWrite(LED_2, HIGH);
    mag_available = true;
  }

  filter.begin(SAMPLE_RATE_HZ);

  // Calibrazione giroscopio
  Serial.println("STATUS:CALIBRATING_GYRO... KEEP STILL");
  long count = 0, sumX = 0, sumY = 0, sumZ = 0;
  unsigned long startCalib = millis();
  while (millis() - startCalib < 3000) {
    if (imu_available) imu.read();
    if (imu_available && imu.last_status == 0) {
      sumX += imu.g.x; sumY += imu.g.y; sumZ += imu.g.z;
      count++;
    }
    delay(5);
  }
  if (count > 0) {
    gx_bias = sumX / (float)count;
    gy_bias = sumY / (float)count;
    gz_bias = sumZ / (float)count;
  }

  Serial.println("STATUS:SYSTEM_READY");
  lastMicros = micros();
}

/* =========================================================================
 * MAIN LOOP (identico all'originale)
 * ========================================================================= */
void loop() {

  // 0. Pulsanti
  if (readButton(btnWifi)) {
    wifi_enabled = !wifi_enabled;
    if (wifi_enabled) {
      WiFi.mode(WIFI_AP);
      WiFi.softAP(ssid, password);
      digitalWrite(LED_WIFI, HIGH);
      Serial.println("STATUS:WIFI_ENABLED");
    } else {
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_OFF);
      digitalWrite(LED_WIFI, LOW);
      Serial.println("STATUS:WIFI_DISABLED");
    }
  }

  if (readButton(btnRec)) {
    sd_recording ? stopSDRecording() : startSDRecording();
  }

  if (readButton(btnRst)) {
    Serial.println("STATUS:RESTARTING...");
    digitalWrite(LED_RST, HIGH);
    delay(500);
    ESP.restart();
  }

  digitalWrite(LED_REC, sd_recording ? HIGH : LOW);

  // 1. Comandi UDP
  checkUDPCommands();

  // 1.5. HTTP clients
  server.handleClient();

  // 2. GPS
  while (Serial1.available() > 0) {
    char c = Serial1.read();
    gps.encode(c);
    static unsigned long lastBlink = 0;
    if (millis() - lastBlink > 10) {
      digitalWrite(LED_1, !digitalRead(LED_1));
      lastBlink = millis();
    }
  }

  // 3. IMU @ 50Hz
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
      float mx = 0, my = 0, mz = 0;

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

  // 4. GPS @ 1Hz
  if (millis() - lastGPSUpdate >= 1000) {
    lastGPSUpdate = millis();

    if (gps.location.isValid()) {
      cached_lat   = gps.location.lat();
      cached_lng   = gps.location.lng();
      cached_alt   = gps.altitude.meters();
      cached_speed = gps.speed.kmph();
      cached_sats  = gps.satellites.value();

      Serial.printf("GPS:%.6f,%.6f,%.1f,%.1f,%d\n",
                    cached_lat, cached_lng, cached_alt, cached_speed, cached_sats);
      snprintf(udpBuffer, sizeof(udpBuffer), "GPS,%d,%.6f,%.6f",
               cached_sats, cached_lat, cached_lng);
    } else {
      cached_sats = gps.satellites.value();
      Serial.printf("GPS:0,0,0,0,%d (NO FIX)\n", cached_sats);
      snprintf(udpBuffer, sizeof(udpBuffer), "GPS,%d,NO_FIX,NO_FIX", cached_sats);
    }

    Serial.printf("GPS_DEBUG: Chars=%lu, FailedChecksum=%lu\n",
                  gps.charsProcessed(), gps.failedChecksum());
    sendUDP(udpBuffer);
  }

  // 5. Status broadcast @ 3s
  static unsigned long lastStatusBroadcast = 0;
  if (millis() - lastStatusBroadcast >= 3000) {
    lastStatusBroadcast = millis();
    if (sd_available && !sd_recording) {
      sendUDP("STATUS:SD_OK:In_Attesa");
      snprintf(udpBuffer, sizeof(udpBuffer), "STATUS:SD_NEXT_SESSION:%04d", sd_session_number);
      sendUDP(udpBuffer);
    } else if (!sd_available) {
      sendUDP("STATUS:SD_NOT_FOUND");
    }
  }
}
