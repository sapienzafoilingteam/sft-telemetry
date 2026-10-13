#include <Arduino.h>
#include <TinyGPS++.h>

#define GPS_RX 1 
#define GPS_TX 2 

HardwareSerial gpsSerial(1);
TinyGPSPlus gps;

unsigned long lastDebug = 0;
uint32_t lastChars = 0;

void setup() {
  Serial.begin(115200);
  delay(2000); 
  Serial.println("\n\n--- GPS RAW DATA CHECK ---");
  
  // Avviamo la seriale
  gpsSerial.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
}

void loop() {
  while (gpsSerial.available() > 0) {
    char c = gpsSerial.read();
    gps.encode(c);
    
    // STAMPA TUTTO QUELLO CHE ARRIVA: 
    // Se vedi testo leggibile ($GPRMC...), è il GPS.
    // Se vedi simboli strani o nulla, è rumore/PSRAM.
    Serial.print(c); 
  }

  if (millis() - lastDebug > 2000) {
    uint32_t currentChars = gps.charsProcessed();
    uint32_t delta = currentChars - lastChars;
    lastChars = currentChars;
    lastDebug = millis();
    
    if (delta == 0) {
      Serial.println("\n[IDLE] No new data in the last 2 seconds.");
    } else {
      Serial.printf("\n[RECEIVING] New bytes: %u | Total: %u | Checksum Errors: %u\n", 
                    delta, currentChars, gps.failedChecksum());
    }
  }
}
