#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <Wire.h>

// Same mapping as the main firmware. No card writes or formatting.
constexpr int CS_PIN=10, MOSI_PIN=11, SCK_PIN=12, MISO_PIN=13;
SPIClass bus(FSPI);
bool softwareSPI=false;

uint8_t transferByte(uint8_t value) {
  if (!softwareSPI) return bus.transfer(value);
  uint8_t result=0;
  for (int bit=7; bit>=0; --bit) {
    digitalWrite(MOSI_PIN, (value >> bit) & 1);
    delayMicroseconds(20);
    digitalWrite(SCK_PIN, HIGH);
    delayMicroseconds(20);
    result=(result << 1) | digitalRead(MISO_PIN);
    digitalWrite(SCK_PIN, LOW);
  }
  return result;
}

void deselect() {
  digitalWrite(CS_PIN, HIGH);
  transferByte(0xFF);
}

uint8_t command(uint8_t cmd, uint32_t arg, uint8_t crc) {
  digitalWrite(CS_PIN, LOW);
  transferByte(0xFF);
  transferByte(0x40 | cmd);
  transferByte(arg >> 24);
  transferByte(arg >> 16);
  transferByte(arg >> 8);
  transferByte(arg);
  transferByte(crc);
  uint8_t response=0xFF;
  for (int i=0; i<16; ++i) {
    response=transferByte(0xFF);
    if (!(response & 0x80)) break;
  }
  return response;
}

void rawTest(bool soft, uint32_t frequency) {
  SD.end();
  bus.end();
  softwareSPI=soft;
  pinMode(CS_PIN, OUTPUT);
  digitalWrite(CS_PIN,HIGH);
  pinMode(MISO_PIN,INPUT_PULLUP);
  if (soft) {
    pinMode(MOSI_PIN,OUTPUT);
    digitalWrite(MOSI_PIN,HIGH);
    pinMode(SCK_PIN,OUTPUT);
    digitalWrite(SCK_PIN,LOW);
  } else {
    bus.begin(SCK_PIN,MISO_PIN,MOSI_PIN,-1);
    bus.beginTransaction(SPISettings(frequency,MSBFIRST,SPI_MODE0));
  }
  delay(100);
  Serial.printf("\nRAW %s clock=%lu Hz (software approx 12500 Hz)\n",soft?"BITBANG":"HARDWARE",(unsigned long)frequency);
  Serial.print("CS HIGH received bytes:");
  for (int i=0;i<16;++i) Serial.printf(" %02X",transferByte(0xFF));
  Serial.println();
  uint8_t r1=0xFF;
  for (int retry=0;retry<5;++retry) {
    r1=command(0,0,0x95);
    deselect();
    Serial.printf("CMD0 attempt=%d R1=0x%02X expected=0x01\n",retry+1,r1);
    if (r1==1) break;
    delay(100);
  }
  if (r1==1) {
    r1=command(8,0x1AA,0x87);
    Serial.printf("CMD8 R1=0x%02X",r1);
    if (r1==1) {
      Serial.print(" R7=");
      for (int i=0;i<4;++i) Serial.printf("%02X ",transferByte(0xFF));
    }
    Serial.println();
    deselect();
  }
  if (!soft) bus.endTransaction();
  bus.end();
}

void runTests() {
  Serial.println("\n=== SD USB PROBE: NO FILE WRITES / NO FORMAT ===");
  Serial.printf("CS=%d MOSI=%d SCK=%d MISO=%d\n",CS_PIN,MOSI_PIN,SCK_PIN,MISO_PIN);
  SD.end(); bus.end();
  pinMode(CS_PIN,OUTPUT); digitalWrite(CS_PIN,HIGH);
  pinMode(SCK_PIN,OUTPUT); digitalWrite(SCK_PIN,LOW);
  pinMode(MOSI_PIN,OUTPUT); digitalWrite(MOSI_PIN,HIGH);
  for (int mode=0;mode<3;++mode) {
    pinMode(MISO_PIN,mode==0?INPUT:mode==1?INPUT_PULLUP:INPUT_PULLDOWN);
    delay(10);
    int highs=0;
    for(int i=0;i<100;++i) { highs+=digitalRead(MISO_PIN); delayMicroseconds(100); }
    Serial.printf("MISO CS HIGH mode=%s high_samples=%d/100\n",mode==0?"FLOAT":mode==1?"PULLUP":"PULLDOWN",highs);
  }
  rawTest(false,100000);
  rawTest(false,400000);
  rawTest(true,12500);
  softwareSPI=false;
  pinMode(CS_PIN,OUTPUT); digitalWrite(CS_PIN,HIGH);
  bus.begin(SCK_PIN,MISO_PIN,MOSI_PIN,-1);
  pinMode(MISO_PIN,INPUT_PULLUP);
  Serial.println("\nSD.begin: mount only, requested data clock 400kHz; library logs follow");
  if(SD.begin(CS_PIN,bus,400000,"/sd",5,false)) {
    Serial.printf("MOUNT PASS cardType=%u cardBytes=%llu totalBytes=%llu\n",SD.cardType(),SD.cardSize(),SD.totalBytes());
    File root=SD.open("/");
    Serial.printf("ROOT READ %s\n",root?"PASS":"FAIL");
    root.close();
  } else Serial.println("MOUNT FAIL");
  SD.end(); bus.end();
  Serial.println("\nI2C scan SDA=8 SCL=9");
  Wire.begin(8,9,100000);
  Wire.setTimeOut(20);
  int found=0;
  for(int addr=1;addr<127;++addr) {
    Wire.beginTransmission(addr);
    if(Wire.endTransmission()==0) { Serial.printf("I2C device 0x%02X\n",addr); ++found; }
  }
  Wire.end();
  Serial.printf("I2C devices=%d\n=== TEST COMPLETE; send R to repeat ===\n",found);
}

void setup() {
  pinMode(CS_PIN,OUTPUT); digitalWrite(CS_PIN,HIGH);
  Serial.begin(115200);
  while(!Serial) delay(10);
  delay(1000);
  runTests();
}
void loop() {
  if(Serial.available()) {
    char c=Serial.read();
    if(c=='r'||c=='R') runTests();
  }
  delay(10);
}
