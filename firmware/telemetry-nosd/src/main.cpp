#if __has_include("secrets.h")
#include "secrets.h"
#else
#define SFT_WIFI_PASSWORD "change-me-now"
#endif

#include <Arduino.h>
#include <LIS3MDL.h>
#include <LSM6.h>
#include <MadgwickAHRS.h>
#include <LittleFS.h>
#include <TinyGPS++.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Wire.h>

/* =========================================================================
 * NETWORK CONFIGURATION (ACCESS POINT MODE)
 * ========================================================================= */
const char *ssid = "SFT_telemetry";
const char *password = SFT_WIFI_PASSWORD; 

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
#define BTN_WIFI 5
#define LED_WIFI 4
#define BTN_REC 7
#define LED_REC 6
#define BTN_RST 18
#define LED_RST 17

#define GPS_RX 1  
#define GPS_TX 2  

// SAMPLING RATE
const int SAMPLE_RATE_HZ = 50;
const unsigned long SAMPLE_PERIOD_MICROS = 1000000 / SAMPLE_RATE_HZ; 

// SENSOR SCALING CONSTANTS
const float GYRO_SENSITIVITY = 0.00875f;   
const float ACCEL_SENSITIVITY = 0.000061f; 
const float MAG_SENSITIVITY = 0.000146f;   

// FS write buffer: flush every N lines or every 1 second
const int FS_BUFFER_LINES = 50; 

/* =========================================================================
 * GLOBAL OBJECTS & STATE VARIABLES
 * ========================================================================= */
LSM6 imu;
LIS3MDL mag;
Madgwick filter;
TinyGPSPlus gps;

unsigned long lastMicros;
unsigned long lastGPSUpdate = 0;
unsigned long lastAlive = 0;

float gx_bias = 0, gy_bias = 0, gz_bias = 0;
bool imu_available = false;
bool mag_available = false;

char udpBuffer[256]; 

// --- LittleFS State ---
bool fs_available = false;
bool fs_recording = false;
File fs_file;
int fs_session_number = 0;
char fs_filename[32];

// FS write buffer
String fs_write_buffer = "";
int fs_buffer_count = 0;
unsigned long fs_last_flush = 0;

// GPS cached values
double cached_lat = 0.0, cached_lng = 0.0;
double cached_alt = 0.0, cached_speed = 0.0;
int cached_sats = 0;

// UDP receive buffer
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
      if (btn.currentState == HIGH) { 
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
  if (!wifi_enabled) return;
  udp.beginPacket(targetIP, targetPort);
  udp.print(message);
  udp.endPacket();
}

/* =========================================================================
 * LITTLEFS FUNCTIONS
 * ========================================================================= */

