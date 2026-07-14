/*
  Arduino Uno + LCD1602(4位并口) + RC522 + 软时钟 + ESP32桥 —— 门禁记录版
  --------------------------------------------------------------
  在 uno_lcd_rfid 的基础上新增:
    1) 软时钟:不用RTC硬件!时间由 T 命令喂入(Mac的logger或ESP32的NTP),
       millis()走时,重启后自动广播"求对表"(CLOCK,WAITING)
    2) 待机屏幕:第一行 "Scan card...",第二行显示当前时间,每秒刷新一次
    3) 认识的卡:LCD显示 "Welcome 名字" + 时间,串口打印一行 CSV 日志
    4) 陌生卡:LCD显示 "Access denied" + 卡号,串口打印 DENY 日志
    5) EEPROM 环形日志:掉电不丢失,容量约200条,存满后自动覆盖最旧的记录
    6) 串口发 'd' 可以把整个日志导出成CSV;发 'c' 可以清空日志
  本文件保留了 uno_lcd_rfid.ino 里跑通的所有硬件细节(SPI降速、LCD接线、
  对比度PWM、RC522的SS/RST脚位、自检+掉线检测逻辑),没有改动那些部分。
  uno_lcd_rfid.ino 本身不做任何修改,继续作为"能跑通"的备份版本。

  新增接线(ESP32桥,可选——不接也能跑,只是没WiFi直传):
    Uno A1(TX) --[220Ω]--+--[440Ω(两个220串联)]-- GND     (5V降到3.3V的分压)
                          +--> ESP32 GPIO16 (RX2)
    Uno A0(RX) <-- ESP32 GPIO17 (TX2) 直连
    Uno 5V --> ESP32 VIN;  GND 共地
    (LCD 和 RC522 接线跟 uno_lcd_rfid.ino 完全一样;DS1302已退役,拆掉)
*/
#include <SPI.h>
#define MFRC522_SPICLOCK (250000u)   // SPI降速到250kHz:仿板RC522只有这个速度才稳定,不要删
#include <MFRC522.h>
#include <LiquidCrystal.h>
#include <RTClib.h>          // 只用它的 DateTime 类做时间换算
#include <SoftwareSerial.h>  // 第二串口:和 ESP32 桥对话(A0=收 A1=发)
#include <EEPROM.h>
#include <avr/wdt.h>     // 看门狗:程序卡死4秒自动重启,防止"屏幕停住要手动重插"

#define SS_PIN 10
#define RST_PIN 9
#define V0_PIN 6          // LCD 对比度(PWM)
#define CONTRAST 40       // 字泡在黑块里=调大;字太淡看不见=调小(0~255)

LiquidCrystal lcd(8, 7, 5, 4, 3, 2);   // RS, E, D4, D5, D6, D7
MFRC522 rfid(SS_PIN, RST_PIN);

// ESP32 桥(负责 WiFi 上传 + NTP 授时):A0=RX(接ESP32的GPIO17) A1=TX(经分压接ESP32的GPIO16)
SoftwareSerial espSerial(A0, A1);   // RX, TX

// 软时钟:不再用DS1302硬件!时间由 T 命令喂进来(Mac的logger 或 ESP32的NTP),
// 之后用 millis() 自己走时。ESP32每小时重新对一次表,误差可忽略。
uint32_t syncUnix = 0;        // 最近一次对表的时间(0=还没对过表,显示 no clock)
unsigned long syncMs = 0;     // 对表那一刻的 millis()

// ===== 白名单:真实卡号在 secrets.h(不进git)——复制 secrets.h.example 填你的卡 =====
struct Card { const char* uid; const char* name; };
#include "secrets.h"   // 定义 KNOWN[](数组下标=EEPROM里的卡编号,不要随便调换顺序)
const int KNOWN_LEN = sizeof(KNOWN) / sizeof(KNOWN[0]);
const uint8_t DENIED_IDX = 255;   // 陌生卡在日志里用这个编号表示

// ===== 状态变量 =====
bool rfidOk = false;   // RC522 通了之后才进入刷卡模式(自检逻辑跟原版一样)

bool idleShown = false;          // 待机屏幕的第一行是否已经画好
#define TIME_BUF_LEN 15   // "07-13 10:25PM" 13字符 + 结尾,留点余量
char lastIdleTime[TIME_BUF_LEN] = "";  // 上一次画在第二行的时间字符串,变了才重画,防止闪烁

