/*
 * =========================================================================
 *  SFT HARDWARE TEST PANEL — ESP32-S3 DevKitC-1
 *  Sapienza Foiling Team
 * =========================================================================
 *
 *  Comprehensive diagnostic firmware to verify:
 *    1. GPIO pins (LEDs)
 *    2. I2C bus (scan + device detection)
 *    3. IMU — LSM6DS33 (Accel + Gyro) @ 0x6B
 *    4. Magnetometer — LIS3MDL @ 0x1E
 *    5. GPS — NEO-6M via UART (Serial1)
 *    6. SPI — MicroSD card slot
 *
 *  Usage:
 *    1. Upload with: pio run -d hw_test --target upload
 *    2. Monitor with: pio device monitor -b 115200
 *    3. Follow the interactive menu in the Serial Monitor.
 *
 * =========================================================================
 */

#include <Arduino.h>
#include <LIS3MDL.h>
#include <LSM6.h>
#include <SD.h>
#include <SPI.h>
#include <TinyGPS++.h>
#include <Wire.h>
#include <driver/gpio.h>

/* =========================================================================
 * PIN DEFINITIONS (must match your wiring)
 * ========================================================================= */
#define I2C_SDA 8
#define I2C_SCL 9

#define LED_1 21 // IMU status LED
#define LED_2 45 // Magnetometer status LED

#define GPS_RX 2 // ESP32 RX <- GPS TX
#define GPS_TX 1 // ESP32 TX -> GPS RX

// MicroSD SPI pins
#define SD_CS 10
#define SD_MOSI 11
#define SD_SCK 12
#define SD_MISO 13

/* =========================================================================
 * GLOBAL OBJECTS
 * ========================================================================= */
LSM6 imu;
LIS3MDL mag;
TinyGPSPlus gps;
SPIClass sdSPI(FSPI);

// Test result counters
int tests_passed = 0;
int tests_failed = 0;
int tests_warned = 0;

/* =========================================================================
 * HELPERS — pretty printing
 * ========================================================================= */

void printSeparator() {
  Serial.println("══════════════════════════════════════════════════════════");
}

void printHeader(const char *title) {
  Serial.println();
  printSeparator();
  Serial.printf("  %s\n", title);
  printSeparator();
}

void printResult(const char *test, const char *status, const char *detail) {
  // status: "PASS", "FAIL", "WARN", "INFO"
  const char *icon;
  if (strcmp(status, "PASS") == 0) {
    icon = "✅";
    tests_passed++;
  } else if (strcmp(status, "FAIL") == 0) {
    icon = "❌";
    tests_failed++;
  } else if (strcmp(status, "WARN") == 0) {
    icon = "⚠️ ";
    tests_warned++;
  } else {
    icon = "ℹ️ ";
  }

  Serial.printf("  %s [%s] %s", icon, status, test);
  if (detail && strlen(detail) > 0) {
    Serial.printf(" — %s", detail);
  }
  Serial.println();
}

void resetCounters() {
  tests_passed = 0;
  tests_failed = 0;
  tests_warned = 0;
}

void printSummary() {
  printHeader("TEST SUMMARY");
  Serial.printf("  Passed : %d\n", tests_passed);
  Serial.printf("  Failed : %d\n", tests_failed);
  Serial.printf("  Warnings: %d\n", tests_warned);
  Serial.printf("  Total  : %d\n", tests_passed + tests_failed + tests_warned);
  Serial.println();
  if (tests_failed == 0) {
    Serial.println("  🟢 ALL CRITICAL TESTS PASSED");
  } else {
    Serial.printf("  🔴 %d CRITICAL TEST(S) FAILED — check wiring!\n",
                  tests_failed);
  }
  printSeparator();
  Serial.println();
}

/* =========================================================================
 * TEST 1: GPIO / LED PINS
 * ========================================================================= */
