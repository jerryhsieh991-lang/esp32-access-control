/*
  按压测试:锁定 SS=14 SCK=23 MOSI=4 MISO=22,
  每秒软SPI读版本100次,打印成功率。
  测试时用手按压 RC522 的排针,看成功率是否随按压变化。
*/
#include <Arduino.h>

#define PIN_SS 14
#define PIN_SCK 23
#define PIN_MOSI 4
#define PIN_MISO 22

const int HOLD_HIGH[] = {5, 13, 16, 17, 18, 19, 21, 25, 26, 27, 32, 33};

byte softRead() {
  digitalWrite(PIN_SS, LOW); delayMicroseconds(10);
  byte addr = 0x80 | (0x37 << 1);
  for (int i = 7; i >= 0; i--) {
    digitalWrite(PIN_MOSI, (addr >> i) & 1);
    digitalWrite(PIN_SCK, HIGH); delayMicroseconds(4);
    digitalWrite(PIN_SCK, LOW);  delayMicroseconds(4);
  }
  byte val = 0;
  for (int i = 7; i >= 0; i--) {
    digitalWrite(PIN_SCK, HIGH); delayMicroseconds(4);
    if (digitalRead(PIN_MISO)) val |= (1 << i);
    digitalWrite(PIN_SCK, LOW);  delayMicroseconds(4);
  }
  digitalWrite(PIN_SS, HIGH);
  return val;
}

bool valid(byte v) {
  return v == 0x88 || v == 0x89 || v == 0x90 || v == 0x91 || v == 0x92 || v == 0xB2;
}

void setup() {
  Serial.begin(115200);
  delay(300);
  for (int p : HOLD_HIGH) { pinMode(p, OUTPUT); digitalWrite(p, HIGH); }
  pinMode(PIN_SS, OUTPUT); pinMode(PIN_SCK, OUTPUT);
  pinMode(PIN_MOSI, OUTPUT); pinMode(PIN_MISO, INPUT);
  digitalWrite(PIN_SS, HIGH); digitalWrite(PIN_SCK, LOW);
  delay(50);
  Serial.println("\n=== 按压测试:用手把RC522排针往板上按紧,看成功率变化 ===");
}

void loop() {
  int ok = 0; byte last = 0;
  for (int i = 0; i < 100; i++) {
    byte v = softRead();
    if (valid(v)) { ok++; last = v; }
    delayMicroseconds(300);
  }
  if (ok > 0) Serial.printf("成功率 %3d/100   版本=0x%02X   %s\n", ok, last,
                            ok > 90 ? "<<< 稳了!" : "<<< 时通时断");
  else Serial.println("成功率   0/100   (不通)");
  delay(400);
}
