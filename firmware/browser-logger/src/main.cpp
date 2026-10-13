#if __has_include("secrets.h")
#include "secrets.h"
#else
#define SFT_WIFI_PASSWORD "change-me-now"
#endif

#include <Arduino.h>
#include <LIS3MDL.h>
#include <LSM6.h>
#include <LittleFS.h>
#include <MadgwickAHRS.h>
#include <TinyGPS++.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <WiFi.h>
#include <Wire.h>

/* =========================================================================
 * NETWORK CONFIGURATION (ACCESS POINT MODE)
 * ========================================================================= */
const char *ssid     = "SFT_telemetry";
const char *password = SFT_WIFI_PASSWORD;

WebServer        httpServer(80);
WebSocketsServer wsServer(81);

/* =========================================================================
 * HARDWARE CONFIGURATION & CONSTANTS
 * ========================================================================= */
#define I2C_SDA  8
#define I2C_SCL  9
#define LED_1      21   // IMU status
#define LED_2      45   // WebSocket client connected (repurposed from MAG status)
#define LED_CLIENT LED_2 // alias — lights up when a browser is connected

// PCB ONDA v2.5 buttons and LEDs
// S1 (Top) — Wi-Fi toggle
#define BTN_WIFI 5
#define LED_WIFI 4
// S3 (Bottom) — Restart
#define BTN_RST  18
#define LED_RST  17

#define GPS_RX   2
#define GPS_TX   1

// Sampling rate
const int SAMPLE_RATE_HZ           = 50;
const unsigned long SAMPLE_PERIOD_MICROS = 1000000 / SAMPLE_RATE_HZ; // 20 000 µs

// Sensor scaling (Pololu MinIMU-9 v5 defaults)
const float GYRO_SENSITIVITY  = 0.00875f;   // ±245 dps  → dps/LSB
const float ACCEL_SENSITIVITY = 0.000061f;  // ±2 g      → g/LSB
const float MAG_SENSITIVITY   = 0.000146f;  // ±4 gauss  → gauss/LSB

/* =========================================================================
 * GLOBAL OBJECTS & STATE
 * ========================================================================= */
LSM6      imu;
LIS3MDL   mag;
Madgwick  filter;
TinyGPSPlus gps;

unsigned long lastMicros    = 0;
unsigned long lastGPSUpdate = 0;

float gx_bias = 0, gy_bias = 0, gz_bias = 0;
bool  imu_available = false;
bool  mag_available = false;
bool  wifi_enabled  = true;

// Connected WebSocket clients counter (for LED feedback)
uint8_t ws_clients = 0;

// GPS cached values (updated @1 Hz, embedded in every 50 Hz data row)
double cached_lat   = 0.0, cached_lng   = 0.0;
double cached_alt   = 0.0, cached_speed = 0.0;
int    cached_sats  = 0;
bool   cached_fix   = false;

// Pre-allocated send buffer
char txBuf[256];

/* =========================================================================
 * BUTTON DEBOUNCING
 * ========================================================================= */
struct ButtonState {
  uint8_t  pin;
  bool     lastState, currentState;
  unsigned long lastDebounce;
  ButtonState(uint8_t p)
    : pin(p), lastState(LOW), currentState(LOW), lastDebounce(0) {}
};

ButtonState btnWifi(BTN_WIFI);
ButtonState btnRst(BTN_RST);

const unsigned long DEBOUNCE_MS = 20;

bool readButton(ButtonState &btn) {
  int  reading = digitalRead(btn.pin);
  if (reading != btn.lastState) btn.lastDebounce = millis();
  bool pressed = false;
  if ((millis() - btn.lastDebounce) > DEBOUNCE_MS) {
    if (reading != btn.currentState) {
      btn.currentState = reading;
      if (btn.currentState == HIGH) pressed = true;
    }
  }
  btn.lastState = reading;
  return pressed;
}

/* =========================================================================
 * WEBSOCKET EVENT HANDLER
 * ========================================================================= */
void onWsEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      ws_clients++;
      digitalWrite(LED_CLIENT, HIGH);
      Serial.printf("[WS] Client #%u connected — total: %u\n", num, ws_clients);
      // Greet new client with current status
      wsServer.sendTXT(num, "STATUS,CONNECTED");
      if (imu_available) wsServer.sendTXT(num, "STATUS,IMU_OK");
      if (mag_available) wsServer.sendTXT(num, "STATUS,MAG_OK");
      break;

    case WStype_DISCONNECTED:
      if (ws_clients > 0) ws_clients--;
      if (ws_clients == 0) digitalWrite(LED_CLIENT, LOW);
      Serial.printf("[WS] Client #%u disconnected — total: %u\n", num, ws_clients);
      break;

    case WStype_TEXT:
      // No commands expected from client in no_memory mode
      Serial.printf("[WS] Text from #%u: %s\n", num, payload);
      break;

    default:
      break;
  }
}

