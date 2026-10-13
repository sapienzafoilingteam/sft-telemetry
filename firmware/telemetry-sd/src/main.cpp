#if __has_include("secrets.h")
#include "secrets.h"
#else
#define SFT_WIFI_PASSWORD "change-me-now"
#endif

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
// The ESP32 will create this Wi-Fi network. Connect your PC to it!
const char *ssid = "SFT_telemetry";
const char *password = SFT_WIFI_PASSWORD; // Minimum 8 characters

// Broadcast IP: 255.255.255.255 means "Send to everyone connected to my Wi-Fi"
// This eliminates the need to hardcode your PC's IP address.
const char *targetIP = "255.255.255.255";
const int targetPort = 4210;

WiFiUDP udp;
WebServer server(80);

/* =========================================================================
 * HARDWARE CONFIGURATION & CONSTANTS
 * ========================================================================= */
#define I2C_SDA 8
#define I2C_SCL 9
#define LED_1 21
#define LED_2 45

// Tasti e LED aggiuntivi (basati sulla PCB ONDA v2.5)
// NOTA: su PCB i pin sono invertiti rispetto alla serigrafia!
// S1 (Top) - Wi-Fi
#define BTN_WIFI 5
#define LED_WIFI 4
// S2 (Middle) - Record SD
#define BTN_REC 7
#define LED_REC 6
// S3 (Bottom) - Restart
#define BTN_RST 18
#define LED_RST 17

#define GPS_RX 1  // ESP RX ← GPS TX (per GEMINI.md: GPS TX -> ESP RX: GPIO 1)
#define GPS_TX 2  // ESP TX → GPS RX (per GEMINI.md: GPS RX -> ESP TX: GPIO 2)

// MicroSD Card SPI Pins
#define SD_CS 10
#define SD_MOSI 11
#define SD_SCK 12
#define SD_MISO 13

// SAMPLING RATE
const int SAMPLE_RATE_HZ = 50;
const unsigned long SAMPLE_PERIOD_MICROS =
    1000000 / SAMPLE_RATE_HZ; // 20000 us (20 ms)

// SENSOR SCALING CONSTANTS (Pololu MinIMU-9 v5 defaults)
const float GYRO_SENSITIVITY = 0.00875f;   // ±245 dps
const float ACCEL_SENSITIVITY = 0.000061f; // ±2g
const float MAG_SENSITIVITY = 0.000146f;   // ±4 gauss

// SD write buffer: flush every N lines or every 1 second
const int SD_BUFFER_LINES = 50; // ~1 second at 50Hz

/* =========================================================================
 * GLOBAL OBJECTS & STATE VARIABLES
 * ========================================================================= */
LSM6 imu;
LIS3MDL mag;
Madgwick filter;
TinyGPSPlus gps;
SPIClass sdSPI(FSPI);

unsigned long lastMicros;
unsigned long lastGPSUpdate = 0;

float gx_bias = 0, gy_bias = 0, gz_bias = 0;
bool imu_available = false;
bool mag_available = false;

char udpBuffer[256]; // Pre-allocated buffer for string formatting

// --- MicroSD State ---
bool sd_available = false;
bool sd_recording = false;
File sd_file;
int sd_session_number = 0;
char sd_filename[32];

// SD write buffer
String sd_write_buffer = "";
int sd_buffer_count = 0;
unsigned long sd_last_flush = 0;

// GPS cached values (for CSV at 50Hz, GPS updates at 1Hz)
double cached_lat = 0.0, cached_lng = 0.0;
double cached_alt = 0.0, cached_speed = 0.0;
uint32_t cached_sats = 0;

// UDP receive buffer for commands
char udpRxBuffer[64];

/* =========================================================================
 * BUTTONS & WIFI STATE
 * ========================================================================= */
bool wifi_enabled = true;

struct ButtonState {
  uint8_t pin;
  bool lastState;
  bool currentState;
  unsigned long lastDebounceTime;

