/*
  阶段 3 — 舵机开锁
  ------------------------------------------------------------
  在阶段2基础上加舵机当"锁舌"。认识的卡 -> 绿灯亮 + 舵机转到90°(开锁),
  等3秒后自动转回0°(上锁)。陌生卡 -> 红灯亮 1.5 秒(和阶段2一样)。

  接线(在阶段2基础上新增):
    舵机 信号线(橙) -> ESP32 GPIO13
    舵机 VCC(红)    -> 5V(VIN)   (舵机耗电大,别用ESP32的3.3V供电)
    舵机 GND(棕)    -> GND        (一定要和ESP32共地)

  *** 使用前:把下面 WHITELIST 里的 "a1b2c3d4" 换成你阶段1打印的卡号! ***
*/

#include <SPI.h>
#include <MFRC522.h>
#include <ESP32Servo.h>

#define SS_PIN 5
#define RST_PIN 27
#define GREEN_PIN 32
#define RED_PIN 33
#define SERVO_PIN 13

#define LOCKED_ANGLE 0     // 0°  = 锁
#define OPEN_ANGLE 90      // 90° = 开
#define UNLOCK_MS 3000     // 开锁保持时间

MFRC522 rfid(SS_PIN, RST_PIN);
Servo lock;

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

  lock.attach(SERVO_PIN);
  lock.write(LOCKED_ANGLE);   // 上电先锁上

  Serial.println();
  Serial.println("=== 阶段3: 舵机开锁 ===");
  Serial.println("刷卡试试... 认识的卡开锁3秒后自动锁上,陌生卡红灯");
}

void loop() {
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) {
    return;
  }

  String uid = uidToString(rfid.uid);
  Serial.print("卡号: ");
  Serial.print(uid);

  if (isAllowed(uid)) {
    Serial.println(" -> 白名单内,开锁");
    digitalWrite(GREEN_PIN, HIGH);
    lock.write(OPEN_ANGLE);     // 开锁
    delay(UNLOCK_MS);
    lock.write(LOCKED_ANGLE);   // 自动回锁
    digitalWrite(GREEN_PIN, LOW);
  } else {
    Serial.println(" -> 陌生卡,拒绝");
    digitalWrite(RED_PIN, HIGH);
    delay(1500);
    digitalWrite(RED_PIN, LOW);
  }

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  delay(300);   // 简单防抖
}
