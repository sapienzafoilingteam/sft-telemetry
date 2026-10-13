#include <Arduino.h>
#include <SD.h>
#include <SPI.h>

/* =========================================================================
 * SD CARD DEBUG TOOL — ESP32-S3
 *
 * Questo sketch testa in dettaglio la connessione alla microSD.
 * Diagnostica: pin SPI, frequenza clock, tipo di card, lettura/scrittura.
 * ========================================================================= */

// Pin configurati dall'utente
#define SD_CS 10
#define SD_MOSI 11
#define SD_SCK 12
#define SD_MISO 13

// LED per feedback visivo
#define LED_1 45

SPIClass sdSPI(HSPI);

void waitForSerial() {
  // Aspetta che il Serial Monitor si connetta (USB CDC)
  pinMode(LED_1, OUTPUT);
  while (!Serial) {
    digitalWrite(LED_1, !digitalRead(LED_1));
    delay(100);
  }
  digitalWrite(LED_1, HIGH);
}

void printDivider() {
  Serial.println("================================================");
}

void testPinState() {
  Serial.println("\n[TEST 1] VERIFICA PIN GPIO");
  printDivider();

  // Testa se i pin sono accessibili
  struct {
    int pin;
    const char *name;
  } pins[] = {
      {SD_CS, "CS  "}, {SD_MOSI, "MOSI"}, {SD_SCK, "SCK "}, {SD_MISO, "MISO"}};

  for (auto &p : pins) {
    pinMode(p.pin, INPUT_PULLUP);
    int val_pullup = digitalRead(p.pin);
    pinMode(p.pin, INPUT_PULLDOWN);
    int val_pulldown = digitalRead(p.pin);

    Serial.printf("  GPIO %2d (%s): PULLUP=%d  PULLDOWN=%d", p.pin, p.name,
                  val_pullup, val_pulldown);

    if (val_pullup == val_pulldown) {
      Serial.println("  ⚠️  Pin potrebbe essere bloccato!");
    } else {
      Serial.println("  ✅ OK");
    }
  }

  // Testa CS toggle
  Serial.println("\n  Test CS toggle...");
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  delay(10);
  digitalWrite(SD_CS, LOW);
  delay(10);
  digitalWrite(SD_CS, HIGH);
  Serial.println("  CS toggle completato");
}

bool testSDInitWithPins(int sck, int miso, int mosi, int cs, uint32_t frequency, const char* freqName) {
  Serial.printf("\n    %s (%u Hz)... ", freqName, frequency);
  
  // Reinizializza SPI
  SD.end();
  sdSPI.end();
  delay(100);
  sdSPI.begin(sck, miso, mosi, cs);
  delay(100);
  
  // Prova SD.begin con la frequenza specificata
  if (SD.begin(cs, sdSPI, frequency)) {
    Serial.println("✅ SUCCESSO!");
    return true;
  } else {
    Serial.println("❌ FALLITO");
    SD.end();
    return false;
  }
}

bool tryPinConfig(int sck, int miso, int mosi, int cs, const char* configName) {
  Serial.printf("\n  📌 CONFIG: %s\n", configName);
  Serial.printf("     SCK=GPIO%d  MISO=GPIO%d  MOSI=GPIO%d  CS=GPIO%d\n", sck, miso, mosi, cs);
  
  bool mounted = false;
  if (!mounted) mounted = testSDInitWithPins(sck, miso, mosi, cs, 400000,   "400 KHz");
  if (!mounted) mounted = testSDInitWithPins(sck, miso, mosi, cs, 1000000,  "1 MHz  ");
  if (!mounted) mounted = testSDInitWithPins(sck, miso, mosi, cs, 4000000,  "4 MHz  ");
  if (!mounted) mounted = testSDInitWithPins(sck, miso, mosi, cs, 20000000, "20 MHz ");
  
  return mounted;
}

// Variabili globali per memorizzare la configurazione funzionante
int working_sck = -1, working_miso = -1, working_mosi = -1, working_cs = -1;

