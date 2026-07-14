/*
  Arduino Uno + LCD1602(4位并口) + RC522 门禁显示版
  --------------------------------------------------
  开机:LCD 自检并显示 RFID 状态(OK = 模块通了;NOT FOUND = 接线/焊接问题)
  刷卡:认识的卡显示 "Welcome 名字",陌生卡显示 "Unknown card" + 卡号
  (LCD1602 只能显示英文/数字,显示不了中文 —— 硬件字库限制)

  接线:
    LCD: RS=8  E=7  D4=5  D5=4  D6=3  D7=2  V0=6(PWM对比度)
         VDD=5V  RW=GND  GND=GND  BLA=3.3V  BLK=GND
    RC522: SDA=10  SCK=13  MOSI=11  MISO=12  RST=9  VCC=3.3V  GND=GND
*/
#include <SPI.h>
#define MFRC522_SPICLOCK (250000u)   // SPI降速到250kHz:接线不完美时更稳(默认4MHz)
#include <MFRC522.h>
#include <LiquidCrystal.h>

#define SS_PIN 10
#define RST_PIN 9
#define V0_PIN 6          // LCD 对比度(PWM)
#define CONTRAST 40       // 字泡在黑块里=调大;字太淡看不见=调小(0~255)

LiquidCrystal lcd(8, 7, 5, 4, 3, 2);   // RS, E, D4, D5, D6, D7
MFRC522 rfid(SS_PIN, RST_PIN);

// ===== 白名单:刷一次卡,LCD会显示卡号,把卡号抄到这里、起个英文名字 =====
struct Card { const char* uid; const char* name; };
Card KNOWN[] = {
  {"a1b2c3d4", "Jerry"},   // <- 换成你的卡号和名字(名字用英文)
};
const int KNOWN_LEN = sizeof(KNOWN) / sizeof(KNOWN[0]);

String uidToString(MFRC522::Uid uid) {
  String s = "";
  for (byte i = 0; i < uid.size; i++) {
    if (uid.uidByte[i] < 0x10) s += "0";
    s += String(uid.uidByte[i], HEX);
  }
  s.toLowerCase();
  return s;
}

const char* lookupName(String uid) {
  for (int i = 0; i < KNOWN_LEN; i++)
    if (uid == KNOWN[i].uid) return KNOWN[i].name;
  return nullptr;
}

void setup() {
  Serial.begin(9600);
  pinMode(V0_PIN, OUTPUT);
  analogWrite(V0_PIN, CONTRAST);

  lcd.begin(16, 2);
  lcd.print("Access Control");

  SPI.begin();
  rfid.PCD_Init();
  delay(100);
}

bool rfidOk = false;   // 通了之后才进入刷卡模式

void loop() {
  // ===== 实时自检模式:每0.5秒测一次,屏幕实时显示 =====
  // 用手按压/晃动每根线和排针,盯着屏幕:哪个动作让它变OK,问题就在哪
  if (!rfidOk) {
    byte v = rfid.PCD_ReadRegister(MFRC522::VersionReg);
    Serial.print("VersionReg=0x"); Serial.println(v, HEX);
    lcd.setCursor(0, 1);
    if (v == 0x00 || v == 0xFF) {
      lcd.print("RFID:NOT FOUND! ");
    } else {
      rfidOk = true;
      rfid.PCD_Init();          // 通了,正式初始化一次
      delay(50);
      lcd.clear();
      lcd.print("RFID OK! v=0x");
      lcd.print(v, HEX);
      lcd.setCursor(0, 1);
      lcd.print("Scan card...");
    }
    delay(500);
    return;
  }

  // ===== 刷卡模式 =====
  // 每3秒复查一次连接稳定性,掉线立刻回到自检模式并报告
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck > 3000) {
    lastCheck = millis();
    byte v = rfid.PCD_ReadRegister(MFRC522::VersionReg);
    Serial.print("心跳 VersionReg=0x"); Serial.println(v, HEX);
    if (v == 0x00 || v == 0xFF) {
      rfidOk = false;
      lcd.clear();
      lcd.print("RFID LOST!");
      return;
    }
  }
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;

  String uid = uidToString(rfid.uid);
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  Serial.println("UID: " + uid);

  lcd.clear();
  const char* name = lookupName(uid);
  if (name) {
    lcd.print("Welcome ");
    lcd.print(name);
  } else {
    lcd.print("Unknown card");
  }
  lcd.setCursor(0, 1);
  lcd.print(uid);          // 第二行永远显示卡号,方便你抄下来填白名单

  delay(2500);
  lcd.clear();
  lcd.print("Scan card...");
}