// =========================================================
// 卡号转字符串:两位小写十六进制拼起来,跟原版完全一样
// =========================================================
String uidToString(MFRC522::Uid uid) {
  String s = "";
  for (byte i = 0; i < uid.size; i++) {
    if (uid.uidByte[i] < 0x10) s += "0";
    s += String(uid.uidByte[i], HEX);
  }
  s.toLowerCase();
  return s;
}

// 在白名单里查这张卡,找到返回下标(0/1/2),没找到返回 -1
int lookupCard(const String &uid) {
  for (int i = 0; i < KNOWN_LEN; i++) {
    if (uid == KNOWN[i].uid) return i;
  }
  return -1;
}

// 日志里卡编号 -> 名字("DENIED"表示陌生卡,查不到表示数据坏了)
const char* nameForIndex(uint8_t idx) {
  if (idx == DENIED_IDX) return "DENIED";
  if (idx < KNOWN_LEN) return KNOWN[idx].name;
  return "UNKNOWN";
}

// =========================================================
// 时间相关:没接RTC或者RTC没通,一律返回 "no clock",不让整机卡死
// =========================================================
// 同时算出 unix 时间戳(给EEPROM/日志用)和给LCD看的短字符串,
// 只读一次RTC,省I2C次数
void getNowInfo(uint32_t &ts, char *buf, size_t len) {
  if (syncUnix > 0) {
    ts = syncUnix + (millis() - syncMs) / 1000UL;   // 软时钟:对表点 + 已流逝秒数
    DateTime now(ts);
    // 12小时制 + AM/PM
    uint8_t h = now.hour();
    bool pm = (h >= 12);
    uint8_t h12 = h % 12;
    if (h12 == 0) h12 = 12;
    snprintf(buf, len, "%02d-%02d %d:%02d%s", now.month(), now.day(), h12, now.minute(), pm ? "PM" : "AM");
    return;
  }
  ts = 0;
  strncpy(buf, "no clock", len);
  buf[len - 1] = '\0';
}

// =========================================================
// EEPROM 环形日志
// 布局: [0]magic  [1-2]head(下一条要写的位置,uint16)  [3-4]count(有效条数,uint16)
//       [5...] 一条条5字节的记录: 4字节时间戳(uint32) + 1字节卡编号
// 容量200条,占用 5 + 200*5 = 1005 字节,Uno的1024字节EEPROM够用
// 存满后从头覆盖最旧的一条(环形缓冲区),用 EEPROM.put/update 只写有变化的字节,省寿命
// =========================================================
#define EE_MAGIC 0xA5
const int EE_ADDR_MAGIC = 0;
const int EE_ADDR_HEAD = 1;
const int EE_ADDR_COUNT = 3;
const int EE_ADDR_RECORDS = 5;
const int EE_RECORD_SIZE = 5;
const uint16_t EE_CAPACITY = 200;

uint16_t eeHead = 0;    // 下一次要写入的槽位
uint16_t eeCount = 0;   // 当前有效记录条数(最多到EE_CAPACITY)

void eepromInit() {
  uint8_t magic = EEPROM.read(EE_ADDR_MAGIC);
  if (magic != EE_MAGIC) {
    // 第一次用,或者数据是坏的:清空重来
    eeHead = 0;
    eeCount = 0;
    EEPROM.update(EE_ADDR_MAGIC, EE_MAGIC);
    EEPROM.put(EE_ADDR_HEAD, eeHead);
    EEPROM.put(EE_ADDR_COUNT, eeCount);
    return;
  }
  EEPROM.get(EE_ADDR_HEAD, eeHead);
  EEPROM.get(EE_ADDR_COUNT, eeCount);
  if (eeHead >= EE_CAPACITY || eeCount > EE_CAPACITY) {
    // 数值超出合理范围,说明数据被破坏了,保险起见重置
    eeHead = 0;
    eeCount = 0;
    EEPROM.put(EE_ADDR_HEAD, eeHead);
    EEPROM.put(EE_ADDR_COUNT, eeCount);
  }
}

void eepromAppend(uint32_t ts, uint8_t cardIdx) {
  int addr = EE_ADDR_RECORDS + (long)eeHead * EE_RECORD_SIZE;
  EEPROM.put(addr, ts);              // put内部逐字节用update写,值没变就不会真的擦写
  EEPROM.update(addr + 4, cardIdx);
  eeHead = (eeHead + 1) % EE_CAPACITY;
  if (eeCount < EE_CAPACITY) eeCount++;
  EEPROM.put(EE_ADDR_HEAD, eeHead);
  EEPROM.put(EE_ADDR_COUNT, eeCount);
}