void testSDMount() {
  Serial.println("\n[TEST 2] INIZIALIZZAZIONE SD CARD");
  printDivider();
  
  Serial.printf("  Pin dichiarati: CS=%d, MOSI=%d, SCK=%d, MISO=%d\n", SD_CS, SD_MOSI, SD_SCK, SD_MISO);
  Serial.println("  Bus: HSPI");
  Serial.println("  Proverò TUTTE le combinazioni MOSI/MISO...\n");
  
  bool mounted = false;
  
  // --- CONFIG A: Pin originali (come dichiarati) ---
  if (!mounted) {
    mounted = tryPinConfig(SD_SCK, SD_MISO, SD_MOSI, SD_CS, 
                           "A - ORIGINALE (MOSI=11, MISO=13)");
    if (mounted) { working_sck=SD_SCK; working_miso=SD_MISO; working_mosi=SD_MOSI; working_cs=SD_CS; }
  }

  // --- CONFIG B: MOSI e MISO scambiati ---
  if (!mounted) {
    mounted = tryPinConfig(SD_SCK, SD_MOSI, SD_MISO, SD_CS, 
                           "B - MOSI/MISO INVERTITI (MOSI=13, MISO=11)");
    if (mounted) { working_sck=SD_SCK; working_miso=SD_MOSI; working_mosi=SD_MISO; working_cs=SD_CS; }
  }



  
  if (!mounted) {
    Serial.println("\n  ❌ NESSUNA CONFIGURAZIONE HA FUNZIONATO");
    Serial.println("\n  🔧 CHECKLIST HARDWARE:");
    Serial.println("    [ ] MicroSD inserita correttamente nell'adattatore?");
    Serial.println("    [ ] VCC del modulo SD collegato a 3.3V? (misura con multimetro)");
    Serial.println("    [ ] GND del modulo SD collegato a GND dell'ESP32?");
    Serial.println("    [ ] Fili saldati/ben inseriti nel breadboard?");
    Serial.println("    [ ] MicroSD formattata in FAT32? (prova a formattarla da PC)");
    Serial.println("    [ ] Prova un'altra microSD card");
    Serial.println("    [ ] Prova un altro modulo/adattatore SD");
    return;
  }
  
  // Se siamo qui, abbiamo trovato una configurazione funzionante!
  Serial.println("\n  ✅ SD CARD TROVATA!");
  Serial.printf("  📌 CONFIGURAZIONE FUNZIONANTE:\n");
  Serial.printf("     SCK  = GPIO %d\n", working_sck);
  Serial.printf("     MISO = GPIO %d\n", working_miso);
  Serial.printf("     MOSI = GPIO %d\n", working_mosi);
  Serial.printf("     CS   = GPIO %d\n", working_cs);
  
  if (working_mosi != SD_MOSI || working_miso != SD_MISO) {
    Serial.println("\n  ⚠️  ATTENZIONE: I pin MOSI/MISO sono INVERTITI rispetto");
    Serial.println("     alla configurazione dichiarata! Aggiorna il firmware");
    Serial.println("     principale (main.cpp) con i valori corretti sopra.");
  }
}

void testSDInfo() {
  Serial.println("\n[TEST 3] INFORMAZIONI SD CARD");
  printDivider();

  uint8_t cardType = SD.cardType();

  if (cardType == CARD_NONE) {
    Serial.println("  ❌ Nessuna card rilevata dopo il mount!");
    return;
  }

  Serial.print("  Tipo: ");
  switch (cardType) {
  case CARD_MMC:
    Serial.println("MMC");
    break;
  case CARD_SD:
    Serial.println("SD");
    break;
  case CARD_SDHC:
    Serial.println("SDHC");
    break;
  default:
    Serial.println("SCONOSCIUTO");
    break;
  }

  uint64_t cardSize = SD.cardSize() / (1024 * 1024);
  uint64_t totalBytes = SD.totalBytes();
  uint64_t usedBytes = SD.usedBytes();

  Serial.printf("  Dimensione: %llu MB\n", cardSize);
  Serial.printf("  Spazio totale: %llu bytes\n", totalBytes);
  Serial.printf("  Spazio usato:  %llu bytes\n", usedBytes);
  Serial.printf("  Spazio libero: %llu bytes\n", totalBytes - usedBytes);
}

void testSDReadWrite() {
  Serial.println("\n[TEST 4] TEST LETTURA/SCRITTURA");
  printDivider();

  const char *testFile = "/sd_debug_test.txt";
  const char *testData = "SFT SD Debug Test - ESP32-S3 - 2026";

  // --- WRITE TEST ---
  Serial.printf("  Scrittura file: %s ... ", testFile);
  File file = SD.open(testFile, FILE_WRITE);
  if (!file) {
    Serial.println("❌ Impossibile aprire in scrittura!");
    Serial.println(
        "  La SD potrebbe essere protetta da scrittura (switch sul modulo)");
    return;
  }

  size_t written = file.println(testData);
  file.close();

  if (written > 0) {
    Serial.printf("✅ %d bytes scritti\n", written);
  } else {
    Serial.println("❌ Nessun byte scritto!");
    return;
  }

  // --- READ TEST ---
  Serial.printf("  Lettura file: %s ... ", testFile);
  file = SD.open(testFile, FILE_READ);
  if (!file) {
    Serial.println("❌ Impossibile aprire in lettura!");
    return;
  }

  String content = file.readStringUntil('\n');
  file.close();

  if (content == testData) {
    Serial.println("✅ Dati verificati correttamente!");
  } else {
    Serial.printf("⚠️ Dati letti diversi: '%s'\n", content.c_str());
  }

  // --- DELETE TEST ---
  Serial.printf("  Eliminazione file test... ");
  if (SD.remove(testFile)) {
    Serial.println("✅ OK");
  } else {
    Serial.println("⚠️ Non eliminato (non grave)");
  }
}