  ButtonState(uint8_t p)
      : pin(p), lastState(LOW), currentState(LOW), lastDebounceTime(0) {}
};

ButtonState btnWifi(BTN_WIFI);
ButtonState btnRec(BTN_REC);
ButtonState btnRst(BTN_RST);

const unsigned long debounceDelay = 20;

bool readButton(ButtonState &btn) {
  int reading = digitalRead(btn.pin);
  if (reading != btn.lastState) {
    btn.lastDebounceTime = millis();
  }

  bool pressed = false;
  if ((millis() - btn.lastDebounceTime) > debounceDelay) {
    if (reading != btn.currentState) {
      btn.currentState = reading;
      if (btn.currentState ==
          HIGH) { // Active HIGH: button connects pin to VCC when pressed
        pressed = true;
      }
    }
  }
  btn.lastState = reading;
  return pressed;
}

/* =========================================================================
 * HELPER FUNCTION: Send UDP Broadcast
 * ========================================================================= */
void sendUDP(const char *message) {
  if (!wifi_enabled)
    return;
  // We broadcast the data to the target port
  udp.beginPacket(targetIP, targetPort);
  udp.print(message);
  udp.endPacket();
}

/* =========================================================================
 * MICROSD FUNCTIONS
 * ========================================================================= */

/**
 * Scan the SD card root to find the highest existing session_NNNN.csv
 * and return the next available session number.
 */
int findNextSessionNumber() {
  int maxFound = 0;
  File root = SD.open("/");
  if (!root)
    return 1;

  File entry;
  while ((entry = root.openNextFile())) {
    String name = entry.name();
    // Match pattern: session_NNNN.csv
    if (name.startsWith("session_") && name.endsWith(".csv")) {
      // Extract the number between "session_" and ".csv"
      String numStr = name.substring(8, name.length() - 4);
      int num = numStr.toInt();
      if (num > maxFound) {
        maxFound = num;
      }
    }
    entry.close();
  }
  root.close();
  return maxFound + 1;
}

/**
 * Initialize the SD card and detect its presence.
 * Returns true if SD card is available.
 */
bool initSDCard() {
  Serial.println("STATUS:INIT_SD...");

  static bool sdPrevInit = false;
  if (sdPrevInit) {
    SD.end();
    sdSPI.end();
    delay(50);
  }

  // CS must be OUTPUT HIGH before SPI starts — required by SD card SPI spec.
  // The SD library manages CS via GPIO; do NOT pass CS to sdSPI.begin() or
  // the hardware SS line will fight the SD library's GPIO toggling.
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  pinMode(SD_MISO, INPUT_PULLUP); // weak pull-up on MISO helps detect card absence

  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, -1); // -1 = no hardware SS
  delay(100); // SD cards need ~74 slow clock cycles + settling time after power-up

  sdPrevInit = true;

  // SD spec mandates slow clock during init. Try frequencies slowest-first.
  // Between retries: clock 80 dummy bytes with CS HIGH to drain any pending
  // card response and release the busy state (MISO held LOW).
  const uint32_t freqs[] = {400000, 1000000, 4000000};
  uint32_t initFreq = 0;
  for (int i = 0; i < 3 && !initFreq; i++) {
    // Flush card state before each attempt
    sdSPI.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
    digitalWrite(SD_CS, HIGH);
    for (int j = 0; j < 80; j++) sdSPI.transfer(0xFF);
    sdSPI.endTransaction();
    delay(100);

    if (SD.begin(SD_CS, sdSPI, freqs[i])) {
      initFreq = freqs[i];
    }
  }

  if (!initFreq) {
    Serial.println("STATUS:SD_NOT_FOUND");
    sendUDP("STATUS:SD_NOT_FOUND");
    return false;
  }

  uint8_t cardType = SD.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("STATUS:SD_NO_CARD");
    sendUDP("STATUS:SD_NO_CARD");
    return false;
  }

  const char *typeStr = "UNKNOWN";
  if (cardType == CARD_MMC)
    typeStr = "MMC";
  else if (cardType == CARD_SD)
    typeStr = "SD";
  else if (cardType == CARD_SDHC)
    typeStr = "SDHC";

  uint64_t cardSize = SD.cardSize() / (1024 * 1024); // MB

  Serial.printf("STATUS:SD_OK:%s_%lluMB@%lukHz\n", typeStr, cardSize, initFreq / 1000);
  snprintf(udpBuffer, sizeof(udpBuffer), "STATUS:SD_OK:%s_%lluMB@%lukHz", typeStr,
           cardSize, initFreq / 1000);
  sendUDP(udpBuffer);

  // Find next session number
  sd_session_number = findNextSessionNumber();
  Serial.printf("STATUS:SD_NEXT_SESSION:%04d\n", sd_session_number);

  return true;
}