void eepromClear() {
  eeHead = 0;
  eeCount = 0;
  EEPROM.update(EE_ADDR_MAGIC, EE_MAGIC);
  EEPROM.put(EE_ADDR_HEAD, eeHead);
  EEPROM.put(EE_ADDR_COUNT, eeCount);
}

// 把日志从最老的一条开始,逐条打印成CSV,最后打印 DUMP,END
void eepromDump() {
  uint16_t startSlot = (eeCount < EE_CAPACITY) ? 0 : eeHead;
  for (uint16_t i = 0; i < eeCount; i++) {
    uint16_t slot = (startSlot + i) % EE_CAPACITY;
    int addr = EE_ADDR_RECORDS + (long)slot * EE_RECORD_SIZE;
    uint32_t ts;
    EEPROM.get(addr, ts);
    uint8_t idx = EEPROM.read(addr + 4);

    Serial.print(F("DUMP,"));
    Serial.print(i);
    Serial.print(',');
    Serial.print(ts);
    Serial.print(',');
    if (ts == 0) {
      Serial.print('-');
    } else {
      DateTime dt(ts);
      char buf[20];
      snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
               dt.year(), dt.month(), dt.day(), dt.hour(), dt.minute(), dt.second());
      Serial.print(buf);
    }
    Serial.print(',');
    Serial.println(nameForIndex(idx));
  }
  Serial.println(F("DUMP,END"));
}

// 对表:记下"现在是几点"和"此刻的millis()",之后软时钟自己走
void setClock(uint32_t unixts) {
  syncUnix = unixts;
  syncMs = millis();
}

// 处理串口命令。full=true(USB串口)支持全部命令;full=false(ESP32口)只收对表
// 命令表:'T<unix>'=对表  'd'=导出日志  'c'=清空日志  't'=查时间
void handleCommands(Stream &s, bool full) {
  if (!s.available()) return;
  char c = s.read();
  if (c == 'T') {
    long v = s.parseInt();
    if (v > 1600000000L) {
      setClock((uint32_t)v);
      s.println(F("TIMESET,OK"));
    }
  } else if (!full) {
    return;                       // ESP32口只认对表命令,别的字符忽略
  } else if (c == 'd') {
    eepromDump();
  } else if (c == 'c') {
    eepromClear();
    Serial.println(F("CLEARED"));
  } else if (c == 't') {
    // 查询当前时间:TIME,<unix时间戳>,<可读时间>
    uint32_t ts;
    char t[TIME_BUF_LEN];
    getNowInfo(ts, t, sizeof(t));
    Serial.print(F("TIME,"));
    Serial.print(ts);
    Serial.print(',');
    Serial.println(t);
  }
}

// =========================================================
// 待机屏幕:第一行固定文字只画一次,第二行时间每秒check一次、变了才重画,避免闪烁
// force=true 用于"刚开机/刚刷完卡"这种需要强制整屏重画的场合
// =========================================================
void showIdle(bool force) {
  static unsigned long lastTick = 0;

  if (!idleShown || force) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(F("Scan card..."));
    idleShown = true;
    lastIdleTime[0] = '\0';   // 清空缓存,强制下面立刻重画时间
    lastTick = 0;
  }

  if (millis() - lastTick < 1000) return;   // 每秒才查一次时间,省I2C
  lastTick = millis();

  uint32_t ts;
  char t[TIME_BUF_LEN];
  getNowInfo(ts, t, sizeof(t));
  if (strcmp(t, lastIdleTime) != 0) {
    lcd.setCursor(0, 1);
    lcd.print(t);
    for (byte i = strlen(t); i < 16; i++) lcd.print(' ');  // 补空格盖掉上次留下的尾巴
    strncpy(lastIdleTime, t, TIME_BUF_LEN);
  }
}

// LCD(重新)初始化:被电气噪声打懵的屏,只有重跑begin才能救回来
void lcdInit() {
  analogWrite(V0_PIN, CONTRAST);
  lcd.begin(16, 2);
}

void setup() {
  wdt_disable();            // 先关看门狗,避免重启后在setup里连环复位
  Serial.begin(9600);
  pinMode(V0_PIN, OUTPUT);

  lcdInit();
  lcd.print(F("Access Control"));

  SPI.begin();
  rfid.PCD_Init();
  delay(100);

  espSerial.begin(9600);   // 通往 ESP32 桥的第二串口

  eepromInit();

  // 开机广播"求对表":Mac的logger或ESP32的NTP听到后会立刻喂 T 命令
  Serial.println(F("CLOCK,WAITING"));
  espSerial.println(F("CLOCK,WAITING"));

  wdt_enable(WDTO_4S);   // 看门狗上岗:loop卡死超过4秒就自动重启整机
}