int findNextSessionNumber() {
  int maxFound = 0;
  File root = LittleFS.open("/");
  if (!root) return 1;

  File entry;
  while ((entry = root.openNextFile())) {
    String name = entry.name();
    // name could be "session_NNNN.csv" or "/session_NNNN.csv" depending on LittleFS version
    if(name.startsWith("/")) name = name.substring(1);
    
    if (name.startsWith("session_") && name.endsWith(".csv")) {
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

bool initFS() {
  Serial.println("STATUS:INIT_FS...");
  
  if (!LittleFS.begin(true)) {
    Serial.println("STATUS:FS_MOUNT_FAILED");
    sendUDP("STATUS:FS_MOUNT_FAILED");
    return false;
  }

  uint32_t totalBytes = LittleFS.totalBytes();
  uint32_t usedBytes = LittleFS.usedBytes();
  uint32_t freeBytes = totalBytes - usedBytes;

  Serial.printf("STATUS:FS_OK:Total=%u,Used=%u,Free=%u\n", totalBytes, usedBytes, freeBytes);
  snprintf(udpBuffer, sizeof(udpBuffer), "STATUS:FS_OK:Free_%uMB", freeBytes / (1024 * 1024));
  sendUDP(udpBuffer);

  fs_session_number = findNextSessionNumber();
  Serial.printf("STATUS:FS_NEXT_SESSION:%04d\n", fs_session_number);

  return true;
}

bool startFSRecording() {
  if (!fs_available) {
    Serial.println("STATUS:FS_REC_ERROR:NO_FS");
    sendUDP("STATUS:FS_REC_ERROR:NO_FS");
    return false;
  }

  snprintf(fs_filename, sizeof(fs_filename), "/session_%04d.csv", fs_session_number);

  fs_file = LittleFS.open(fs_filename, FILE_WRITE);
  if (!fs_file) {
    Serial.printf("STATUS:FS_REC_ERROR:CANT_OPEN:%s\n", fs_filename);
    snprintf(udpBuffer, sizeof(udpBuffer), "STATUS:FS_REC_ERROR:CANT_OPEN:%s", fs_filename);
    sendUDP(udpBuffer);
    return false;
  }

  fs_file.println("timestamp_ms,roll,pitch,yaw,ax,ay,az,gx,gy,gz,mx,my,mz,gps_lat,gps_lng,gps_alt,gps_speed,gps_sats");
  fs_file.flush();

  fs_recording = true;
  fs_buffer_count = 0;
  fs_write_buffer = "";
  fs_write_buffer.reserve(4096); 
  fs_last_flush = millis();

  Serial.printf("STATUS:FS_REC_STARTED:%s\n", fs_filename + 1); 
  snprintf(udpBuffer, sizeof(udpBuffer), "STATUS:FS_REC_STARTED:%s", fs_filename + 1);
  sendUDP(udpBuffer);

  return true;
}

void stopFSRecording() {
  if (!fs_recording) return;

  if (fs_write_buffer.length() > 0) {
    fs_file.print(fs_write_buffer);
    fs_write_buffer = "";
    fs_buffer_count = 0;
  }

  fs_file.flush();
  fs_file.close();
  fs_recording = false;

  Serial.printf("STATUS:FS_REC_STOPPED:%s\n", fs_filename + 1);
  snprintf(udpBuffer, sizeof(udpBuffer), "STATUS:FS_REC_STOPPED:%s", fs_filename + 1);
  sendUDP(udpBuffer);

  fs_session_number++;
}

void writeFSData(float r, float p, float y, float ax, float ay, float az,
                 float gx, float gy, float gz, float mx, float my, float mz) {
  if (!fs_recording) return;

  char line[256];
  snprintf(line, sizeof(line),
           "%lu,%.2f,%.2f,%.2f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.6f,%.6f,%.1f,%.1f,%d\n",
           millis(), r, p, y, ax, ay, az, gx, gy, gz, mx, my, mz, cached_lat,
           cached_lng, cached_alt, cached_speed, cached_sats);

  fs_write_buffer += line;
  fs_buffer_count++;

  if (fs_buffer_count >= FS_BUFFER_LINES || (millis() - fs_last_flush) >= 1000) {
    fs_file.print(fs_write_buffer);
    fs_file.flush();
    fs_write_buffer = "";
    fs_buffer_count = 0;
    fs_last_flush = millis();
  }
}

void listFSFiles() {
  if (!fs_available) return;

  File root = LittleFS.open("/");
  if (!root) return;

  sendUDP("STATUS:FS_LIST_START");

  File entry;
  while ((entry = root.openNextFile())) {
    if (!entry.isDirectory()) {
      String name = entry.name();
      if(name.startsWith("/")) name = name.substring(1);
      snprintf(udpBuffer, sizeof(udpBuffer), "FILE:%s|%llu", name.c_str(), entry.size());
      sendUDP(udpBuffer);
    }
    entry.close();
  }
  root.close();

  sendUDP("STATUS:FS_LIST_END");
}

void deleteFSFile(const char *filename) {
  if (!fs_available) return;

  char path[64];
  if (filename[0] != '/') snprintf(path, sizeof(path), "/%s", filename);
  else strncpy(path, filename, sizeof(path));

  if (LittleFS.remove(path)) {
    Serial.printf("STATUS:FS_DELETE_OK:%s\n", filename);
    snprintf(udpBuffer, sizeof(udpBuffer), "STATUS:FS_DELETE_OK:%s", filename);
    sendUDP(udpBuffer);
    listFSFiles();
  } else {
    Serial.printf("STATUS:FS_DELETE_ERROR:%s\n", filename);
    snprintf(udpBuffer, sizeof(udpBuffer), "STATUS:FS_DELETE_ERROR:%s", filename);
    sendUDP(udpBuffer);
  }
}

void renameFSFile(const char *oldName, const char *newName) {
  if (!fs_available) return;

  char oldPath[64], newPath[64];
  snprintf(oldPath, sizeof(oldPath), oldName[0] == '/' ? "%s" : "/%s", oldName);
  snprintf(newPath, sizeof(newPath), newName[0] == '/' ? "%s" : "/%s", newName);

  if (LittleFS.rename(oldPath, newPath)) {
    Serial.printf("STATUS:FS_RENAME_OK:%s->%s\n", oldName, newName);
    snprintf(udpBuffer, sizeof(udpBuffer), "STATUS:FS_RENAME_OK:%s|%s", oldName, newName);
    sendUDP(udpBuffer);
    listFSFiles(); 
  } else {
    Serial.printf("STATUS:FS_RENAME_ERROR:%s\n", oldName);
    snprintf(udpBuffer, sizeof(udpBuffer), "STATUS:FS_RENAME_ERROR:%s", oldName);
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

      if (cmd == "START_RECORDING") {
        Serial.println("[CMD] Received START_RECORDING");
        if (!fs_recording) startFSRecording();
      } else if (cmd == "STOP_RECORDING") {
        Serial.println("[CMD] Received STOP_RECORDING");
        if (fs_recording) stopFSRecording();
      } else if (cmd == "LS") {
        Serial.println("[CMD] Received LS");
        listFSFiles();
      } else if (cmd == "INIT_SD" || cmd == "INIT_FS") {
        Serial.println("[CMD] Received INIT_FS");
        fs_available = initFS();
      } else if (cmd == "FORMAT_FS") {
        Serial.println("[CMD] Received FORMAT_FS");
        if (fs_recording) stopFSRecording();
        sendUDP("STATUS:FS_FORMATTING...");
        if (LittleFS.format()) {
           Serial.println("STATUS:FS_FORMAT_OK");
           sendUDP("STATUS:FS_FORMAT_OK");
           fs_session_number = 1;
           listFSFiles();
        } else {
           Serial.println("STATUS:FS_FORMAT_ERROR");
           sendUDP("STATUS:FS_FORMAT_ERROR");
        }
      } else if (cmd.startsWith("DEL:")) {
        String filename = cmd.substring(4);
        deleteFSFile(filename.c_str());
      } else if (cmd.startsWith("REN:")) {
        int commaIndex = cmd.indexOf(',', 4);
        if (commaIndex != -1) {
          String oldName = cmd.substring(4, commaIndex);
          String newName = cmd.substring(commaIndex + 1);
          renameFSFile(oldName.c_str(), newName.c_str());
        }
      }
    }
  }
}

/* =========================================================================
 * SETUP SYSTEM
 * ========================================================================= */
void setup() {
  Serial.begin(115200);
  
  pinMode(LED_1, OUTPUT); 
  pinMode(LED_2, OUTPUT); 
  pinMode(LED_WIFI, OUTPUT);
  pinMode(LED_REC, OUTPUT);
  pinMode(LED_RST, OUTPUT);

  pinMode(BTN_WIFI, INPUT_PULLDOWN);
  pinMode(BTN_REC, INPUT_PULLDOWN);
  pinMode(BTN_RST, INPUT_PULLDOWN);

  Serial1.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
  Serial.println("STATUS:GPS_SERIAL_STARTED");

  for (int i = 0; i < 10; i++) {
    delay(200);
    digitalWrite(LED_1, i % 2);
  }
  digitalWrite(LED_1, HIGH);
  digitalWrite(LED_2, HIGH);

  Serial.println("STATUS:STARTING_WIFI_HOTSPOT...");
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid, password);
  delay(1000); 

  digitalWrite(LED_WIFI, HIGH); 
  digitalWrite(LED_REC, LOW);   
  digitalWrite(LED_RST, LOW);   

  Serial.println("STATUS:HOTSPOT_READY");
  Serial.print("1. Connect your PC to Wi-Fi: ");
  Serial.println(ssid);
  Serial.print("2. Password is: ");
  Serial.println(password);
  Serial.print("3. ESP32 Router IP is: ");
  Serial.println(WiFi.softAPIP());

  udp.begin(targetPort); 

  // --- LITTLEFS INITIALIZATION ---
  fs_available = initFS();

  // --- HTTP SERVER FOR FS DOWNLOAD ---
  server.on("/download", HTTP_GET, []() {
    if (!fs_available) {
      server.send(503, "text/plain", "FS not available");
      return;
    }
    if (!server.hasArg("file")) {
      server.send(400, "text/plain", "Missing 'file' parameter");
      return;
    }
    String filename = server.arg("file");
    if (!filename.startsWith("/")) filename = "/" + filename;

    if (!LittleFS.exists(filename)) {
      server.send(404, "text/plain", "File not found");
      return;
    }

    File downloadFile = LittleFS.open(filename, FILE_READ);
    if (!downloadFile) {
      server.send(500, "text/plain", "Could not open file");
      return;
    }

    server.sendHeader("Content-Disposition", "attachment; filename=\"" + filename.substring(1) + "\"");
    server.streamFile(downloadFile, "application/octet-stream");
    downloadFile.close();
  });
  server.begin();

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

  Serial.println("STATUS:CALIBRATING_GYRO... KEEP STILL");
  long count = 0, sumX = 0, sumY = 0, sumZ = 0;
  unsigned long startCalib = millis();

  while (millis() - startCalib < 3000) {
    if (imu_available) imu.read();
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
  if (readButton(btnWifi)) {
    wifi_enabled = !wifi_enabled;
    if (wifi_enabled) {
      Serial.println("STATUS:WIFI_ENABLED");
      WiFi.mode(WIFI_AP);
      WiFi.softAP(ssid, password);
      digitalWrite(LED_WIFI, HIGH);
    } else {
      Serial.println("STATUS:WIFI_DISABLED");
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_OFF);
      digitalWrite(LED_WIFI, LOW);
    }
  }

  if (readButton(btnRec)) {
    if (fs_recording) stopFSRecording();
    else startFSRecording();
  }

  if (readButton(btnRst)) {
    Serial.println("STATUS:RESTARTING...");
    digitalWrite(LED_RST, HIGH);
    delay(500);
    ESP.restart();
  }

  digitalWrite(LED_REC, fs_recording ? HIGH : LOW);

  checkUDPCommands();
  server.handleClient();

  while (Serial1.available() > 0) {
    char c = Serial1.read();
    gps.encode(c);
    static unsigned long lastBlink = 0;
    if (millis() - lastBlink > 10) {
      digitalWrite(LED_1, !digitalRead(LED_1));
      lastBlink = millis();
    }
  }

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

      float currentRoll = filter.getRoll();
      float currentPitch = filter.getPitch();
      float currentYaw = filter.getYaw();

      Serial.printf("DATA:%.2f,%.2f,%.2f\n", currentRoll, currentPitch, currentYaw);

      snprintf(udpBuffer, sizeof(udpBuffer), "IMU,%.2f,%.2f,%.2f", currentRoll, currentPitch, currentYaw);
      sendUDP(udpBuffer);

      writeFSData(currentRoll, currentPitch, currentYaw, ax, ay, az, gx, gy, gz, mx, my, mz);
    }
  }

  if (millis() - lastGPSUpdate >= 1000) {
    lastGPSUpdate = millis();

    if (gps.location.isValid()) {
      cached_lat = gps.location.lat();
      cached_lng = gps.location.lng();
      cached_alt = gps.altitude.meters();
      cached_speed = gps.speed.kmph();
      cached_sats = gps.satellites.value();

      Serial.printf("GPS:%.6f,%.6f,%.1f,%.1f,%d\n", cached_lat, cached_lng, cached_alt, cached_speed, cached_sats);
      snprintf(udpBuffer, sizeof(udpBuffer), "GPS,%d,%.6f,%.6f", cached_sats, cached_lat, cached_lng);
    } else {
      cached_sats = gps.satellites.value();
      Serial.printf("GPS:0,0,0,0,%d (NO FIX)\n", cached_sats);
      snprintf(udpBuffer, sizeof(udpBuffer), "GPS,%d,NO_FIX,NO_FIX", cached_sats);
    }

    sendUDP(udpBuffer);

    snprintf(udpBuffer, sizeof(udpBuffer),
             "STATUS:GPS_DBG:chars=%lu,fix=%d,sats=%d,age=%lu,chkfail=%lu",
             gps.charsProcessed(),
             gps.location.isValid() ? 1 : 0,
             gps.satellites.value(),
             gps.location.age(),
             gps.failedChecksum());
    sendUDP(udpBuffer);
  }

  static unsigned long lastStatusBroadcast = 0;
  if (millis() - lastStatusBroadcast >= 3000) {
    lastStatusBroadcast = millis();

    if (fs_available && !fs_recording) {
      snprintf(udpBuffer, sizeof(udpBuffer), "STATUS:FS_OK:In_Attesa");
      sendUDP(udpBuffer);
      snprintf(udpBuffer, sizeof(udpBuffer), "STATUS:FS_NEXT_SESSION:%04d", fs_session_number);
      sendUDP(udpBuffer);
    }
  }
}