/**
 * Start recording a new CSV session on the SD card.
 */
bool startSDRecording() {
  if (!sd_available) {
    Serial.println("STATUS:SD_REC_ERROR:NO_SD");
    sendUDP("STATUS:SD_REC_ERROR:NO_SD");
    return false;
  }

  snprintf(sd_filename, sizeof(sd_filename), "/session_%04d.csv",
           sd_session_number);

  sd_file = SD.open(sd_filename, FILE_WRITE);
  if (!sd_file) {
    Serial.printf("STATUS:SD_REC_ERROR:CANT_OPEN:%s\n", sd_filename);
    snprintf(udpBuffer, sizeof(udpBuffer), "STATUS:SD_REC_ERROR:CANT_OPEN:%s",
             sd_filename);
    sendUDP(udpBuffer);
    return false;
  }

  // Write CSV header
  sd_file.println("timestamp_ms,roll,pitch,yaw,ax,ay,az,gx,gy,gz,mx,my,mz,gps_"
                  "lat,gps_lng,gps_alt,gps_speed,gps_sats");
  sd_file.flush();

  sd_recording = true;
  sd_buffer_count = 0;
  sd_write_buffer = "";
  sd_write_buffer.reserve(4096); // Pre-allocate buffer
  sd_last_flush = millis();

  Serial.printf("STATUS:SD_REC_STARTED:%s\n",
                sd_filename + 1); // skip leading /
  snprintf(udpBuffer, sizeof(udpBuffer), "STATUS:SD_REC_STARTED:%s",
           sd_filename + 1);
  sendUDP(udpBuffer);

  return true;
}

/**
 * Stop the current SD recording session.
 */
void stopSDRecording() {
  if (!sd_recording)
    return;

  // Flush remaining buffer
  if (sd_write_buffer.length() > 0) {
    sd_file.print(sd_write_buffer);
    sd_write_buffer = "";
    sd_buffer_count = 0;
  }

  sd_file.flush();
  sd_file.close();
  sd_recording = false;

  Serial.printf("STATUS:SD_REC_STOPPED:%s\n", sd_filename + 1);
  snprintf(udpBuffer, sizeof(udpBuffer), "STATUS:SD_REC_STOPPED:%s",
           sd_filename + 1);
  sendUDP(udpBuffer);

  // Increment for next session
  sd_session_number++;
}

/**
 * Write a line of sensor data to the SD buffer.
 * Data is flushed to the SD card periodically.
 */