void testGPIO() {
  printHeader("TEST 1: GPIO — LED PINS");

  // LED 1 (GPIO 21)
  Serial.println("  Testing LED_1 (GPIO 21)...");
  pinMode(LED_1, OUTPUT);

  // Test HIGH
  digitalWrite(LED_1, HIGH);
  delay(50);
  int val = digitalRead(LED_1);
  if (val == HIGH) {
    printResult("LED_1 write HIGH", "PASS", "GPIO 21 drives HIGH correctly");
  } else {
    printResult("LED_1 write HIGH", "FAIL", "GPIO 21 did not read back HIGH");
  }

  // Test LOW
  digitalWrite(LED_1, LOW);
  delay(50);
  val = digitalRead(LED_1);
  if (val == LOW) {
    printResult("LED_1 write LOW", "PASS", "GPIO 21 drives LOW correctly");
  } else {
    printResult("LED_1 write LOW", "FAIL", "GPIO 21 did not read back LOW");
  }

  // Visual blink test
  Serial.println("  → Blinking LED_1 3 times (should be visible)...");
  for (int i = 0; i < 3; i++) {
    digitalWrite(LED_1, HIGH);
    delay(200);
    digitalWrite(LED_1, LOW);
    delay(200);
  }
  printResult("LED_1 blink", "INFO", "Verify LED_1 blinked visually");

  Serial.println();

  // LED 2 (GPIO 45)
  Serial.println("  Testing LED_2 (GPIO 45)...");
  pinMode(LED_2, OUTPUT);

  digitalWrite(LED_2, HIGH);
  delay(50);
  val = digitalRead(LED_2);
  if (val == HIGH) {
    printResult("LED_2 write HIGH", "PASS", "GPIO 45 drives HIGH correctly");
  } else {
    printResult("LED_2 write HIGH", "FAIL", "GPIO 45 did not read back HIGH");
  }

  digitalWrite(LED_2, LOW);
  delay(50);
  val = digitalRead(LED_2);
  if (val == LOW) {
    printResult("LED_2 write LOW", "PASS", "GPIO 45 drives LOW correctly");
  } else {
    printResult("LED_2 write LOW", "FAIL", "GPIO 45 did not read back LOW");
  }

  Serial.println("  → Blinking LED_2 3 times (should be visible)...");
  for (int i = 0; i < 3; i++) {
    digitalWrite(LED_2, HIGH);
    delay(200);
    digitalWrite(LED_2, LOW);
    delay(200);
  }
  printResult("LED_2 blink", "INFO", "Verify LED_2 blinked visually");
}

/* =========================================================================
 * TEST 2: I2C BUS SCAN
 * ========================================================================= */
void testI2C() {
  printHeader("TEST 2: I2C BUS SCAN (SDA=GPIO8, SCL=GPIO9)");

  Wire.begin(I2C_SDA, I2C_SCL, 400000);
  delay(100);

  int devicesFound = 0;
  bool found_LSM6 = false;
  bool found_LIS3 = false;

  Serial.println("  Scanning addresses 0x01 — 0x7F...");
  Serial.println();
  Serial.println("     00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F");

  for (int row = 0; row < 8; row++) {
    Serial.printf("  %X0:", row);
    for (int col = 0; col < 16; col++) {
      int addr = row * 16 + col;
      if (addr < 1 || addr > 0x7F) {
        Serial.print("   ");
        continue;
      }

      Wire.beginTransmission(addr);
      int error = Wire.endTransmission();

      if (error == 0) {
        Serial.printf(" %02X", addr);
        devicesFound++;
        if (addr == 0x6B)
          found_LSM6 = true;
        if (addr == 0x1E)
          found_LIS3 = true;
      } else {
        Serial.print(" --");
      }
    }
    Serial.println();
  }

  Serial.println();
  Serial.printf("  Devices found: %d\n", devicesFound);
  Serial.println();

  // Evaluate results
  if (found_LSM6) {
    printResult("LSM6DS33 (Accel/Gyro) @ 0x6B", "PASS",
                "Device detected on I2C bus");
  } else {
    printResult("LSM6DS33 (Accel/Gyro) @ 0x6B", "FAIL",
                "NOT found — check SDA/SCL wiring, power, solder joints");
  }

  if (found_LIS3) {
    printResult("LIS3MDL (Magnetometer) @ 0x1E", "PASS",
                "Device detected on I2C bus");
  } else {
    printResult("LIS3MDL (Magnetometer) @ 0x1E", "FAIL",
                "NOT found — check SDA/SCL wiring, power, solder joints");
  }

  if (devicesFound == 0) {
    printResult(
        "I2C bus connectivity", "FAIL",
        "No devices found! Check: SDA (GPIO8), SCL (GPIO9), pull-ups, power");
  } else if (devicesFound > 0 && (!found_LSM6 || !found_LIS3)) {
    char buf[80];
    snprintf(buf, sizeof(buf),
             "%d device(s) found but expected sensors missing", devicesFound);
    printResult("I2C bus connectivity", "WARN", buf);
  }
}

/* =========================================================================
 * TEST 3: IMU — LSM6DS33 (Accelerometer + Gyroscope)
 * ========================================================================= */
