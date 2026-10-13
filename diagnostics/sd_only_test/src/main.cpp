#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include "SdFat.h"
#include <driver/gpio.h>

// Pins
#define SD_CS   10
#define SD_MOSI 11
#define SD_SCK  12
#define SD_MISO 13

#define LED_PIN 21

// We declare the SdFat objects
SdFat sd;

// (Software SPI test rimosso poiché richiede SPI_DRIVER_SELECT in SdFatConfig.h)

// -------------------------------------------------------------
// TEST 1: Standard SD.h Library (FSPI)
// -------------------------------------------------------------
void runStandardSDTest() {
  Serial.println("\n===============================================");
  Serial.println("      TEST 1: Standard SD.h Library (FSPI)");
  Serial.println("===============================================");
  
  SD.end();
  delay(100);
  
  SPIClass spiFSPI(FSPI);
  spiFSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  
  // Disabilitiamo i pull-up software
  // gpio_pullup_en((gpio_num_t)SD_MISO);
  // gpio_pullup_en((gpio_num_t)SD_MOSI);
  // gpio_pullup_en((gpio_num_t)SD_SCK);
  // gpio_pullup_en((gpio_num_t)SD_CS);

  // Aumentiamo la Drive Strength al massimo (CAP_3 = ~40mA) per sovrastare capacità parassite
  gpio_set_drive_capability((gpio_num_t)SD_MISO, GPIO_DRIVE_CAP_3);
  gpio_set_drive_capability((gpio_num_t)SD_MOSI, GPIO_DRIVE_CAP_3);
  gpio_set_drive_capability((gpio_num_t)SD_SCK,  GPIO_DRIVE_CAP_3);
  gpio_set_drive_capability((gpio_num_t)SD_CS,   GPIO_DRIVE_CAP_3);
  delay(100);

  Serial.println("  Initializing SD.h at 100 kHz (Slow Mode)...");
  if (SD.begin(SD_CS, spiFSPI, 100000)) {
    Serial.println("  [SUCCESS] SD.h initialized!");
    SD.end();
  } else {
    Serial.println("  [FAIL] SD.h initialization failed.");
  }
}

// -------------------------------------------------------------
// TEST AUSILIARIO: Lettura dettagli fisici della Card (CID/CSD)
// -------------------------------------------------------------
void runCardDetails() {
  Serial.println("\n-----------------------------------------------");
  Serial.println("   INFO DETTAGLIATE: Registri CID e CSD");
  Serial.println("-----------------------------------------------");
  
  if (!sd.card() || sd.card()->isBusy()) {
    Serial.println("  [FAIL] La scheda non è inizializzata o è occupata.");
    return;
  }

  // Tipo di scheda
  Serial.print("  Tipo di scheda: ");
  switch (sd.card()->type()) {
    case SD_CARD_TYPE_SD1: Serial.println("SD1 (Standard)"); break;
    case SD_CARD_TYPE_SD2: Serial.println("SD2 (Standard v2)"); break;
    case SD_CARD_TYPE_SDHC: Serial.println("SDHC/SDXC (Alta Capacità)"); break;
    default: Serial.println("Sconosciuto");
  }

  // File System
  Serial.print("  File System: FAT");
  Serial.println((int)sd.vol()->fatType());

  // Calcolo Capacità
  uint32_t clusterCount = sd.vol()->clusterCount();
  uint32_t sectorsPerCluster = sd.vol()->sectorsPerCluster();
  
  Serial.print("  Calcolo dello spazio libero (può richiedere qualche secondo)... ");
  uint32_t freeClusters = sd.vol()->freeClusterCount();
  Serial.println("Fatto.");
  
  uint64_t totalSize = (uint64_t)clusterCount * sectorsPerCluster * 512;
  uint64_t freeSize = (uint64_t)freeClusters * sectorsPerCluster * 512;

  Serial.printf("  Capacità Totale: %.2f GB (%llu bytes)\n", (double)totalSize / (1024.0*1024.0*1024.0), totalSize);
  Serial.printf("  Spazio Libero:   %.2f GB (%llu bytes)\n", (double)freeSize / (1024.0*1024.0*1024.0), freeSize);

  // Registro CID (Card Identification)
  cid_t cid;
  if (sd.card()->readCID(&cid)) {
    Serial.printf("  Manufacturer ID (MID): 0x%02X\n", cid.mid);
    Serial.printf("  OEM ID (OID):          %c%c\n", cid.oid[0], cid.oid[1]);
    Serial.print("  Product Name (PNM):    ");
    for (int i = 0; i < 5; i++) Serial.print((char)cid.pnm[i]);
    Serial.println();
    Serial.printf("  Product Revision:      %d.%d\n", cid.prvN(), cid.prvM());
    Serial.printf("  Serial Number (PSN):   0x%08X\n", cid.psn());
    Serial.printf("  Data di Fabbricazione:  %d/%d\n", cid.mdtMonth(), cid.mdtYear());
  } else {
    Serial.println("  [FAIL] Impossibile leggere il registro CID.");
  }
}