void writeSDData(float r, float p, float y, float ax, float ay, float az,
                 float gx, float gy, float gz, float mx, float my, float mz) {
  if (!sd_recording)
    return;

  char line[256];
  snprintf(line, sizeof(line),
           "%lu,%.2f,%.2f,%.2f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%."
           "6f,%.6f,%.1f,%.1f,%d\n",
           millis(), r, p, y, ax, ay, az, gx, gy, gz, mx, my, mz, cached_lat,
           cached_lng, cached_alt, cached_speed, cached_sats);

  sd_write_buffer += line;
  sd_buffer_count++;

  // Flush buffer every SD_BUFFER_LINES lines or every 1 second
  if (sd_buffer_count >= SD_BUFFER_LINES ||
      (millis() - sd_last_flush) >= 1000) {
    size_t toWrite = sd_write_buffer.length();
    size_t written = sd_file.print(sd_write_buffer);
    if (written < toWrite) {
      sd_file.close();
      sd_recording = false;
      sd_write_buffer = "";
      sd_buffer_count = 0;
      Serial.println("STATUS:SD_REC_ERROR:WRITE_FAIL");
      sendUDP("STATUS:SD_REC_ERROR:WRITE_FAIL");
      return;
    }
    sd_file.flush();
    sd_write_buffer = "";
    sd_buffer_count = 0;
    sd_last_flush = millis();
  }
}

/**
 * List all files on the SD card root and send them via UDP.
 */
void listSDFiles() {
  if (!sd_available)
    return;

  File root = SD.open("/");
  if (!root)
    return;

  sendUDP("STATUS:SD_LIST_START");

  File entry;
  while ((entry = root.openNextFile())) {
    if (!entry.isDirectory()) {
      snprintf(udpBuffer, sizeof(udpBuffer), "FILE:%s|%llu", entry.name(),
               entry.size());
      sendUDP(udpBuffer);
    }
    entry.close();
  }
  root.close();

  sendUDP("STATUS:SD_LIST_END");
}

/**
 * Delete a file from the SD card.
 */
void deleteSDFile(const char *filename) {
  if (!sd_available)
    return;

  // Ensure filename starts with /
  char path[64];
  if (filename[0] != '/') {
    snprintf(path, sizeof(path), "/%s", filename);
  } else {
    snprintf(path, sizeof(path), "%s", filename);
  }

  if (SD.remove(path)) {
    Serial.printf("STATUS:SD_DELETE_OK:%s\n", filename);
    snprintf(udpBuffer, sizeof(udpBuffer), "STATUS:SD_DELETE_OK:%s", filename);
    sendUDP(udpBuffer);
    listSDFiles(); // Refresh list
  } else {
    Serial.printf("STATUS:SD_DELETE_ERROR:%s\n", filename);
    snprintf(udpBuffer, sizeof(udpBuffer), "STATUS:SD_DELETE_ERROR:%s",
             filename);
    sendUDP(udpBuffer);
  }
}

/**
 * Rename a file on the SD card.
 */
void renameSDFile(const char *oldName, const char *newName) {
  if (!sd_available)
    return;

  char oldPath[64], newPath[64];
  snprintf(oldPath, sizeof(oldPath), oldName[0] == '/' ? "%s" : "/%s", oldName);
  snprintf(newPath, sizeof(newPath), newName[0] == '/' ? "%s" : "/%s", newName);

  if (SD.rename(oldPath, newPath)) {
    Serial.printf("STATUS:SD_RENAME_OK:%s->%s\n", oldName, newName);
    snprintf(udpBuffer, sizeof(udpBuffer), "STATUS:SD_RENAME_OK:%s|%s", oldName,
             newName);
    sendUDP(udpBuffer);
    listSDFiles(); // Refresh list
  } else {
    Serial.printf("STATUS:SD_RENAME_ERROR:%s\n", oldName);
    snprintf(udpBuffer, sizeof(udpBuffer), "STATUS:SD_RENAME_ERROR:%s",
             oldName);
    sendUDP(udpBuffer);
  }
}

/**
 * Check for incoming UDP commands (START/STOP recording, File management).
 */