void testIMU() {
  printHeader("TEST 3: IMU — LSM6DS33 (Accelerometer + Gyroscope)");

  Wire.begin(I2C_SDA, I2C_SCL, 400000);
  delay(100);

  if (!imu.init()) {
    printResult("LSM6DS33 init()", "FAIL",
                "init() returned false — sensor not responding");
    return;
  }
  printResult("LSM6DS33 init()", "PASS", "Sensor initialized successfully");

  imu.enableDefault();
  delay(100);

  // Read WHO_AM_I register directly
  Wire.beginTransmission(0x6B);
  Wire.write(0x0F); // WHO_AM_I register
  Wire.endTransmission(false);
  Wire.requestFrom(0x6B, 1);
  if (Wire.available()) {
    uint8_t whoami = Wire.read();
    char buf[64];
    snprintf(buf, sizeof(buf), "WHO_AM_I = 0x%02X (expected 0x69 for LSM6DS33)",
             whoami);
    if (whoami == 0x69) {
      printResult("WHO_AM_I register", "PASS", buf);
    } else {
      printResult("WHO_AM_I register", "WARN", buf);
    }
  } else {
    printResult("WHO_AM_I register", "FAIL", "Could not read register");
  }

  // Take multiple readings and check for sane data
  Serial.println();
  Serial.println("  Reading 10 samples (200ms interval)...");
  Serial.println("  Sample | Accel X   Y   Z       | Gyro X   Y   Z");
  Serial.println("  -------|------------------------|------------------------");

  int good_reads = 0;
  bool accel_nonzero = false;
  bool gyro_varies = false;
  int16_t first_ax = 0, first_ay = 0, first_az = 0;

  for (int i = 0; i < 10; i++) {
    imu.read();
    if (imu.last_status == 0) {
      good_reads++;
      Serial.printf("  %4d   | %6d %6d %6d | %6d %6d %6d\n", i + 1, imu.a.x,
                    imu.a.y, imu.a.z, imu.g.x, imu.g.y, imu.g.z);

      // Check if accelerometer has non-zero values (gravity should give ~16384
      // on one axis at ±2g)
      if (abs(imu.a.x) > 1000 || abs(imu.a.y) > 1000 || abs(imu.a.z) > 1000) {
        accel_nonzero = true;
      }

      if (i == 0) {
        first_ax = imu.a.x;
        first_ay = imu.a.y;
        first_az = imu.a.z;
      } else {
        // Check if gyro values are not stuck (comparing with first reading)
        if (abs(imu.g.x - first_ax) > 50 || abs(imu.g.y - first_ay) > 50) {
          gyro_varies = true;
        }
      }
    } else {
      Serial.printf("  %4d   | *** READ ERROR (status=%d) ***\n", i + 1,
                    imu.last_status);
    }
    delay(20);
  }

  Serial.println();

  if (good_reads == 10) {
    printResult("IMU data read (10/10)", "PASS",
                "All reads successful, I2C communication stable");
  } else if (good_reads > 0) {
    char buf[64];
    snprintf(buf, sizeof(buf),
             "%d/10 reads succeeded — intermittent connection?", good_reads);
    printResult("IMU data read", "WARN", buf);
  } else {
    printResult("IMU data read", "FAIL", "All reads failed — check wiring");
  }

  if (accel_nonzero) {
    printResult("Accelerometer gravity detect", "PASS",
                "Non-zero accel values detected (gravity)");
  } else {
    printResult("Accelerometer gravity detect", "FAIL",
                "All accel readings near zero — sensor may be stuck");
  }

  // Check magnitude of acceleration vector (should be ~1g ≈ 16384 at ±2g full
  // scale)
  imu.read();
  float ax = imu.a.x, ay = imu.a.y, az = imu.a.z;
  float mag_g = sqrt(ax * ax + ay * ay + az * az);
  char mbuf[80];
  snprintf(mbuf, sizeof(mbuf),
           "|A| = %.0f raw (expect ~16384 for 1g at ±2g scale)", mag_g);
  if (mag_g > 10000 && mag_g < 25000) {
    printResult("Accel magnitude (1g check)", "PASS", mbuf);
  } else {
    printResult("Accel magnitude (1g check)", "WARN", mbuf);
  }
}

/* =========================================================================
 * TEST 4: MAGNETOMETER — LIS3MDL
 * ========================================================================= */