// -------------------------------------------------------------
// TEST AUSILIARIO: Scrittura e Verifica Integrità Dati
// -------------------------------------------------------------
void runWriteVerifyTest() {
  Serial.println("\n-----------------------------------------------");
  Serial.println("   VERIFICA SCRITTURA & INTEGRITÀ DATI");
  Serial.println("-----------------------------------------------");
  
  const char* filename = "test_int.bin";
  if (sd.exists(filename)) {
    sd.remove(filename);
  }
  
  FsFile file = sd.open(filename, O_WRITE | O_CREAT);
  if (!file) {
    Serial.println("  [FAIL] Impossibile creare il file di test.");
    return;
  }
  
  const int dataSize = 4096; // 4 KB
  uint8_t bufferWrite[dataSize];
  for (int i = 0; i < dataSize; i++) {
    bufferWrite[i] = (uint8_t)(i % 256);
  }
  
  Serial.println("  Scrittura di 4KB con pattern incrementale...");
  unsigned long startWrite = millis();
  int written = file.write(bufferWrite, dataSize);
  file.close();
  unsigned long endWrite = millis();
  
  if (written != dataSize) {
    Serial.printf("  [FAIL] Scritti solo %d di %d bytes!\n", written, dataSize);
    return;
  }
  Serial.printf("  [SUCCESS] Scritto con successo in %lu ms\n", endWrite - startWrite);
  
  // Riapertura e verifica
  file = sd.open(filename, O_READ);
  if (!file) {
    Serial.println("  [FAIL] Impossibile aprire il file per la verifica.");
    return;
  }
  
  uint8_t bufferRead[dataSize];
  Serial.println("  Lettura e confronto dei dati...");
  unsigned long startRead = millis();
  int readBytes = file.read(bufferRead, dataSize);
  file.close();
  unsigned long endRead = millis();
  
  if (readBytes != dataSize) {
    Serial.printf("  [FAIL] Letti solo %d di %d bytes!\n", readBytes, dataSize);
    return;
  }
  Serial.printf("  [SUCCESS] Letto con successo in %lu ms\n", endRead - startRead);
  
  bool match = true;
  for (int i = 0; i < dataSize; i++) {
    if (bufferRead[i] != bufferWrite[i]) {
      Serial.printf("  [CORRUZIONE] Errore al byte %d! Atteso 0x%02X, letto 0x%02X\n", i, bufferWrite[i], bufferRead[i]);
      match = false;
      break;
    }
  }
  
  if (match) {
    Serial.println("  [SUCCESS] Verifica dati completata! Nessun errore rilevato.");
  } else {
    Serial.println("  [FAIL] I dati sul file sono diversi da quelli inviati!");
  }
  
  sd.remove(filename);
}

