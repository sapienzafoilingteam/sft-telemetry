#include <Arduino.h>
#include <TinyGPS++.h>

/* =========================================================================
 * GPS AUTO-BAUD & DEBUG TOOL
 * 
 * Prova diverse velocità e mostra i dati grezzi.
 * ========================================================================= */

#define GPS_RX 1
#define GPS_TX 2
#define LED_1 21

TinyGPSPlus gps;
long bauds[] = {9600, 38400, 115200, 4800};
int baudIdx = 0;

void setup() {
  Serial.begin(115200);
  pinMode(LED_1, OUTPUT);
  Serial.println("\n\n>>> GPS AUTO-BAUD TEST START <<<");
}

unsigned long lastSwitch = 0;
uint32_t bytesInCurrentBaud = 0;

void loop() {
  static long currentBaud = 0;
  if (currentBaud == 0) {
    currentBaud = bauds[baudIdx];
    Serial1.begin(currentBaud, SERIAL_8N1, GPS_RX, GPS_TX);
    Serial.printf("\n--- PROVO VELOCITÀ: %ld baud ---\n", currentBaud);
    lastSwitch = millis();
    bytesInCurrentBaud = 0;
  }

  while (Serial1.available() > 0) {
    char c = Serial1.read();
    bytesInCurrentBaud++;
    gps.encode(c);
    if (c >= 32 && c <= 126) Serial.print(c);
    else if (c == '\n') Serial.print("\n");
  }

  // Se dopo 10 secondi non abbiamo fix o molti byte, cambiamo velocità
  if (millis() - lastSwitch > 10000) {
    if (gps.charsProcessed() < 10) {
      Serial.printf("\n[!] Pochi dati a %ld baud. Provo la prossima...\n", currentBaud);
      baudIdx = (baudIdx + 1) % 4;
      currentBaud = 0;
      Serial1.end();
    } else {
      Serial.printf("\n--- STATO A %ld BAUD ---", currentBaud);
      Serial.printf("\n    Byte: %u | Satelliti: %d | Fix: %s\n", 
                    gps.charsProcessed(), gps.satellites.value(), 
                    gps.location.isValid() ? "SI" : "NO");
      
      if (gps.failedChecksum() > gps.passedChecksum() && gps.charsProcessed() > 100) {
        Serial.println("    [!] Troppi errori di checksum. Velocità probabilmente errata.");
        baudIdx = (baudIdx + 1) % 4;
        currentBaud = 0;
        Serial1.end();
      } else if (gps.passedChecksum() > 0) {
        Serial.println("    [OK] Velocità corretta trovata!");
        lastSwitch = millis(); // Restiamo su questa velocità
      }
    }
  }
}