void testMag() {
  printHeader("TEST 4: MAGNETOMETER — LIS3MDL");

  Wire.begin(I2C_SDA, I2C_SCL, 400000);
  delay(100);

  if (!mag.init()) {
    printResult("LIS3MDL init()", "FAIL",
                "init() returned false — sensor not responding");
    return;
  }
  printResult("LIS3MDL init()", "PASS", "Sensor initialized successfully");

  mag.enableDefault();
  delay(100);

  // Read WHO_AM_I
  Wire.beginTransmission(0x1E);
  Wire.write(0x0F); // WHO_AM_I
  Wire.endTransmission(false);
  Wire.requestFrom(0x1E, 1);
  if (Wire.available()) {
    uint8_t whoami = Wire.read();
    char buf[64];
    snprintf(buf, sizeof(buf), "WHO_AM_I = 0x%02X (expected 0x3D for LIS3MDL)",
             whoami);
    if (whoami == 0x3D) {
      printResult("WHO_AM_I register", "PASS", buf);
    } else {
      printResult("WHO_AM_I register", "WARN", buf);
    }
  } else {
    printResult("WHO_AM_I register", "FAIL", "Could not read register");
  }

  // Take multiple readings
  Serial.println();
  Serial.println("  Reading 10 samples (100ms interval)...");
  Serial.println("  Sample | Mag X     Y     Z");
  Serial.println("  -------|---------------------");

  int good_reads = 0;
  bool nonzero = false;

  for (int i = 0; i < 10; i++) {
    mag.read();
    good_reads++;
    Serial.printf("  %4d   | %6d %6d %6d\n", i + 1, mag.m.x, mag.m.y, mag.m.z);

    if (abs(mag.m.x) > 100 || abs(mag.m.y) > 100 || abs(mag.m.z) > 100) {
      nonzero = true;
    }
    delay(100);
  }

  Serial.println();

  if (good_reads == 10) {
    printResult("Mag data read (10/10)", "PASS", "All reads successful");
  } else {
    char buf[64];
    snprintf(buf, sizeof(buf), "%d/10 reads succeeded", good_reads);
    printResult("Mag data read", "WARN", buf);
  }

  if (nonzero) {
    printResult("Magnetic field detection", "PASS",
                "Non-zero values detected (Earth's magnetic field)");
  } else {
    printResult("Magnetic field detection", "WARN",
                "All readings near zero — unusual, check environment");
  }

  // Magnitude check
  mag.read();
  float mx = mag.m.x, my = mag.m.y, mz = mag.m.z;
  float field = sqrt(mx * mx + my * my + mz * mz);
  char fbuf[80];
  snprintf(fbuf, sizeof(fbuf),
           "|M| = %.0f raw (should be non-trivial if sensor is working)",
           field);
  if (field > 50) {
    printResult("Mag field magnitude", "PASS", fbuf);
  } else {
    printResult("Mag field magnitude", "WARN", fbuf);
  }
}

/* =========================================================================
 * TEST 5: GPS — NEO-6M via UART
 * ========================================================================= */
void testGPS() {
  printHeader("TEST 5: GPS — NEO-6M (UART: RX=GPIO2, TX=GPIO1)");

  Serial1.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
  delay(200);

  Serial.println("  Listening for GPS NMEA data for 10 seconds...");
  Serial.println("  (GPS module must be powered and antenna connected)");
  Serial.println();

  unsigned long startTime = millis();
  unsigned long duration = 10000; // 10 seconds
  int rawChars = 0;
  int nmeaSentences = 0;
  bool gotFix = false;

  // Show raw NMEA stream for first 3 seconds
  Serial.println("  --- RAW NMEA STREAM (first 3s) ---");
  String nmeaLine = "";
  unsigned long rawPhaseEnd = startTime + 3000;

  while (millis() < startTime + duration) {
    while (Serial1.available() > 0) {
      char c = Serial1.read();
      rawChars++;
      gps.encode(c);

      // Print raw stream for the first 3 seconds
      if (millis() < rawPhaseEnd) {
        if (c == '\n') {
          Serial.printf("  > %s\n", nmeaLine.c_str());
          nmeaLine = "";
          nmeaSentences++;
        } else if (c >= ' ') {
          nmeaLine += c;
        }
      }
    }
    delay(1);
  }

  // Flush remaining line
  if (nmeaLine.length() > 0) {
    Serial.printf("  > %s\n", nmeaLine.c_str());
    nmeaSentences++;
  }

  Serial.println("  --- END RAW STREAM ---");
  Serial.println();

  // Evaluate
  unsigned long charsProcessed = gps.charsProcessed();
  unsigned long sentencesWithFix = gps.sentencesWithFix();
  unsigned long failedChecksum = gps.failedChecksum();

  Serial.printf("  Stats: Chars=%lu, Sentences=%lu, FailedCRC=%lu\n",
                charsProcessed, sentencesWithFix, failedChecksum);
  Serial.println();

  if (rawChars > 0) {
    char buf[80];
    snprintf(buf, sizeof(buf), "%d chars received in 10s", rawChars);
    printResult("UART data reception", "PASS", buf);
  } else {
    printResult(
        "UART data reception", "FAIL",
        "No data received — check: GPS power, TX->GPIO2 wiring, baud 9600");
  }

  if (charsProcessed > 50) {
    printResult("NMEA parsing", "PASS", "TinyGPS++ processed valid NMEA chars");
  } else if (charsProcessed > 0) {
    printResult("NMEA parsing", "WARN",
                "Very few chars processed — noisy signal?");
  } else {
    printResult("NMEA parsing", "FAIL", "No chars processed by TinyGPS++");
  }

  if (failedChecksum > 0) {
    char buf[80];
    snprintf(buf, sizeof(buf),
             "%lu failed checksums — possible wiring issue or interference",
             failedChecksum);
    printResult("NMEA checksum", "WARN", buf);
  } else if (charsProcessed > 0) {
    printResult("NMEA checksum", "PASS", "No checksum errors");
  }

  if (gps.location.isValid()) {
    char buf[80];
    snprintf(buf, sizeof(buf), "Lat=%.6f, Lng=%.6f", gps.location.lat(),
             gps.location.lng());
    printResult("GPS fix", "PASS", buf);
  } else {
    printResult("GPS fix", "WARN",
                "No fix yet (normal indoors — take outdoors for fix)");
  }

  int sats = gps.satellites.value();
  char sbuf[64];
  snprintf(sbuf, sizeof(sbuf), "%d satellite(s) in view", sats);
  if (sats > 0) {
    printResult("Satellites", "PASS", sbuf);
  } else {
    printResult("Satellites", "WARN", sbuf);
  }
}