/* =========================================================================
 * HTTP: serve a file from LittleFS
 * ========================================================================= */
String getContentType(const String &path) {
  if (path.endsWith(".html")) return "text/html; charset=utf-8";
  if (path.endsWith(".css"))  return "text/css";
  if (path.endsWith(".js"))   return "application/javascript";
  if (path.endsWith(".png"))  return "image/png";
  if (path.endsWith(".jpg"))  return "image/jpeg";
  if (path.endsWith(".ico"))  return "image/x-icon";
  return "application/octet-stream";
}

void handleNotFound() {
  String path = httpServer.uri();
  if (path == "/") path = "/index.html";

  if (!LittleFS.exists(path)) {
    httpServer.send(404, "text/plain", "404 Not Found: " + path);
    return;
  }
  File f = LittleFS.open(path, "r");
  if (!f) {
    httpServer.send(500, "text/plain", "Failed to open file");
    return;
  }
  httpServer.streamFile(f, getContentType(path));
  f.close();
}

/* =========================================================================
 * SETUP
 * ========================================================================= */
void setup() {
  Serial.begin(115200);

  // ---- LED & Button pins ----
  pinMode(LED_1,      OUTPUT);
  pinMode(LED_2,      OUTPUT);
  pinMode(LED_WIFI,   OUTPUT);
  pinMode(LED_CLIENT, OUTPUT);
  pinMode(LED_RST,    OUTPUT);

  pinMode(BTN_WIFI, INPUT_PULLDOWN);
  pinMode(BTN_RST,  INPUT_PULLDOWN);

  // ---- GPS UART ----
  Serial1.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
  Serial.println("STATUS:GPS_SERIAL_STARTED");

  // ---- Boot blink ----
  for (int i = 0; i < 8; i++) {
    delay(150);
    digitalWrite(LED_1, i % 2);
    digitalWrite(LED_2, (i + 1) % 2);
  }
  digitalWrite(LED_1, HIGH);
  digitalWrite(LED_2, HIGH);

  // ---- LittleFS ----
  Serial.println("STATUS:INIT_LITTLEFS...");
  if (!LittleFS.begin(false)) {
    // Never format here — that would wipe the uploaded tile/web data!
    // If this fires, re-run: pio run -e no_memory --target uploadfs
    Serial.println("STATUS:LITTLEFS_MOUNT_FAILED");
    Serial.println("  → Re-flash filesystem: pio run -e no_memory --target uploadfs");
  } else {
    Serial.println("STATUS:LITTLEFS_OK");
    size_t total = LittleFS.totalBytes();
    size_t used  = LittleFS.usedBytes();
    Serial.printf("  LittleFS: %u KB used / %u KB total\n",
                  used / 1024, total / 1024);
  }

  // ---- Wi-Fi Access Point ----
  Serial.println("STATUS:STARTING_WIFI_AP...");
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid, password);
  delay(500);
  Serial.printf("AP IP: %s\n", WiFi.softAPIP().toString().c_str());
  Serial.printf("Connect to: %s  pwd: %s\n", ssid, password);
  Serial.println("Then open: http://192.168.4.1");
  digitalWrite(LED_WIFI, HIGH);

  // ---- HTTP server ----
  httpServer.onNotFound(handleNotFound);
  httpServer.begin();
  Serial.println("STATUS:HTTP_SERVER_STARTED on :80");

  // ---- WebSocket server ----
  wsServer.begin();
  wsServer.onEvent(onWsEvent);
  Serial.println("STATUS:WS_SERVER_STARTED on :81");

  // ---- I2C + Sensors ----
  Serial.println("STATUS:INIT_I2C...");
  Wire.begin(I2C_SDA, I2C_SCL, 400000);
  delay(200);

  if (!imu.init()) {
    Serial.println("STATUS:ERROR:LSM6_NOT_FOUND");
    imu_available = false;
    digitalWrite(LED_1, LOW);
  } else {
    imu.enableDefault();
    imu_available = true;
    Serial.println("STATUS:IMU_OK");
  }

  if (!mag.init()) {
    Serial.println("STATUS:WARN:LIS3MDL_NOT_FOUND — using 6-DOF");
    mag_available = false;
    digitalWrite(LED_2, LOW);
  } else {
    mag.enableDefault();
    mag_available = true;
    Serial.println("STATUS:MAG_OK_9DOF");
  }

  filter.begin(SAMPLE_RATE_HZ);

  // ---- Gyro calibration (3 s, keep still) ----
  Serial.println("STATUS:CALIBRATING_GYRO... KEEP STILL");
  long count = 0, sumX = 0, sumY = 0, sumZ = 0;
  unsigned long calStart = millis();
  while (millis() - calStart < 3000) {
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
    Serial.printf("Gyro bias: gx=%.1f  gy=%.1f  gz=%.1f\n",
                  gx_bias, gy_bias, gz_bias);
  }

  Serial.println("STATUS:SYSTEM_READY");
  lastMicros = micros();
}