// -------------------------------------------------------------
// TEST AUSILIARIO: Benchmark di Velocità e Latenza
// -------------------------------------------------------------
void runSpeedBenchmark() {
  Serial.println("\n-----------------------------------------------");
  Serial.println("   BENCHMARK VELOCITÀ & LATENZA DI SCRITTURA");
  Serial.println("-----------------------------------------------");
  
  const char* filename = "bench.dat";
  if (sd.exists(filename)) {
    sd.remove(filename);
  }
  
  FsFile file = sd.open(filename, O_WRITE | O_CREAT);
  if (!file) {
    Serial.println("  [FAIL] Impossibile aprire file per benchmark.");
    return;
  }
  
  const size_t bufSize = 512; // 1 settore standard
  uint8_t buf[bufSize];
  memset(buf, 0xAA, bufSize);
  
  const uint32_t numBlocks = 2048; // 1 MB in totale
  uint32_t maxWriteTime = 0;
  uint32_t minWriteTime = 999999;
  uint32_t totalWriteTime = 0;
  
  Serial.printf("  Scrittura di 1MB (%d blocchi da %d byte)...\n", numBlocks, bufSize);
  
  unsigned long startBench = millis();
  for (uint32_t i = 0; i < numBlocks; i++) {
    unsigned long startBlock = micros();
    size_t w = file.write(buf, bufSize);
    unsigned long blockTime = micros() - startBlock;
    
    if (w != bufSize) {
      Serial.printf("  Errore di scrittura al blocco %d!\n", i);
      file.close();
      return;
    }
    
    if (blockTime > maxWriteTime) maxWriteTime = blockTime;
    if (blockTime < minWriteTime) minWriteTime = blockTime;
    totalWriteTime += blockTime;
  }
  
  unsigned long startSync = micros();
  file.sync(); // Forza il salvataggio fisico su Flash
  unsigned long syncTime = micros() - startSync;
  file.close();
  
  unsigned long endBench = millis();
  float writeSpeed = 1024.0 / ((endBench - startBench) / 1000.0);
  
  Serial.printf("  Velocità Scrittura:   %.2f KB/s\n", writeSpeed);
  Serial.printf("  Latenza Minima blocco: %lu us\n", minWriteTime);
  Serial.printf("  Latenza Massima blocco: %lu us (Picco di Latenza!)\n", maxWriteTime);
  Serial.printf("  Latenza Media blocco:   %lu us\n", totalWriteTime / numBlocks);
  Serial.printf("  Tempo di File Sync:    %lu us\n", syncTime);
  
  // Benchmark Lettura
  file = sd.open(filename, O_READ);
  if (!file) {
    Serial.println("  [FAIL] Impossibile aprire file per benchmark lettura.");
    return;
  }
  
  Serial.println("  Lettura di 1MB...");
  uint32_t totalReadTime = 0;
  unsigned long startReadBench = millis();
  
  for (uint32_t i = 0; i < numBlocks; i++) {
    unsigned long startBlock = micros();
    int r = file.read(buf, bufSize);
    unsigned long blockTime = micros() - startBlock;
    
    if (r != bufSize) {
      Serial.printf("  Errore di lettura al blocco %d!\n", i);
      file.close();
      return;
    }
    totalReadTime += blockTime;
  }
  file.close();
  unsigned long endReadBench = millis();
  float readSpeed = 1024.0 / ((endReadBench - startReadBench) / 1000.0);
  
  Serial.printf("  Velocità Lettura:     %.2f KB/s\n", readSpeed);
  Serial.printf("  Latenza Media lettura: %lu us\n", totalReadTime / numBlocks);
  
  sd.remove(filename);
}