/* =========================================================================
 * TEST 6: SPI — MICROSD CARD
 * ========================================================================= */
void testSD() {
  printHeader("TEST 6: SPI / MICROSD (CS=10, MOSI=11, SCK=12, MISO=13)");

  // Deinit first to reset state
  SD.end();
  sdSPI.end();
  delay(100);

  // Assert CS HIGH before SPI init (deselect card)
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  delay(50);

  // Init SPI bus — do NOT pass CS here, SD.begin() manages it
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI);

  // Software pull-ups on SPI lines
  gpio_pullup_en((gpio_num_t)SD_MISO);
  gpio_pullup_en((gpio_num_t)SD_MOSI);
  gpio_pullup_en((gpio_num_t)SD_SCK);
  gpio_pullup_en((gpio_num_t)SD_CS);

  delay(100); // Let card power stabilize

  // Test CS pin can toggle
  digitalWrite(SD_CS, LOW);
  delay(10);
  digitalWrite(SD_CS, HIGH);
  delay(10);
  int csVal = digitalRead(SD_CS);
  if (csVal == HIGH) {
    printResult("SD_CS (GPIO 10) toggle", "PASS", "CS pin drives HIGH");
  } else {
    printResult("SD_CS (GPIO 10) toggle", "FAIL", "CS pin not responding");
  }

  // ---- EXTENDED RAW SPI DIAGNOSTIC ----
  Serial.println();
  Serial.println("  --- EXTENDED SPI CARD DIAGNOSTIC ---");

  sdSPI.beginTransaction(SPISettings(100000, MSBFIRST, SPI_MODE0));

  // Helper lambda: send command, return R1 response
  auto sendCmd = [&](uint8_t cmd, uint32_t arg, uint8_t crc) -> uint8_t {
    digitalWrite(SD_CS, LOW);
    delay(1);
    sdSPI.transfer(0x40 | cmd);
    sdSPI.transfer((arg >> 24) & 0xFF);
    sdSPI.transfer((arg >> 16) & 0xFF);
    sdSPI.transfer((arg >> 8) & 0xFF);
    sdSPI.transfer(arg & 0xFF);
    sdSPI.transfer(crc | 0x01);

    uint8_t r = 0xFF;
    for (int i = 0; i < 16; i++) {
      r = sdSPI.transfer(0xFF);
      if (r != 0xFF)
        break;
    }
    return r;
  };

  auto deselectCard = [&]() {
    digitalWrite(SD_CS, HIGH);
    sdSPI.transfer(0xFF);
  };

  // Step 1: Send ≥80 dummy clocks with CS HIGH
  digitalWrite(SD_CS, HIGH);
  for (int i = 0; i < 10; i++)
    sdSPI.transfer(0xFF);
  Serial.println("  [1] Sent 80 clocks with CS HIGH");

  // Step 2: CMD0 — GO_IDLE_STATE
  uint8_t r1 = sendCmd(0, 0x00000000, 0x94);
  deselectCard();
  Serial.printf("  [2] CMD0  (GO_IDLE)    → R1=0x%02X %s\n", r1,
                r1 == 0x01 ? "✓ idle" : "✗ FAIL");
  if (r1 != 0x01) {
    printResult("SD raw init", "FAIL", "CMD0 failed — card not responding");
    sdSPI.endTransaction();
    Serial.println("  --- END DIAGNOSTIC ---");
    return;
  }

  // Step 3: CMD8 — SEND_IF_COND (check SDv2 / SDHC support)
  r1 = sendCmd(8, 0x000001AA, 0x86);
  uint8_t cmd8_resp[4] = {0};
  if (r1 <= 0x01) {
    for (int i = 0; i < 4; i++)
      cmd8_resp[i] = sdSPI.transfer(0xFF);
  }
  deselectCard();
  Serial.printf("  [3] CMD8  (IF_COND)    → R1=0x%02X", r1);
  if (r1 == 0x01) {
    Serial.printf(" data=[%02X %02X %02X %02X]", cmd8_resp[0], cmd8_resp[1],
                  cmd8_resp[2], cmd8_resp[3]);
    if (cmd8_resp[2] == 0x01 && cmd8_resp[3] == 0xAA) {
      Serial.println(" ✓ SDv2 card");
    } else {
      Serial.println(" ✗ bad echo-back");
    }
  } else if (r1 == 0x05) {
    Serial.println(" → SDv1 card (CMD8 not supported)");
  } else {
    Serial.printf(" ✗ unexpected\n");
  }

  bool isSDv2 = (r1 == 0x01 && cmd8_resp[3] == 0xAA);

  // Step 4: ACMD41 — SD_SEND_OP_COND (with HCS bit for SDv2)
  //   ACMD = CMD55 + CMD41
  //   Some cards need up to 2-3 seconds; we allow 5s total
  uint32_t acmd41_arg = isSDv2 ? 0x40000000 : 0x00000000;
  bool cardReady = false;
  Serial.print("  [4] ACMD41 (SEND_OP)   → ");

  unsigned long acmdStart = millis();
  for (int retry = 0; retry < 200; retry++) {
    // CMD55 (APP_CMD prefix)
    r1 = sendCmd(55, 0, 0x64);
    deselectCard();
    if (r1 > 0x01) {
      Serial.printf("CMD55 rejected (R1=0x%02X)\n", r1);
      break;
    }

    // CMD41
    r1 = sendCmd(41, acmd41_arg, 0x76);
    deselectCard();

    if (r1 == 0x00) {
      cardReady = true;
      Serial.printf("R1=0x00 ✓ ready (after %d retries, %lums)\n", retry + 1,
                    millis() - acmdStart);
      break;
    }

    // Progress dots every 500ms
    if (retry > 0 && retry % 20 == 0)
      Serial.print(".");

    delay(25);
  }

  if (!cardReady) {
    Serial.println();
    Serial.println("  [4b] Falling back to CMD1 (MMC/old card protocol)...");
    unsigned long cmd1Start = millis();
    for (int retry = 0; retry < 100; retry++) {
      r1 = sendCmd(1, 0, 0xF9);
      deselectCard();
      if (r1 == 0x00) {
        cardReady = true;
        Serial.printf("  CMD1 R1=0x00 ✓ ready (after %d retries, %lums)\n", retry + 1,
                      millis() - cmd1Start);
        break;
      }
      if (retry > 0 && retry % 20 == 0)
        Serial.print(".");
      delay(25);
    }
  }

  if (!cardReady) {
    Serial.printf("R1=0x%02X ✗ card never became ready (ACMD41 & CMD1 timed out)\n", r1);
    printResult("SD raw init", "FAIL",
                "ACMD41 & CMD1 timeout — card won't initialize. Check voltage (try 5V instead of 3.3V) or try another card.");
    sdSPI.endTransaction();
    Serial.println("  --- END DIAGNOSTIC ---");
    return;
  }

  // Step 5: CMD58 — READ_OCR (check CCS bit → SDHC?)
  r1 = sendCmd(58, 0, 0xFC);
  uint8_t ocr[4] = {0};
  if (r1 == 0x00) {
    for (int i = 0; i < 4; i++)
      ocr[i] = sdSPI.transfer(0xFF);
  }
  deselectCard();
  bool isHC = (ocr[0] & 0x40) != 0;
  Serial.printf("  [5] CMD58 (READ_OCR)   → R1=0x%02X OCR=[%02X %02X %02X %02X]"
                " %s\n",
                r1, ocr[0], ocr[1], ocr[2], ocr[3],
                isHC ? "SDHC/SDXC" : "SDSC");

  // Step 6: CMD17 — READ_SINGLE_BLOCK (try to read sector 0)
  Serial.print("  [6] CMD17 (READ BLK 0) → ");
  r1 = sendCmd(17, 0, 0x00);
  if (r1 == 0x00) {
    // Wait for data token (0xFE)
    uint8_t token = 0xFF;
    for (int i = 0; i < 1000; i++) {
      token = sdSPI.transfer(0xFF);
      if (token != 0xFF)
        break;
    }

    if (token == 0xFE) {
      // Read 512 bytes (just consume them)
      uint8_t firstBytes[16];
      for (int i = 0; i < 512; i++) {
        uint8_t b = sdSPI.transfer(0xFF);
        if (i < 16)
          firstBytes[i] = b;
      }
      // Read 2 CRC bytes
      sdSPI.transfer(0xFF);
      sdSPI.transfer(0xFF);
      deselectCard();

      Serial.println("R1=0x00 token=0xFE ✓ data received");
      Serial.print("  First 16 bytes of sector 0: ");
      for (int i = 0; i < 16; i++)
        Serial.printf("%02X ", firstBytes[i]);
      Serial.println();

      // Check for valid FAT32 boot signature
      bool looksLikeFAT = (firstBytes[0] == 0xEB || firstBytes[0] == 0xE9);
      if (looksLikeFAT) {
        printResult("Sector 0 read", "PASS",
                    "Boot sector looks like valid FAT (starts with JMP)");
      } else if (firstBytes[0] == 0x00 && firstBytes[1] == 0x00) {
        printResult("Sector 0 read", "WARN",
                    "Sector 0 is all zeros — card may need reformatting");
      } else {
        char hexBuf[48];
        snprintf(hexBuf, sizeof(hexBuf), "Sector starts with 0x%02X — MBR?",
                 firstBytes[0]);
        printResult("Sector 0 read", "INFO", hexBuf);
      }
    } else {
      deselectCard();
      Serial.printf("R1=0x00 but token=0x%02X ✗ data read failed\n", token);
      printResult("Sector 0 read", "FAIL", "No data token received");
    }
  } else {
    deselectCard();
    Serial.printf("R1=0x%02X ✗ read command rejected\n", r1);
    printResult("Sector 0 read", "FAIL", "CMD17 rejected by card");
  }

  sdSPI.endTransaction();
  Serial.println("  --- END DIAGNOSTIC ---");
  Serial.println();

  // ---- SD.begin with retries ----
  // Full re-init SPI after raw test
  sdSPI.end();
  delay(100);
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  delay(50);
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI);
  delay(100);

  bool sdOk = false;
  for (int attempt = 1; attempt <= 3 && !sdOk; attempt++) {
    Serial.printf("  Attempting SD.begin (attempt %d/3)...\n", attempt);
    if (SD.begin(SD_CS, sdSPI, 400000)) {
      sdOk = true;
      char buf[64];
      snprintf(buf, sizeof(buf),
               "SD card initialized at 400 kHz (attempt %d)", attempt);
      printResult("SD.begin (400 kHz)", "PASS", buf);
    } else {
      if (attempt < 3) {
        SD.end();
        delay(500 * attempt);
      } else {
        printResult("SD.begin", "FAIL",
                    "SD init failed after 3 attempts");
      }
    }
  }

  if (!sdOk)
    return;

  // Card type
  uint8_t cardType = SD.cardType();
  const char *typeStr = "UNKNOWN";
  if (cardType == CARD_MMC)
    typeStr = "MMC";
  if (cardType == CARD_SD)
    typeStr = "SD";
  if (cardType == CARD_SDHC)
    typeStr = "SDHC";
  if (cardType == CARD_NONE) {
    printResult("Card detection", "FAIL", "No card detected (CARD_NONE)");
    return;
  }

  uint64_t cardSize = SD.cardSize() / (1024 * 1024);
  char buf[80];
  snprintf(buf, sizeof(buf), "Type=%s, Size=%lluMB", typeStr, cardSize);
  printResult("Card detection", "PASS", buf);

  // Try write + read test
  const char *testFile = "/_hw_test.tmp";
  const char *testData = "SFT_HW_TEST_OK";

  File f = SD.open(testFile, FILE_WRITE);
  if (f) {
    f.println(testData);
    f.close();
    printResult("SD write test", "PASS", "Wrote test file successfully");

    // Read it back
    f = SD.open(testFile, FILE_READ);
    if (f) {
      String readBack = f.readStringUntil('\n');
      readBack.trim();
      f.close();

      if (readBack == testData) {
        printResult("SD read-back verify", "PASS", "Data integrity OK");
      } else {
        char rbuf[128];
        snprintf(rbuf, sizeof(rbuf), "Mismatch: wrote '%s', read '%s'",
                 testData, readBack.c_str());
        printResult("SD read-back verify", "FAIL", rbuf);
      }

      // Clean up
      SD.remove(testFile);
      printResult("SD delete test file", "PASS", "Cleaned up test file");
    } else {
      printResult("SD read-back", "FAIL",
                  "Could not re-open test file for reading");
    }
  } else {
    printResult("SD write test", "FAIL",
                "Could not create test file — card may be read-only");
  }

  // List root contents
  File root = SD.open("/");
  if (root) {
    Serial.println();
    Serial.println("  Files on SD card root:");
    int fileCount = 0;
    File entry;
    while ((entry = root.openNextFile())) {
      if (!entry.isDirectory()) {
        Serial.printf("    📄 %-30s %8lu bytes\n", entry.name(),
                      (unsigned long)entry.size());
        fileCount++;
      }
      entry.close();
    }
    root.close();
    if (fileCount == 0) {
      Serial.println("    (empty)");
    }
    char cbuf[32];
    snprintf(cbuf, sizeof(cbuf), "%d file(s) on card", fileCount);
    printResult("SD file listing", "PASS", cbuf);
  }
}