/* =========================================================================
 * MAIN LOOP
 * ========================================================================= */
void loop() {

  // ---- Buttons ----
  if (readButton(btnWifi)) {
    wifi_enabled = !wifi_enabled;
    if (wifi_enabled) {
      WiFi.mode(WIFI_AP);
      WiFi.softAP(ssid, password);
      wsServer.begin();
      httpServer.begin();
      digitalWrite(LED_WIFI, HIGH);
      Serial.println("STATUS:WIFI_ENABLED");
    } else {
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_OFF);
      digitalWrite(LED_WIFI, LOW);
      digitalWrite(LED_CLIENT, LOW);
      Serial.println("STATUS:WIFI_DISABLED");
    }
  }

  if (readButton(btnRst)) {
    Serial.println("STATUS:RESTARTING...");
    digitalWrite(LED_RST, HIGH);
    delay(300);
    ESP.restart();
  }

  // ---- Network handling ----
  if (wifi_enabled) {
    wsServer.loop();
    httpServer.handleClient();
  }

  // ---- GPS: continuously read UART buffer ----
  while (Serial1.available() > 0) {
    gps.encode(Serial1.read());
  }

  // ---- 50 Hz sensor loop ----
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

      // Serial debug
      Serial.printf("DATA:%.2f,%.2f,%.2f\n", roll, pitch, yaw);

      // WebSocket broadcast — CSV data row (prefix "D,")
      // Format: D,timestamp_ms,roll,pitch,yaw,ax,ay,az,gx,gy,gz,mx,my,mz,lat,lng,alt,speed,sats
      snprintf(txBuf, sizeof(txBuf),
               "D,%lu,%.2f,%.2f,%.2f"
               ",%.4f,%.4f,%.4f"
               ",%.4f,%.4f,%.4f"
               ",%.4f,%.4f,%.4f"
               ",%.6f,%.6f,%.1f,%.1f,%d",
               millis(), roll, pitch, yaw,
               ax, ay, az,
               gx, gy, gz,
               mx, my, mz,
               cached_lat, cached_lng, cached_alt, cached_speed, cached_sats);

      if (wifi_enabled && ws_clients > 0) {
        wsServer.broadcastTXT(txBuf);
      }
    }
  }

  // ---- 1 Hz GPS update ----
  if (millis() - lastGPSUpdate >= 1000) {
    lastGPSUpdate = millis();

    if (gps.location.isValid()) {
      cached_lat   = gps.location.lat();
      cached_lng   = gps.location.lng();
      cached_alt   = gps.altitude.isValid()  ? gps.altitude.meters() : 0.0;
      cached_speed = gps.speed.isValid()      ? gps.speed.kmph()      : 0.0;
      cached_sats  = gps.satellites.isValid() ? (int)gps.satellites.value() : 0;
      cached_fix   = true;
    } else {
      cached_sats = gps.satellites.isValid() ? (int)gps.satellites.value() : 0;
      cached_fix  = false;
    }

    // GPS-specific message for map update
    // Format: GPS,timestamp_ms,lat,lng,alt,speed,sats,fix
    snprintf(txBuf, sizeof(txBuf),
             "GPS,%lu,%.6f,%.6f,%.1f,%.1f,%d,%d",
             millis(), cached_lat, cached_lng,
             cached_alt, cached_speed, cached_sats, (int)cached_fix);

    if (wifi_enabled && ws_clients > 0) {
      wsServer.broadcastTXT(txBuf);
    }

    Serial.printf("GPS:%.6f,%.6f alt=%.1f spd=%.1f sats=%d fix=%d\n",
                  cached_lat, cached_lng, cached_alt,
                  cached_speed, cached_sats, (int)cached_fix);
    Serial.printf("GPS_DEBUG: chars=%lu failed=%lu\n",
                  gps.charsProcessed(), gps.failedChecksum());
  }
}
