#include <SPI.h>
#include "SdFat.h"

int pins[] = {10, 11, 12, 13};
SdFat sd;
SPIClass spi(FSPI);

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n\n--- SPI BRUTE FORCE SCANNER ---");
  Serial.println("Testing all 24 permutations of pins 10, 11, 12, 13...\n");

  bool found = false;

  for (int i = 0; i < 4; i++) {
    for (int j = 0; j < 4; j++) {
      if (j == i) continue;
      for (int k = 0; k < 4; k++) {
        if (k == i || k == j) continue;
        for (int l = 0; l < 4; l++) {
          if (l == i || l == j || l == k) continue;

          int test_CS = pins[i];
          int test_MOSI = pins[j];
          int test_MISO = pins[k];
          int test_SCK = pins[l];

          Serial.printf("Trying: CS=%d, MOSI=%d, MISO=%d, SCK=%d ... ", test_CS, test_MOSI, test_MISO, test_SCK);

          // Ripuliamo lo stato dei pin e dello SPI per essere sicuri
          pinMode(10, INPUT_PULLUP);
          pinMode(11, INPUT_PULLUP);
          pinMode(12, INPUT_PULLUP);
          pinMode(13, INPUT_PULLUP);

          spi.end();
          delay(10);
          spi.begin(test_SCK, test_MISO, test_MOSI, test_CS);
          delay(10);

          // Proviamo ad avviare la SD a bassa velocità per massima stabilità
          if (sd.begin(SdSpiConfig(test_CS, DEDICATED_SPI, SD_SCK_MHZ(1), &spi))) {
            Serial.println("SUCCESS! <<<====== WE FOUND IT!");
            Serial.print("Card Type: ");
            switch(sd.card()->type()) {
                case SD_CARD_TYPE_SD1: Serial.println("SD1"); break;
                case SD_CARD_TYPE_SD2: Serial.println("SD2"); break;
                case SD_CARD_TYPE_SDHC: Serial.println("SDHC/SDXC"); break;
                default: Serial.println("Unknown");
            }
            found = true;
            Serial.println("\n**************************************");
            Serial.println("CORRECT MAPPING:");
            Serial.printf("#define SD_CS   %d\n", test_CS);
            Serial.printf("#define SD_MOSI %d\n", test_MOSI);
            Serial.printf("#define SD_MISO %d\n", test_MISO);
            Serial.printf("#define SD_SCK  %d\n", test_SCK);
            Serial.println("**************************************\n");
            
            // Trovato! Ci fermiamo
            while(1) { delay(1000); }
          } else {
            Serial.println("Failed.");
          }
        }
      }
    }
  }

  if (!found) {
    Serial.println("\n--- SCAN COMPLETE ---");
    Serial.println("Nessuna delle 24 combinazioni ha funzionato.");
    Serial.println("Questo significa che il problema non e' solo l'inversione di due pin.");
    Serial.println("Cause probabili rimaste:");
    Serial.println("1. Un pin tra 10,11,12,13 non e' saldato o la pista e' tagliata.");
    Serial.println("2. GND o VCC sul lettore SD non sono collegati o saldati bene.");
    Serial.println("3. Il lettore MicroSD e' guasto o la scheda e' difettosa.");
  }
}

void loop() {
  delay(1000);
}