// -------------------------------------------------------------
// TEST AUSILIARIO: Stress Test Simulando Telemetria a 50Hz
// -------------------------------------------------------------
void runSimulatedTelemetryStressTest() {
  Serial.println("\n-----------------------------------------------");
  Serial.println("   STRESS TEST TELEMETRIA SIMULATA (50Hz / 20ms)");
  Serial.println("-----------------------------------------------");
  
  const char* filename = "telem.csv";
  if (sd.exists(filename)) {
    sd.remove(filename);
  }
  
  FsFile file = sd.open(filename, O_WRITE | O_CREAT);
  if (!file) {
    Serial.println("  [FAIL] Impossibile creare il file csv.");
    return;
  }
  
  file.println("timestamp,ax,ay,az,gx,gy,gz,mx,my,mz,lat,lon,alt");
  
  Serial.println("  Simulazione logging dati IMU + GPS a 50Hz per 10 secondi...");
  Serial.println("  (Scrittura di circa 100 byte ogni 20ms. Monitoraggio sforamenti)");
  
  int totalSamples = 500; // 50 Hz * 10 s
  int violations = 0;
  uint32_t maxWriteTime = 0;
  
  unsigned long testStart = millis();
  
  for (int i = 0; i < totalSamples; i++) {
    unsigned long sampleStart = millis();
    
    // Generiamo una riga CSV fittizia di circa 100 byte
    char telemetryRow[120];
    snprintf(telemetryRow, sizeof(telemetryRow), "%lu,0.12,0.34,-9.81,0.01,-0.02,0.05,123.4,456.7,-789.0,41.890251,12.492373,25.4\n", sampleStart);
    
    unsigned long writeStart = micros();
    file.print(telemetryRow);
    
    // Per ottimizzare la durata dei cicli, eseguiamo il sync sulla SD ogni 10 righe (200ms)
    // invece che ad ogni singola riga.
    if (i % 10 == 0) {
      file.sync();
    }
    
    unsigned long writeDuration = micros() - writeStart;
    
    if (writeDuration > maxWriteTime) {
      maxWriteTime = writeDuration;
    }
    
    // Controlliamo se abbiamo sforato la finestra temporale di 20ms
    unsigned long sampleDuration = millis() - sampleStart;
    if (sampleDuration > 20) {
      violations++;
      Serial.printf("  [SFORAMENTO] Campione %d: Scrittura + Sync impiegato %lu ms (>20ms!)\n", i, sampleDuration);
    }
    
    // Calcoliamo il delay per mantenere la frequenza a 50Hz (20ms)
    long delayTime = 20 - (millis() - sampleStart);
    if (delayTime > 0) {
      delay(delayTime);
    }
  }
  
  file.close();
  unsigned long testEnd = millis();
  
  Serial.printf("\n  Stress Test Concluso in %lu ms:\n", testEnd - testStart);
  Serial.printf("  - Campioni totali registrati: %d\n", totalSamples);
  Serial.printf("  - Tempo massimo singola scrittura (incluso sync): %lu us\n", maxWriteTime);
  Serial.printf("  - Sforamenti temporali (>20ms): %d (%.2f%% del totale)\n", violations, (float)violations/totalSamples*100.0);
  
  if (violations == 0) {
    Serial.println("  [SUCCESS] La scheda SD risponde entro i tempi per logging a 50Hz!");
  } else {
    Serial.println("  [WARNING] Rilevati sforamenti di tempo! Si consiglia una coda RAM (FreeRTOS) per non bloccare l'IMU.");
  }
  
  sd.remove(filename);
}

// -------------------------------------------------------------
// TEST 2: SdFat Library (Hardware SPI)
// -------------------------------------------------------------
void runSdFatHardwareSPITest() {
  Serial.println("\n===============================================");
  Serial.println("   TEST 2: SdFat Library (Hardware SPI)");
  Serial.println("===============================================");
  
  SD.end();
  delay(100);

  SPI.end();
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  
  // gpio_pullup_en((gpio_num_t)SD_MISO);
  // gpio_pullup_en((gpio_num_t)SD_MOSI);
  // gpio_pullup_en((gpio_num_t)SD_SCK);
  // gpio_pullup_en((gpio_num_t)SD_CS);
  
  gpio_set_drive_capability((gpio_num_t)SD_MISO, GPIO_DRIVE_CAP_3);
  gpio_set_drive_capability((gpio_num_t)SD_MOSI, GPIO_DRIVE_CAP_3);
  gpio_set_drive_capability((gpio_num_t)SD_SCK,  GPIO_DRIVE_CAP_3);
  gpio_set_drive_capability((gpio_num_t)SD_CS,   GPIO_DRIVE_CAP_3);
  delay(100);

  // Configure hardware SPI on SdFat
  SdSpiConfig hwConfig(SD_CS, DEDICATED_SPI, SD_SCK_MHZ(1)); 
  
  Serial.println("  Initializing SdFat Hardware SPI @ 1 MHz...");
  if (sd.begin(hwConfig)) {
    Serial.println("  [SUCCESS] SdFat Hardware SPI inizializzata!");
    
    // Eseguiamo tutti i sotto-test diagnostici avanzati
    runCardDetails();
    runWriteVerifyTest();
    runSpeedBenchmark();
    runSimulatedTelemetryStressTest();
  } else {
    Serial.println("  [FAIL] SdFat Hardware SPI fallita.");
    sd.initErrorPrint(&Serial);
  }
}