void checkUDPCommands() {
  if (!wifi_enabled)
    return;
  int packetSize = udp.parsePacket();
  if (packetSize > 0) {
    int len = udp.read(udpRxBuffer, sizeof(udpRxBuffer) - 1);
    if (len > 0) {
      udpRxBuffer[len] = '\0';
      String cmd = String(udpRxBuffer);
      cmd.trim();

      if (cmd == "START_RECORDING") {
        Serial.println("[CMD] Received START_RECORDING");
        if (!sd_recording) {
          startSDRecording();
        }
      } else if (cmd == "STOP_RECORDING") {
        Serial.println("[CMD] Received STOP_RECORDING");
        if (sd_recording) {
          stopSDRecording();
        }
      } else if (cmd == "LS") {
        Serial.println("[CMD] Received LS");
        listSDFiles();
      } else if (cmd == "INIT_SD") {
        Serial.println("[CMD] Received INIT_SD");
        sd_available = initSDCard();
      } else if (cmd.startsWith("DEL:")) {
        String filename = cmd.substring(4);
        Serial.printf("[CMD] Received DEL: %s\n", filename.c_str());
        deleteSDFile(filename.c_str());
      } else if (cmd.startsWith("REN:")) {
        int commaIndex = cmd.indexOf(',', 4);
        if (commaIndex != -1) {
          String oldName = cmd.substring(4, commaIndex);
          String newName = cmd.substring(commaIndex + 1);
          Serial.printf("[CMD] Received REN: %s to %s\n", oldName.c_str(),
                        newName.c_str());
          renameSDFile(oldName.c_str(), newName.c_str());
        }
      }
    }
  }
}

/* =========================================================================
 * SETUP SYSTEM
 * ========================================================================= */
