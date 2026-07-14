/*
  RC522 接线诊断:读版本寄存器,判断 SPI 连接是否正常。
  0x91 / 0x92 / 0x88 = 模块正常;0x00 或 0xFF = 接线不对/没上电。
*/
#include <SPI.h>
#include <MFRC522.h>

#define SS_PIN 5
#define RST_PIN 22   // RST 已从 27 挪到 22

MFRC522 rfid(SS_PIN, RST_PIN);

void setup() {
  Serial.begin(115200);
  delay(300);
  SPI.begin(18, 19, 23, 5);
  rfid.PCD_Init();
}

void loop() {
  byte v = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  Serial.printf("VersionReg = 0x%02X  -> %s\n", v,
    (v == 0x00 || v == 0xFF) ? "连接失败:检查接线/供电!" : "RC522 连接正常");

  // 额外线索:空闲时把 MISO(GPIO19)挂内部上拉读电平。
  // 一直是 0 = MISO 那根线大概率被拉死或短路;是 1 = 线悬空/模块没响应。
  SPI.end();
  pinMode(19, INPUT_PULLUP);
  delayMicroseconds(50);
  Serial.printf("MISO 空闲电平(带上拉) = %d\n", digitalRead(19));
  SPI.begin(18, 19, 23, 5);
  rfid.PCD_Init();
  delay(2000);
}