void loop() {
  wdt_reset();   // 喂狗:只要loop还在转,就不会触发重启
  handleCommands(Serial, true);      // USB口(Mac):全部命令
  handleCommands(espSerial, false);  // ESP32口:只收对表

  // ===== 实时自检模式:跟原版一样,每0.5秒测一次,屏幕实时显示 =====
  if (!rfidOk) {
    byte v = rfid.PCD_ReadRegister(MFRC522::VersionReg);
    Serial.print(F("VersionReg=0x")); Serial.println(v, HEX);
    lcd.setCursor(0, 1);
    if (v == 0x00 || v == 0xFF) {
      lcd.print(F("RFID:NOT FOUND! "));
    } else {
      rfidOk = true;
      rfid.PCD_Init();          // 通了,正式初始化一次
      delay(50);
      lcd.clear();
      lcd.print(F("RFID OK! v=0x"));
      lcd.print(v, HEX);
      lcd.setCursor(0, 1);
      lcd.print(F("Scan card..."));
      // 不用管 idleShown:它还是初始值false,下一圈loop会自动把屏幕
      // 正式切换成"待机屏幕"(第一行Scan card...,第二行时间)
    }
    delay(500);
    return;
  }

  // ===== 每3秒复查一次连接稳定性,掉线立刻回到自检模式并报告 =====
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck > 3000) {
    lastCheck = millis();
    // 还没对过表:每3秒广播一次"求对表",Mac的logger或ESP32听到就会喂时间
    if (syncUnix == 0) {
      Serial.println(F("CLOCK,WAITING"));
      espSerial.println(F("CLOCK,WAITING"));
    }
    byte v = rfid.PCD_ReadRegister(MFRC522::VersionReg);
    Serial.print(F("Heartbeat VersionReg=0x")); Serial.println(v, HEX);
    if (v == 0x00 || v == 0xFF) {
      rfidOk = false;
      idleShown = false;    // 回到自检模式后,下次恢复要重新画待机屏幕
      lcdInit();            // 顺手重初始化LCD:掉线多半伴随电气抖动,屏可能也被打懵了
      lcd.clear();
      lcd.print(F("RFID LOST!"));
      return;
    }
  }

  // LCD自愈:每60秒重新初始化一次屏幕(空闲时),被噪声干扰花屏/死屏也能1分钟内自动恢复
  static unsigned long lastLcdFix = 0;
  if (millis() - lastLcdFix > 60000) {
    lastLcdFix = millis();
    lcdInit();
    idleShown = false;      // 强制重画待机屏幕
  }

  showIdle(false);

  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;

  String uid = uidToString(rfid.uid);
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  int idx = lookupCard(uid);
  uint32_t ts;
  char tbuf[TIME_BUF_LEN];
  getNowInfo(ts, tbuf, sizeof(tbuf));

  lcd.clear();
  if (idx >= 0) {
    // ===== 白名单里的卡:放行 =====
    lcd.setCursor(0, 0);
    lcd.print(F("Welcome "));
    lcd.print(KNOWN[idx].name);
    lcd.setCursor(0, 1);
    lcd.print(tbuf);

    eepromAppend(ts, (uint8_t)idx);

    // 日志同时发给 Mac(USB)和 ESP32(WiFi上传),谁在线谁处理
    char logline[64];
    snprintf(logline, sizeof(logline), "LOG,%lu,%s,%s,%s,OK",
             (unsigned long)ts, tbuf, KNOWN[idx].name, uid.c_str());
    Serial.println(logline);
    espSerial.println(logline);
  } else {
    // ===== 陌生卡:拒绝 =====
    lcd.setCursor(0, 0);
    lcd.print(F("Access denied"));
    lcd.setCursor(0, 1);
    lcd.print(uid);

    eepromAppend(ts, DENIED_IDX);

    char logline[64];
    snprintf(logline, sizeof(logline), "LOG,%lu,%s,UNKNOWN,%s,DENY",
             (unsigned long)ts, tbuf, uid.c_str());
    Serial.println(logline);
    espSerial.println(logline);
  }

  delay(2500);
  showIdle(true);   // 强制重画:整屏切回"Scan card..." + 当前时间
}