// -------------------------------------------------------------
// TEST 6: Sweep Frequenze Hardware SPI
// -------------------------------------------------------------
void runFrequencySweepTest() {
  Serial.println("\n===============================================");
  Serial.println("   TEST 6: Sweep delle frequenze Hardware SPI");
  Serial.println("===============================================");
  
  // Testiamo a diverse frequenze (in MHz) per trovare il limite fisico del cablaggio
  uint8_t testSpeeds[] = {1, 4, 8, 16, 20, 24};
  
  for (uint8_t speed : testSpeeds) {
    Serial.printf("  Inizializzazione SPI a %d MHz... ", speed);
    
    SD.end();
    SPI.end();
    delay(100);
    
    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    SdSpiConfig config(SD_CS, DEDICATED_SPI, SD_SCK_MHZ(speed));
    
    if (sd.begin(config)) {
      // Breve test di scrittura per confermare la stabilità elettrica del bus a quella velocità
      FsFile file = sd.open("speed_t.txt", O_WRITE | O_CREAT);
      if (file) {
        file.println("test");
        file.close();
        
        file = sd.open("speed_t.txt", O_READ);
        if (file) {
          char buf[10];
          int bytes = file.read(buf, sizeof(buf));
          file.close();
          sd.remove("speed_t.txt");
          
          if (bytes > 0) {
            Serial.println("[SUCCESS]");
          } else {
            Serial.println("[FAIL - Lettura vuota]");
          }
        } else {
          Serial.println("[FAIL - Riapertura Lettura]");
        }
      } else {
        Serial.println("[FAIL - Scrittura]");
      }
    } else {
      Serial.println("[FAIL - Inizializzazione]");
    }
    delay(100);
  }
}
// -------------------------------------------------------------
// TEST 0: Diagnostica GPIO a basso livello
// -------------------------------------------------------------
void runGPIODiagnostic() {
  Serial.println("\n===============================================");
  Serial.println("   TEST 0: DIAGNOSTICA GPIO (Cablaggio Fisico)");
  Serial.println("===============================================");

  struct PinInfo {
    int pin;
    const char* name;
  };

  PinInfo pins[] = {
    {SD_CS,   "CS   (GPIO 10)"},
    {SD_MOSI, "MOSI (GPIO 11)"},
    {SD_SCK,  "SCK  (GPIO 12)"},
    {SD_MISO, "MISO (GPIO 13)"}
  };

  // Test 0a: Lettura stato pin senza configurazione (floating)
  Serial.println("\n  [0a] Stato pin in modalità INPUT (floating):");
  for (auto& p : pins) {
    pinMode(p.pin, INPUT);
    delay(10);
    int val = digitalRead(p.pin);
    Serial.printf("    %s = %d %s\n", p.name, val, val ? "(HIGH)" : "(LOW)");
  }

  // Test 0b: Lettura con pull-up interno
  Serial.println("\n  [0b] Stato pin con INPUT_PULLUP:");
  for (auto& p : pins) {
    pinMode(p.pin, INPUT_PULLUP);
    delay(10);
    int val = digitalRead(p.pin);
    Serial.printf("    %s = %d %s\n", p.name, val, val ? "(HIGH - pull-up OK)" : "(LOW - qualcosa trascina il pin a GND!)");
  }

  // Test 0c: Output HIGH, poi leggi (verifica che il pin non sia in corto a GND)
  Serial.println("\n  [0c] Test OUTPUT HIGH su ogni pin:");
  for (auto& p : pins) {
    pinMode(p.pin, OUTPUT);
    digitalWrite(p.pin, HIGH);
    delay(10);
    // Rileggiamo
    int val = digitalRead(p.pin);
    Serial.printf("    %s -> scritto HIGH, letto %d %s\n", p.name, val,
                  val ? "(OK)" : "(PROBLEMA: il pin non sale a HIGH! Possibile corto a GND)");
    digitalWrite(p.pin, LOW);
    pinMode(p.pin, INPUT);
  }

  // Test 0d: Verifica tensione su 3.3V — il MISO è l'unico pin che la SD guida,
  // quindi con la SD alimentata ma inattiva dovrebbe essere alto (pull-up)
  Serial.println("\n  [0d] Test pull-up sul pin MISO (unico pin guidato dalla SD):");
  pinMode(SD_MISO, INPUT_PULLUP);
  delay(50);
  int miso = digitalRead(SD_MISO);
  Serial.printf("    MISO con pull-up = %d\n", miso);
  if (miso == 0) {
    Serial.println("    >> MISO bloccato a LOW. Verificare:");
    Serial.println("       - Il modulo SD è alimentato? (VCC -> 3.3V, GND -> GND)");
    Serial.println("       - La microSD è inserita nel modulo?");
    Serial.println("       - Il pin MISO (GPIO 13) è collegato al pin DO del modulo?");
  }

  // Reset tutti i pin a INPUT
  for (auto& p : pins) {
    pinMode(p.pin, INPUT);
  }

  Serial.println("\n  Diagnostica GPIO completata.\n");
}

