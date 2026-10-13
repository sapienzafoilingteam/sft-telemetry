#include <Arduino.h>
#include <SPI.h>

#define SD_CS 10
#define SD_MOSI 11
#define SD_SCK 12
#define SD_MISO 13

SPIClass spi(FSPI);

void setup() {
  Serial.begin(115200);
  delay(2000);

  spi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);

  Serial.println("SPI test");

  while (true) {
    uint8_t b = spi.transfer(0xFF);
    Serial.printf("%02X ", b);
    delay(500);
  }
}

void loop() {}