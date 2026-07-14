/*
  鉴别版 v2:SS=14 SCK=23 MOSI=4 MISO=22
  1) 软SPI慢速连续探测300次,统计读到的值的分布 -> 判断线路是否稳定
  2) 硬件SPI降速到400kHz再试正规驱动
*/
#include <SPI.h>
#define MFRC522_SPICLOCK (400000u)   // 硬件SPI降到400kHz(默认4MHz)
#include <MFRC522.h>

#define PIN_SS 14
#define PIN_SCK 23
#define PIN_MOSI 4
#define PIN_MISO 22

const int HOLD_HIGH[] = {5, 13, 16, 17, 18, 21, 25, 26, 27, 32, 33};

byte softRead() {
  pinMode(PIN_SS, OUTPUT); pinMode(PIN_SCK, OUTPUT);
  pinMode(PIN_MOSI, OUTPUT); pinMode(PIN_MISO, INPUT);
  digitalWrite(PIN_SS, HIGH); digitalWrite(PIN_SCK, LOW);
  delayMicroseconds(20);
  digitalWrite(PIN_SS, LOW); delayMicroseconds(10);
  byte addr = 0x80 | (0x37 << 1);
  for (int i = 7; i >= 0; i--) {
    digitalWrite(PIN_MOSI, (addr >> i) & 1);
    digitalWrite(PIN_SCK, HIGH); delayMicroseconds(3);
    digitalWrite(PIN_SCK, LOW);  delayMicroseconds(3);
  }
  byte val = 0;
  for (int i = 7; i >= 0; i--) {
    digitalWrite(PIN_SCK, HIGH); delayMicroseconds(3);
    if (digitalRead(PIN_MISO)) val |= (1 << i);
    digitalWrite(PIN_SCK, LOW);  delayMicroseconds(3);
  }
  digitalWrite(PIN_SS, HIGH);
  return val;
}

void setup() {
  Serial.begin(115200);
  delay(300);
  for (int p : HOLD_HIGH) { pinMode(p, OUTPUT); digitalWrite(p, HIGH); }
  delay(50);
}

void loop() {
  // --- 1) 软SPI 300次统计 ---
  int n92 = 0, n00 = 0, nFF = 0, nOther = 0;
  for (int i = 0; i < 300; i++) {
    byte v = softRead();
    if (v == 0x92 || v == 0x91 || v == 0x88 || v == 0x90) n92++;
    else if (v == 0x00) n00++;
    else if (v == 0xFF) nFF++;
    else nOther++;
    delayMicroseconds(200);
  }
  Serial.printf("[软SPI x300] 有效版本号:%d  全0:%d  全1:%d  乱码:%d  -> %s\n",
    n92, n00, nFF, nOther,
    n92 > 280 ? "线路稳定!" : (n92 > 0 ? "接触不良(时通时断)" : "不通"));

  // --- 2) 硬件SPI 400kHz ---
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_SS);
  MFRC522 rfid(PIN_SS, MFRC522::UNUSED_PIN);
  rfid.PCD_Init();
  byte v = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  Serial.printf("[硬SPI 400kHz] VersionReg = 0x%02X\n\n", v);
  SPI.end();

  delay(3000);
}
