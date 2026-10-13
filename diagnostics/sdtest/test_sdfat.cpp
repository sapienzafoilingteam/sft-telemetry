#include <Arduino.h>
#include <SPI.h>
#include <SdFat.h>

#define SD_CS 10
#define SD_MOSI 11
#define SD_SCK 12
#define SD_MISO 13

SPIClass sdSPI(FSPI);
SdFat sd;

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n=== INIZIO TEST SDFAT ===");
  
  pinMode(SD_MISO, INPUT_PULLUP);
  
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, -1);
  
  Serial.println("Tento sd.cardBegin(sdSPI, SD_CS)...");
  
  // Try lower speed first
  if (!sd.cardBegin(SdSpiConfig(SD_CS, SHARED_SPI, SD_SCK_MHZ(1), &sdSPI))) {
    Serial.println("cardBegin fallita a 1 MHz!");
    Serial.printf("SdFat Error: 0x%X, Data: 0x%X\n", sd.card()->errorCode(), sd.card()->errorData());
    
    // Attempt to print readable error
    sd.initErrorPrint(&Serial);
  } else {
    Serial.println("cardBegin SUCCESS a 1 MHz!");
    if (!sd.volumeBegin()) {
      Serial.println("volumeBegin fallita! Partizione danneggiata?");
    } else {
      Serial.println("volumeBegin SUCCESS!");
      sd.ls(&Serial, LS_SIZE);
    }
  }
}

void loop() {
  delay(1000);
}