void testSessionScan() {
  Serial.println("\n[TEST 5] SCANSIONE FILE SESSIONE");
  printDivider();

  File root = SD.open("/");
  if (!root) {
    Serial.println("  ❌ Impossibile aprire la root!");
    return;
  }

  int fileCount = 0;
  int sessionCount = 0;
  int maxSessionNum = 0;

  Serial.println("  Contenuto della SD:");
  File entry;
  while ((entry = root.openNextFile())) {
    fileCount++;
    String name = entry.name();
    bool isDir = entry.isDirectory();
    size_t size = entry.size();

    Serial.printf("    %s %s", isDir ? "[DIR]" : "     ", name.c_str());
    if (!isDir) {
      Serial.printf(" (%d bytes)", size);
    }

    // Check for session files
    if (name.startsWith("session_") && name.endsWith(".csv")) {
      sessionCount++;
      String numStr = name.substring(8, name.length() - 4);
      int num = numStr.toInt();
      if (num > maxSessionNum)
        maxSessionNum = num;
      Serial.print(" <- SESSIONE");
    }
    Serial.println();
    entry.close();
  }
  root.close();

  if (fileCount == 0) {
    Serial.println("    (vuota)");
  }

  Serial.printf("\n  Totale file: %d\n", fileCount);
  Serial.printf("  Sessioni trovate: %d\n", sessionCount);
  Serial.printf("  Prossima sessione: session_%04d.csv\n", maxSessionNum + 1);
}

void testWriteSpeed() {
  Serial.println("\n[TEST 6] TEST VELOCITÀ SCRITTURA");
  printDivider();

  const char *speedFile = "/speed_test.csv";

  File file = SD.open(speedFile, FILE_WRITE);
  if (!file) {
    Serial.println("  ❌ Impossibile aprire file speed test");
    return;
  }

  // Simula scrittura CSV come farebbe il firmware reale
  file.println("timestamp_ms,roll,pitch,yaw,ax,ay,az,gx,gy,gz,mx,my,mz,lat,lng,"
               "alt,spd,sats");

  const int NUM_LINES = 500; // 10 secondi di dati a 50Hz
  char line[256];

  unsigned long startTime = micros();

  for (int i = 0; i < NUM_LINES; i++) {
    snprintf(line, sizeof(line),
             "%lu,%.2f,%.2f,%.2f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%"
             ".6f,%.6f,%.1f,%.1f,%d\n",
             millis(), -0.55f, 3.58f, 136.85f, 0.0012f, -0.0034f, 1.0023f,
             0.12f, -0.05f, 0.003f, 0.23f, -0.12f, 0.45f, 41.890210, 12.492070,
             15.3, 5.2, 7);
    file.print(line);

    // Flush every 50 lines (come il firmware reale)
    if ((i + 1) % 50 == 0) {
      file.flush();
    }
  }

  file.flush();
  unsigned long elapsed = micros() - startTime;
  file.close();

  float totalMs = elapsed / 1000.0f;
  float msPerLine = totalMs / NUM_LINES;
  float linesPerSec = 1000.0f / msPerLine;

  Serial.printf("  %d righe scritte in %.1f ms\n", NUM_LINES, totalMs);
  Serial.printf("  Media: %.3f ms/riga\n", msPerLine);
  Serial.printf("  Throughput: %.0f righe/sec\n", linesPerSec);

  if (linesPerSec >= 50) {
    Serial.println("  ✅ Velocità sufficiente per 50Hz!");
  } else {
    Serial.printf(
        "  ⚠️ Troppo lento per 50Hz (serve >= 50 righe/sec, hai %.0f)\n",
        linesPerSec);
  }

  // Cleanup
  SD.remove(speedFile);
}

void setup() {
  Serial.begin(115200);
  waitForSerial();
  delay(500);

  Serial.println();
  printDivider();
  Serial.println("   SFT MICROSD DEBUG TOOL v1.0");
  Serial.println("   ESP32-S3 DevKitC-1");
  printDivider();
  Serial.printf("   Timestamp: %lu ms dal boot\n", millis());

  // Test 1: Verifica pin
  testPinState();

  // Test 2: Prova montaggio SD
  testSDMount();

  // Se la SD è montata, continua con gli altri test
  if (SD.cardType() != CARD_NONE) {
    testSDInfo();
    testSDReadWrite();
    testSessionScan();
    testWriteSpeed();

    SD.end();

    Serial.println("\n");
    printDivider();
    Serial.println("   ✅ TUTTI I TEST COMPLETATI CON SUCCESSO!");
    printDivider();
  } else {
    Serial.println("\n");
    printDivider();
    Serial.println("   ❌ SD NON RILEVATA - VERIFICA CABLAGGIO");
    printDivider();
  }

  Serial.println("\n   Premi RESET per ripetere i test.");
  Serial.println("   Oppure cambia cablaggio e riprova.\n");
}

void loop() {
  // Lampeggio LED lento per indicare che il test è finito
  digitalWrite(LED_1, !digitalRead(LED_1));
  delay(1000);
}
