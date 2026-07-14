/*
  阶段 4 — 整合版(核心功能:读卡 + 白名单 + 舵机 + 蜂鸣器)
  ------------------------------------------------------------
  这是把阶段1~4整合到一起的完整门禁逻辑:
    读卡 -> 判断是否在白名单
      是 -> 绿灯亮 + 舵机开锁(3秒)+ 自动回锁
      否 -> 红灯亮 + 蜂鸣器"嘀嘀"两声报警

  完整接线(阶段1~4全部接上):
    RC522  SDA(SS) -> GPIO5    RC522 SCK  -> GPIO18
    RC522  MOSI     -> GPIO23  RC522 MISO -> GPIO19
    RC522  RST      -> GPIO27  RC522 VCC  -> 3.3V(不要接5V!) RC522 GND -> GND
    舵机   信号(橙) -> GPIO13   舵机 VCC(红) -> 5V(VIN)  舵机 GND(棕) -> GND
    绿LED  +(经220Ω) -> GPIO32  负极 -> GND
    红LED  +(经220Ω) -> GPIO33  负极 -> GND
    有源蜂鸣器 + -> GPIO25      - -> GND

  *** 使用前:把下面 WHITELIST 里的 "a1b2c3d4" 换成你阶段1打印的卡号! ***
*/

#include <SPI.h>
#include <MFRC522.h>
#include <ESP32Servo.h>

// ---------- 引脚定义 ----------
#define SS_PIN 5
#define RST_PIN 27
#define GREEN_PIN 32
#define RED_PIN 33
#define BUZZ_PIN 25
#define SERVO_PIN 13

// ---------- 参数 ----------
#define LOCKED_ANGLE 0     // 0°  = 锁
#define OPEN_ANGLE 90      // 90° = 开
#define UNLOCK_MS 3000     // 开锁保持时间
#define BEEP_MS 120        // 蜂鸣器单次响/停时长

MFRC522 rfid(SS_PIN, RST_PIN);
Servo lock;

// 换成你阶段1打印的卡号,可以加多张卡,用逗号隔开
String WHITELIST[] = {"a1b2c3d4"};
const int WHITELIST_LEN = sizeof(WHITELIST) / sizeof(WHITELIST[0]);

// 与阶段1完全相同的格式化函数(小写、每字节两位十六进制、补零),
// 全部阶段必须用同一个函数,否则白名单比对会对不上。
String uidToString(MFRC522::Uid uid) {
  String s = "";
  for (byte i = 0; i < uid.size; i++) {
    if (uid.uidByte[i] < 0x10) s += "0";   // 补前导零
    s += String(uid.uidByte[i], HEX);
  }
  s.toLowerCase();
  return s;
}

// 读卡:没有新卡返回空字符串;读到卡则返回卡号,并在函数内部
// 完成 HaltA / StopCrypto1,调用方不用再管。
String readUID() {
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) {
    return "";
  }
  String uid = uidToString(rfid.uid);
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  return uid;
}

// 判断卡号是否在白名单里
bool isAllowed(String uid) {
  for (int i = 0; i < WHITELIST_LEN; i++) {
    if (WHITELIST[i] == uid) return true;
  }
  return false;
}

// 放行:绿灯亮 + 舵机开锁3秒 + 自动回锁
void grantAccess(String uid) {
  Serial.println("卡号 " + uid + " -> 白名单内,开锁");
  digitalWrite(GREEN_PIN, HIGH);
  lock.write(OPEN_ANGLE);
  delay(UNLOCK_MS);
  lock.write(LOCKED_ANGLE);
  digitalWrite(GREEN_PIN, LOW);
}

// 拒绝:红灯亮 + 蜂鸣器响两声报警
void denyAccess(String uid) {
  Serial.println("卡号 " + uid + " -> 陌生卡,拒绝");
  digitalWrite(RED_PIN, HIGH);
  for (int i = 0; i < 2; i++) {
    digitalWrite(BUZZ_PIN, HIGH);
    delay(BEEP_MS);
    digitalWrite(BUZZ_PIN, LOW);
    delay(BEEP_MS);
  }
  digitalWrite(RED_PIN, LOW);
}

void setup() {
  Serial.begin(115200);
  delay(300);

  SPI.begin(18, 19, 23, 5);   // SCK, MISO, MOSI, SS
  rfid.PCD_Init();

  pinMode(GREEN_PIN, OUTPUT);
  pinMode(RED_PIN, OUTPUT);
  pinMode(BUZZ_PIN, OUTPUT);
  digitalWrite(GREEN_PIN, LOW);
  digitalWrite(RED_PIN, LOW);
  digitalWrite(BUZZ_PIN, LOW);

  lock.attach(SERVO_PIN);
  lock.write(LOCKED_ANGLE);   // 上电先锁上

  Serial.println();
  Serial.println("=== 阶段4: 门禁整合版 ===");
  Serial.println("刷卡试试... 白名单卡开锁,陌生卡报警");
}

void loop() {
  String uid = readUID();
  if (uid == "") return;   // 没读到卡,继续等

  Serial.print("卡号: ");
  Serial.println(uid);

  if (isAllowed(uid)) {
    grantAccess(uid);
  } else {
    denyAccess(uid);
  }

  delay(300);   // 简单防抖:一次刷卡只处理一次
}
