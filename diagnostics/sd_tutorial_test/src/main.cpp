#include <Arduino.h>
#include "FS.h"
#include "SD.h"
#include "SPI.h"

// Definisci i pin corretti per il tuo ESP32-S3 e modulo SD
#define SCK  12
#define MISO 13
#define MOSI 11
#define CS   10

SPIClass spi(FSPI);

void printDir(const char *dirname, uint8_t levels = 0) {
  File root = SD.open(dirname);
  if (!root || !root.isDirectory()) return;

  File file = root.openNextFile();
  while (file) {
    for (int i = 0; i < levels; i++) Serial.print("  ");

    if (file.isDirectory()) {
      Serial.printf("[%s]\n", file.name());
      printDir(file.path(), levels + 1);
    } else {
      Serial.printf("%s (%d)\n", file.name(), file.size());
    }
    file = root.openNextFile();
  }
}

void writeFile(const char *path, const char *text) {
  File file = SD.open(path, FILE_WRITE);
  if (!file) {
    Serial.println("Failed to open file for writing");
    return;
  }
  if (file.print(text)) {
    Serial.println("File written successfully");
  } else {
    Serial.println("Write failed");
  }
  file.close();
}

void appendFile(const char *path, const char *text) {
  File file = SD.open(path, FILE_APPEND);
  if (!file) {
    Serial.println("Failed to open file for appending");
    return;
  }
  if (file.print(text)) {
    Serial.println("Message appended successfully");
  } else {
    Serial.println("Append failed");
  }
  file.close();
}

void printFile(const char *path) {
  File file = SD.open(path);
  if (!file) {
    Serial.println("Failed to open file for reading");
    return;
  }

  while (file.available()) {
    Serial.write(file.read());
  }
  file.close();
  Serial.println(); // Aggiunge una riga vuota alla fine
}

void setup() {
  Serial.begin(115200);
  delay(2000); // Wait for Serial Monitor

  Serial.println("\n===============================================");
  Serial.println("       TUTORIAL SD CARD ESP32 TEST");
  Serial.println("===============================================\n");

  // Configurazione dei pin SPI (abilitazione pull-up interni)
  pinMode(MISO, INPUT_PULLUP);
  pinMode(MOSI, INPUT_PULLUP);
  pinMode(SCK, INPUT_PULLUP);
  pinMode(CS, INPUT_PULLUP);

  spi.begin(SCK, MISO, MOSI, CS);
  
  // Proviamo prima a 1 MHz (1000000) invece che alla massima velocità
  Serial.println("Tentativo di montaggio scheda a 1 MHz...");
  if (!SD.begin(CS, spi, 1000000)) {
    Serial.println("Card Mount Failed! Controlla:");
    Serial.println("1. Cablaggio: SCK=12, MISO=13, MOSI=11, CS=10.");
    Serial.println("2. Alimentazione: Se il modulo ha un regolatore a bordo (spesso c'è scritto 5V o VCC), prova ad alimentarlo dal pin 5V (VIN) dell'ESP32 e non dal 3.3V.");
    Serial.println("3. Formattazione: L'ESP32 supporta solo FAT32 (no exFAT, NTFS). Le SD > 32GB di default sono exFAT.");
    return;
  }
  Serial.println("SD Card Initialized successfully");

  // 1. Leggi il tipo di carta SD
  uint8_t cardType = SD.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("No SD card attached");
    return;
  } else if (cardType == CARD_MMC) {
    Serial.println("Tipo di scheda: MMC Card");
  } else if (cardType == CARD_SD) {
    Serial.println("Tipo di scheda: Standard SD Card");
  } else if (cardType == CARD_SDHC) {
    Serial.println("Tipo di scheda: SDHC Card");
  } else {
    Serial.println("Tipo di scheda: Unknown Card Type");
  }

  // 2. Info Spazio
  uint64_t MB = 1024 * 1024;
  Serial.printf("SD Card Size: %lluMB\n", SD.cardSize() / MB);
  Serial.printf("Total space: %lluMB\n", SD.totalBytes() / MB);
  Serial.printf("Used space: %lluMB\n", SD.usedBytes() / MB);

  Serial.println("\n--- Testing File Operations ---");

  const char* path = "/readme.txt";

  Serial.println("1. Writing to file...");
  writeFile(path, "This is the readme\n");

  Serial.println("2. Appending to file...");
  appendFile(path, "This is more to read\n");

  Serial.println("3. Reading file content:");
  printFile(path);

  Serial.println("\n4. Current Directory Listing (Root):");
  printDir("/");

  Serial.println("\n===============================================");
  Serial.println("                 TEST FINITO");
  Serial.println("===============================================\n");
}

void loop() {
  // Niente da fare nel loop
}