// -------------------------------------------------------------
// TEST EXTRA: Tentativo con Pull-up Software abilitati
// -------------------------------------------------------------
void runSDWithPullups() {
  Serial.println("\n===============================================");
  Serial.println("   TEST EXTRA: SD.h con Pull-Up Software");
  Serial.println("===============================================");
  
  SD.end();
  delay(100);
  
  // Abilita pull-up software su tutti i pin SPI
  gpio_pullup_en((gpio_num_t)SD_MISO);
  gpio_pullup_en((gpio_num_t)SD_MOSI);
  gpio_pullup_en((gpio_num_t)SD_SCK);
  gpio_pullup_en((gpio_num_t)SD_CS);
  delay(100);
  
  SPIClass spiFSPI(FSPI);
  spiFSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  
  gpio_set_drive_capability((gpio_num_t)SD_MISO, GPIO_DRIVE_CAP_3);
  gpio_set_drive_capability((gpio_num_t)SD_MOSI, GPIO_DRIVE_CAP_3);
  gpio_set_drive_capability((gpio_num_t)SD_SCK,  GPIO_DRIVE_CAP_3);
  gpio_set_drive_capability((gpio_num_t)SD_CS,   GPIO_DRIVE_CAP_3);
  delay(100);

  Serial.println("  Initializing SD.h at 100 kHz con Pull-Up...");
  if (SD.begin(SD_CS, spiFSPI, 100000)) {
    Serial.println("  [SUCCESS] SD.h con pull-up inizializzata!");
    SD.end();
  } else {
    Serial.println("  [FAIL] SD.h con pull-up fallita.");
  }
}

// -------------------------------------------------------------
// Setup & Loop
// -------------------------------------------------------------
void setup() {
  Serial.begin(115200);

  pinMode(LED_PIN, OUTPUT);
  for (int i = 0; i < 15; i++) {
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    delay(100);
  }
  digitalWrite(LED_PIN, LOW);

  Serial.println("\n===============================================");
  Serial.println("  ESP32-S3 COMPREHENSIVE SD LIBRARY TESTER");
  Serial.println("===============================================");

  // TEST 0: Diagnostica GPIO
  runGPIODiagnostic();
  delay(500);

  // Eseguiamo il Test 1 (SD.h base)
  runStandardSDTest();
  delay(1000);

  // Test extra: con pull-up software
  runSDWithPullups();
  delay(1000);

  // Eseguiamo il Test 2 (SdFat hardware + benchmark/stress test/details)
  runSdFatHardwareSPITest();
  delay(1000);

  // Eseguiamo il Test 6 (Sweep di frequenze)
  runFrequencySweepTest();

  Serial.println("\n===============================================");
  Serial.println("             ALL TESTS COMPLETED");
  Serial.println("===============================================");
}

void loop() {
  digitalWrite(LED_PIN, HIGH);
  delay(500);
  digitalWrite(LED_PIN, LOW);
  delay(500);
}