/* =========================================================================
 * RUN ALL TESTS
 * ========================================================================= */
void runAllTests() {
  resetCounters();

  Serial.println();
  Serial.println();
  printSeparator();
  Serial.println("  ⚡ SFT HARDWARE TEST PANEL — ESP32-S3 DevKitC-1");
  Serial.println("  ⚡ Sapienza Foiling Team — Diagnostica Hardware");
  printSeparator();
  Serial.printf("  Timestamp: %lu ms since boot\n", millis());
  Serial.printf("  CPU Freq : %d MHz\n", getCpuFrequencyMhz());
  Serial.printf("  Free Heap: %lu bytes\n", (unsigned long)ESP.getFreeHeap());
  Serial.printf("  SDK      : %s\n", ESP.getSdkVersion());
  Serial.printf("  Chip Rev : %d\n", ESP.getChipRevision());
  Serial.printf("  Flash    : %lu MB\n",
                (unsigned long)(ESP.getFlashChipSize() / (1024 * 1024)));

  testGPIO();
  testI2C();
  testIMU();
  testMag();
  testGPS();
  testSD();

  printSummary();
}

/* =========================================================================
 * INTERACTIVE MENU
 * ========================================================================= */
void printMenu() {
  Serial.println();
  printSeparator();
  Serial.println("  INTERACTIVE TEST MENU");
  printSeparator();
  Serial.println("  [A] Run ALL tests");
  Serial.println("  [1] Test GPIO / LEDs");
  Serial.println("  [2] Test I2C bus scan");
  Serial.println("  [3] Test IMU (LSM6DS33)");
  Serial.println("  [4] Test Magnetometer (LIS3MDL)");
  Serial.println("  [5] Test GPS (NEO-6M UART)");
  Serial.println("  [6] Test MicroSD card (SPI)");
  Serial.println("  [L] Live sensor stream (10 seconds)");
  Serial.println("  [?] Show this menu");
  printSeparator();
  Serial.println("  Send a character to select a test:");
}

