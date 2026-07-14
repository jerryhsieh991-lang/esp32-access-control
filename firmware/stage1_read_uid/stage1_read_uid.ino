/*
  阶段 1 — 读到卡号
  ------------------------------------------------------------
  只接 RC522(SPI)。烧录本程序后打开串口监视器(波特率 115200),
  把卡/钥匙扣靠近读卡器,串口会打印出卡号(UID)。

  接线(只需要这几根线):
    RC522 SDA(SS) -> ESP32 GPIO5
    RC522 SCK      -> ESP32 GPIO18
    RC522 MOSI     -> ESP32 GPIO23
    RC522 MISO     -> ESP32 GPIO19
    RC522 RST      -> ESP32 GPIO27
    RC522 VCC      -> ESP32 3.3V   (注意!不要接 5V,会烧坏模块)
    RC522 GND      -> ESP32 GND

  *** 重要:请把打印出来的卡号抄下来,阶段 2 的白名单要用到它。***
*/

#include <SPI.h>
#include <MFRC522.h>

#define SS_PIN 5
#define RST_PIN 27

MFRC522 rfid(SS_PIN, RST_PIN);

// 把一张卡的 UID 转成小写、每字节两位十六进制的字符串,例如 "a1b2c3d4"。
// 注意:String(byte, HEX) 遇到小于 0x10 的字节会丢掉前导 0(比如 0x0a 会
// 打印成 "a" 而不是 "0a"),所以这里手动补零,保证格式统一。
// 阶段2/3/4 的白名单比对都要用这个同样的函数,否则会对不上。
String uidToString(MFRC522::Uid uid) {
  String s = "";
  for (byte i = 0; i < uid.size; i++) {
    if (uid.uidByte[i] < 0x10) s += "0";   // 补前导零
    s += String(uid.uidByte[i], HEX);
  }
  s.toLowerCase();                          // 强制小写,格式统一
  return s;
}

void setup() {
  Serial.begin(115200);
  delay(300);

  SPI.begin(18, 19, 23, 5);   // 参数顺序: SCK, MISO, MOSI, SS
  rfid.PCD_Init();

  Serial.println();
  Serial.println("=== 阶段1: 读卡测试 ===");
  Serial.println("刷卡试试... (把卡片靠近 RC522 模块)");
}

void loop() {
  // 没有新卡就直接返回,继续等待
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) {
    return;
  }

  String uid = uidToString(rfid.uid);
  Serial.print("卡号: ");
  Serial.println(uid);
  Serial.println("↑ 把这个卡号抄下来,阶段2要用!");

  // 读完一张卡后要停掉,避免卡在天线范围内时反复触发
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  delay(800);   // 简单防抖:一次刷卡只打印一次
}