void setup() {
  // --- MICROCONTROLLER INIT ---
  // Modello: ESP32-S3 (DevKitC-1)
  Serial.begin(115200);

  // Drive CS HIGH immediately — SD card must not see CS=LOW during boot
  // while the pin is in its default floating-input state.
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);

  // LED di stato
  pinMode(LED_1, OUTPUT); // LED_1: Stato IMU
  pinMode(LED_2, OUTPUT); // LED_2: Stato Magnetometro

  // Pulsanti e LED (PCB ONDA v2.5)
  pinMode(LED_WIFI, OUTPUT);
  pinMode(LED_REC, OUTPUT);
  pinMode(LED_RST, OUTPUT);

  pinMode(BTN_WIFI, INPUT_PULLDOWN);
  pinMode(BTN_REC, INPUT_PULLDOWN);
  pinMode(BTN_RST, INPUT_PULLDOWN);

  // --- GPS EARLY INIT ---
  // Modello: Modulo GPS NEO-6M (comunicazione UART)
  Serial1.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
  Serial.println("STATUS:GPS_SERIAL_STARTED");

  // USB CDC initialization delay
  for (int i = 0; i < 10; i++) {
    delay(200);
    digitalWrite(LED_1, i % 2);
  }
  digitalWrite(LED_1, LOW);   // Start LOW so GPS blink starts from known OFF state
  digitalWrite(LED_2, HIGH);

  // --- WIFI INITIALIZATION (HOTSPOT) ---
  // Hardware: Modulo Wi-Fi integrato nell'ESP32-S3
  Serial.println("STATUS:STARTING_WIFI_HOTSPOT...");

  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid, password);

  delay(2000); // Allow 3.3V rail to stabilize after WiFi AP startup before SD init

  digitalWrite(LED_WIFI, HIGH); // WiFi acceso all'avvio
  digitalWrite(LED_REC, LOW);   // Nessuna registrazione all'avvio
  digitalWrite(LED_RST, LOW);   // LED Restart spento

  Serial.println("STATUS:HOTSPOT_READY");
  Serial.print("1. Connect your PC to Wi-Fi: ");
  Serial.println(ssid);
  Serial.print("2. Password is: ");
  Serial.println(password);
  Serial.print("3. ESP32 Router IP is: ");
  Serial.println(WiFi.softAPIP());

  // --- UDP INITIALIZATION ---
  udp.begin(targetPort); // Also listen on this port for incoming commands

  // --- MICROSD INITIALIZATION ---
  // Modello: Modulo lettore schede microSD (SKU: CE482333) - Comunicazione SPI
  sd_available = initSDCard();

  // --- HTTP SERVER FOR SD DOWNLOAD ---
  server.on("/download", HTTP_GET, []() {
    if (!sd_available) {
      server.send(503, "text/plain", "SD Card not available");
      return;
    }
    if (!server.hasArg("file")) {
      server.send(400, "text/plain", "Missing 'file' parameter");
      return;
    }
    String filename = server.arg("file");
    if (!filename.startsWith("/"))
      filename = "/" + filename;

    if (!SD.exists(filename)) {
      server.send(404, "text/plain", "File not found");
      return;
    }

    File downloadFile = SD.open(filename, FILE_READ);
    if (!downloadFile) {
      server.send(500, "text/plain", "Could not open file");
      return;
    }

    server.sendHeader("Content-Disposition",
                      "attachment; filename=\"" + filename.substring(1) + "\"");
    server.streamFile(downloadFile, "application/octet-stream");
    downloadFile.close();
  });
  server.begin();

  // --- SENSOR INITIALIZATION ---
  // Modello: Pololu MinIMU-9 v5
  // Sensori inclusi: LSM6DS33 (Accelerometro/Giroscopio) + LIS3MDL (Magnetometro)
  // Comunicazione: Bus I2C
  Serial.println("STATUS:INIT_I2C...");
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
    Serial.println("STATUS:WARN:LIS3MDL_NOT_FOUND_FALLBACK_TO_6DOF");
    digitalWrite(LED_2, LOW);
    mag_available = false;
  } else {
    Serial.println("STATUS:MAG_OK_USING_9DOF");
    mag.enableDefault();
    digitalWrite(LED_2, HIGH);
    mag_available = true;
  }

  filter.begin(SAMPLE_RATE_HZ);

  // --- CALIBRATION ---
  Serial.println("STATUS:CALIBRATING_GYRO... KEEP STILL");
  long count = 0, sumX = 0, sumY = 0, sumZ = 0;
  unsigned long startCalib = millis();

  while (millis() - startCalib < 3000) {
    if (imu_available)
      imu.read();
    if (imu_available && imu.last_status == 0) {
      sumX += imu.g.x;
      sumY += imu.g.y;
      sumZ += imu.g.z;
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
 * MAIN LOOP
 * ========================================================================= */
void loop() {

  // 0. CHECK BUTTONS
  if (readButton(btnWifi)) {
    wifi_enabled = !wifi_enabled;
    if (wifi_enabled) {
      Serial.println("STATUS:WIFI_ENABLED");
      WiFi.mode(WIFI_AP);
      WiFi.softAP(ssid, password);
      udp.begin(targetPort);
      digitalWrite(LED_WIFI, HIGH);
    } else {
      Serial.println("STATUS:WIFI_DISABLED");
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_OFF);
      digitalWrite(LED_WIFI, LOW);
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
    Serial.println("STATUS:RESTARTING...");
    digitalWrite(LED_RST, HIGH);
    delay(500);
    ESP.restart();
  }

  // Aggiorna lo stato del LED di registrazione (in caso venga avviata via UDP)
  digitalWrite(LED_REC, sd_recording ? HIGH : LOW);

  // 1. CHECK FOR INCOMING UDP COMMANDS (START/STOP recording)
  checkUDPCommands();

  // 1.5 HANDLE HTTP CLIENTS
  server.handleClient();

  // 2. ASYNCHRONOUS GPS (Constantly reading UART buffer)
  while (Serial1.available() > 0) {
    char c = Serial1.read();
    gps.encode(c);
    // Blink LED_1 briefly when data is received
    static unsigned long lastBlink = 0;
    if (millis() - lastBlink > 10) {
      digitalWrite(LED_1, !digitalRead(LED_1));
      lastBlink = millis();
    }
  }

  // 3. SENSOR FUSION & DATA OUTPUT (Strictly 50 Hz)
  if (micros() - lastMicros >= SAMPLE_PERIOD_MICROS) {
    lastMicros += SAMPLE_PERIOD_MICROS;

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

      float currentRoll = filter.getRoll();
      float currentPitch = filter.getPitch();
      float currentYaw = filter.getYaw();

      // Format string per IMU: DATA:roll,pitch,yaw
      // Questo formato è richiesto da visualizer.html per muovere il cubo
      Serial.printf("DATA:%.2f,%.2f,%.2f\n", currentRoll, currentPitch,
                    currentYaw);

      // Invio anche via UDP (Wi-Fi) come backup
      snprintf(udpBuffer, sizeof(udpBuffer), "IMU,%.2f,%.2f,%.2f", currentRoll,
               currentPitch, currentYaw);
      sendUDP(udpBuffer);

      // Write to SD card if recording
      writeSDData(currentRoll, currentPitch, currentYaw, ax, ay, az, gx, gy, gz,
                  mx, my, mz);
    }
  }

  // 4. GPS OUTPUT (Every 1 second for stability)
  if (millis() - lastGPSUpdate >= 1000) {
    lastGPSUpdate = millis();

    if (gps.location.isValid()) {
      // Update cached values for SD recording
      cached_lat = gps.location.lat();
      cached_lng = gps.location.lng();
      cached_alt = gps.altitude.meters();
      cached_speed = gps.speed.kmph();
      cached_sats = gps.satellites.value();

      // Formato per visualizer_gps.html e Serial Debug
      Serial.printf("GPS:%.6f,%.6f,%.1f,%.1f,%u\n", cached_lat, cached_lng,
                    cached_alt, cached_speed, cached_sats);

      // Formato per visualizer.html (tramite serverw.py) -> GPS,sats,lat,lng
      snprintf(udpBuffer, sizeof(udpBuffer), "GPS,%u,%.6f,%.6f", cached_sats,
               cached_lat, cached_lng);
    } else {
      cached_sats = gps.satellites.value();

      // Se non c'è il fix, mandiamo i dati minimi per il debug sulla dashboard
      Serial.printf("GPS:0,0,0,0,%u (NO FIX)\n", cached_sats);

      // Formato: GPS,sats,lat,lng
      snprintf(udpBuffer, sizeof(udpBuffer), "GPS,%u,NO_FIX,NO_FIX",
               cached_sats);
    }

    // Mostriamo anche i caratteri processati nel log seriale per debug hardware
    Serial.printf("GPS_DEBUG: Chars=%u, FailedChecksum=%u\n",
                  gps.charsProcessed(), gps.failedChecksum());

    sendUDP(udpBuffer);

    // Invia diagnostica GPS dettagliata via UDP per debug senza serial monitor
    snprintf(udpBuffer, sizeof(udpBuffer),
             "STATUS:GPS_DBG:chars=%u,fix=%d,sats=%u,age=%u,chkfail=%u",
             gps.charsProcessed(),
             gps.location.isValid() ? 1 : 0,
             gps.satellites.value(),
             gps.location.age(),
             gps.failedChecksum());
    sendUDP(udpBuffer);
  }

  // 5. PERIODIC SYSTEM STATUS BROADCAST (Every 3 seconds)
  // Ensure the web visualizer sees the SD status even if connected later
  static unsigned long lastStatusBroadcast = 0;
  if (millis() - lastStatusBroadcast >= 3000) {
    lastStatusBroadcast = millis();

    if (sd_available && !sd_recording) {
      // Send SD status to update the UI
      snprintf(udpBuffer, sizeof(udpBuffer), "STATUS:SD_OK:In_Attesa");
      sendUDP(udpBuffer);

      // Also send the next session number for logging
      snprintf(udpBuffer, sizeof(udpBuffer), "STATUS:SD_NEXT_SESSION:%04d",
               sd_session_number);
      sendUDP(udpBuffer);
    } else if (!sd_available) {
      sendUDP("STATUS:SD_NOT_FOUND");
    }
  }
}