/* =========================================================================
 * LIVE SENSOR STREAM — continuous data for 10 seconds
 * ========================================================================= */
void liveSensorStream() {
  printHeader("LIVE SENSOR STREAM (10 seconds)");
  Serial.println("  Press any key to stop early.");
  Serial.println();

  Wire.begin(I2C_SDA, I2C_SCL, 400000);
  delay(100);

  bool imuOk = imu.init();
  bool magOk = mag.init();

  if (imuOk)
    imu.enableDefault();
  if (magOk)
    mag.enableDefault();

  Serial1.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);

  Serial.println("  Time(ms) | AX     AY     AZ    | GX     GY     GZ    | MX  "
                 "   MY     MZ    | GPS chars");
  Serial.println("  "
                 "---------|----------------------|----------------------|-----"
                 "-----------------|----------");

  unsigned long startTime = millis();
  while (millis() - startTime < 10000) {
    // Check for early stop
    if (Serial.available()) {
      Serial.read(); // consume
      break;
    }

    // Read sensors
    if (imuOk)
      imu.read();
    if (magOk)
      mag.read();

    // Read GPS
    while (Serial1.available()) {
      gps.encode(Serial1.read());
    }

    Serial.printf("  %7lu  | %6d %6d %6d | %6d %6d %6d | %6d %6d %6d | %lu\n",
                  millis() - startTime, imuOk ? imu.a.x : 0,
                  imuOk ? imu.a.y : 0, imuOk ? imu.a.z : 0, imuOk ? imu.g.x : 0,
                  imuOk ? imu.g.y : 0, imuOk ? imu.g.z : 0, magOk ? mag.m.x : 0,
                  magOk ? mag.m.y : 0, magOk ? mag.m.z : 0,
                  gps.charsProcessed());

    delay(100); // 10 Hz for readability
  }

  Serial.println();
  Serial.println("  Stream ended.");
}

