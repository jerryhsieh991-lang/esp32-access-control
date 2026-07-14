/*
  阶段 2 — 白名单 + LED
  ------------------------------------------------------------
  在阶段1的基础上加两个 LED。刷到白名单里的卡 -> 绿灯亮 1.5 秒;
  刷到陌生卡 -> 红灯亮 1.5 秒。

  接线(在阶段1基础上新增):
    绿 LED  正极(经 220Ω 电阻) -> ESP32 GPIO32,负极 -> GND
    红 LED  正极(经 220Ω 电阻) -> ESP32 GPIO33,负极 -> GND

  *** 使用前:把下面 WHITELIST 里的 "a1b2c3d4" 换成你阶段1打印的卡号! ***
*/

#include <SPI.h>
#include <MFRC522.h>

#define SS_PIN 5
#define RST_PIN 27
#define GREEN_PIN 32
#define RED_PIN 33

MFRC522 rfid(SS_PIN, RST_PIN);

// 白名单:允许通过的卡号列表,必须是小写、每字节两位十六进制,
// 格式要和阶段1串口打印出来的一致(阶段1的 uidToString 函数)。
// 换成你阶段1打印的卡号
String WHITELIST[] = {"a1b2c3d4"};
const int WHITELIST_LEN = sizeof(WHITELIST) / sizeof(WHITELIST[0]);

// 与阶段1完全相同的格式化函数,保证比对时格式一致
String uidToString(MFRC522::Uid uid) {
  String s = "";
  for (byte i = 0; i < uid.size; i++) {
    if (uid.uidByte[i] < 0x10) s += "0";
    s += String(uid.uidByte[i], HEX);
  }
  s.toLowerCase();
  return s;
}

bool isAllowed(String uid) {
  for (int i = 0; i < WHITELIST_LEN; i++) {
    if (WHITELIST[i] == uid) return true;
  }
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(300);

  SPI.begin(18, 19, 23, 5);   // SCK, MISO, MOSI, SS
  rfid.PCD_Init();

  pinMode(GREEN_PIN, OUTPUT);
  pinMode(RED_PIN, OUTPUT);
  digitalWrite(GREEN_PIN, LOW);
  digitalWrite(RED_PIN, LOW);

  Serial.println();
  Serial.println("=== 阶段2: 白名单 + LED ===");
  Serial.println("刷卡试试... 认识的卡绿灯,陌生卡红灯");
}

void loop() {
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) {
    return;
  }

  String uid = uidToString(rfid.uid);
  Serial.print("卡号: ");
  Serial.print(uid);

  if (isAllowed(uid)) {
    Serial.println(" -> 白名单内,绿灯亮");
    digitalWrite(GREEN_PIN, HIGH);
    delay(1500);
    digitalWrite(GREEN_PIN, LOW);
  } else {
    Serial.println(" -> 陌生卡,红灯亮");
    digitalWrite(RED_PIN, HIGH);
    delay(1500);
    digitalWrite(RED_PIN, LOW);
  }

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  delay(300);   // 简单防抖
}