/* =========================================================================
 * SETUP & LOOP
 * ========================================================================= */
void setup() {
  Serial.begin(115200);

  // USB CDC init delay — blink LED to show we're alive
  pinMode(LED_1, OUTPUT);
  for (int i = 0; i < 10; i++) {
    delay(200);
    digitalWrite(LED_1, i % 2);
  }
  digitalWrite(LED_1, LOW);

  // Auto-run all tests on boot
  runAllTests();
  printMenu();
}

void loop() {
  if (Serial.available()) {
    char c = Serial.read();

    // Consume any trailing newline/carriage return
    delay(50);
    while (Serial.available())
      Serial.read();

    switch (toupper(c)) {
    case 'A':
      runAllTests();
      break;
    case '1':
      resetCounters();
      testGPIO();
      printSummary();
      break;
    case '2':
      resetCounters();
      testI2C();
      printSummary();
      break;
    case '3':
      resetCounters();
      testIMU();
      printSummary();
      break;
    case '4':
      resetCounters();
      testMag();
      printSummary();
      break;
    case '5':
      resetCounters();
      testGPS();
      printSummary();
      break;
    case '6':
      resetCounters();
      testSD();
      printSummary();
      break;
    case 'L':
      liveSensorStream();
      break;
    case '?':
      printMenu();
      break;
    default:
      Serial.printf("  Unknown command: '%c'. Press '?' for menu.\n", c);
      break;
    }

    printMenu();
  }
}
