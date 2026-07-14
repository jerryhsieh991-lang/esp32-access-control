# 门禁站 · 完整搭建指南 (ESP32)

刷 RFID 卡开锁 → 舵机转动 → 绿灯/蜂鸣反馈 →(可选)RTC 记时间 + WiFi 推手机。
**只用一个 ESP32,不用 Arduino、不用 LCD。** 按阶段来,每步先跑通再加下一步。

> ⚠️ 接线前对着模块丝印核对引脚与电压;**RC522 用 3.3V,不要接 5V**(会烧)。改接线一律**先断电**。

---

## 1. 件清单
| 件 | 作用 |
|---|---|
| ESP32 开发板 ×1 | 大脑 + WiFi |
| RC522 RFID 读卡器 + 卡/钥匙扣 | 刷卡识别(**3.3V**) |
| SG90 舵机 | 当锁舌 |
| LED ×2(绿/红)+ 220Ω 电阻 ×2 | 状态灯 |
| 有源蜂鸣器 | 报警声 |
| DS3231 RTC 模块(可选) | 记录进出时间 |
| 面包板 + 杜邦线 | 连接 |
| USB 线 / 5V 供电 | 供电(舵机小,USB 够) |

> **开工前三个新手大坑,先自查:**
> 1. **USB 线必须是数据线**——很多 Micro-USB 线是纯充电线,插上电脑认不出板子。认不出先换线。
> 2. **RC522 通常出厂不焊排针**(排针散装在袋里)。需要电烙铁 + 焊锡把直排针焊上才能插面包板。不会焊就买"已焊排针"版本。
> 3. **macOS/Windows 可能要装 USB 串口驱动**(板子芯片是 CP210x 或 CH340,看板背面丝印)。插上后 Arduino IDE 看不到串口就去装对应驱动。

## 2. 软件准备
1. 装 **Arduino IDE** → 文件→首选项→附加开发板网址填:
   `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
   → 开发板管理器搜 `esp32` 安装。开发板选 **ESP32 Dev Module**。
2. 库管理器安装:**MFRC522**(RFID)、**ESP32Servo**、**RTClib**(RTC,可选)。

## 3. 接线表
| 模块 | 模块引脚 | 接 ESP32 |
|---|---|---|
| **RC522** | SDA(SS) | GPIO 5 |
| | SCK | GPIO 18 |
| | MOSI | GPIO 23 |
| | MISO | GPIO 19 |
| | RST | GPIO 27 |
| | **VCC** | **3.3V** |
| | GND | GND |
| **舵机** | 信号(橙) | GPIO 13 |
| | VCC(红) | 5V(VIN) |
| | GND(棕) | GND |
| 绿 LED | +(经 220Ω) | GPIO 32 |
| 红 LED | +(经 220Ω) | GPIO 33 |
| 蜂鸣器 | + | GPIO 25 |
| **DS3231**(可选) | SDA | GPIO 21 |
| | SCL | GPIO 22 |
| | VCC | 3.3V |

> **键盘怎么办?** 4×4 矩阵键盘要 8 个脚,加上 RFID 后 ESP32 引脚不太够。想加键盘,最省脚的做法是买个 **I2C 键盘/PCF8574 扩展**,和 RTC 共用 SDA=21/SCL=22 两根线,不占额外脚。先把 RFID 版做出来,键盘当后期扩展。

---

## 4. 分阶段搭建

### 阶段 1 — 读到卡号
只接 RC522。烧下面代码,打开串口监视器(115200),刷卡看有没有打印卡号。
```cpp
#include <SPI.h>
#include <MFRC522.h>
#define SS_PIN 5
#define RST_PIN 27
MFRC522 rfid(SS_PIN, RST_PIN);

void setup() {
  Serial.begin(115200);
  SPI.begin(18, 19, 23, 5);   // SCK, MISO, MOSI, SS
  rfid.PCD_Init();
  Serial.println("刷卡试试...");
}
void loop() {
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;
  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) uid += String(rfid.uid.uidByte[i], HEX);
  Serial.println("卡号: " + uid);
  rfid.PICC_HaltA();
  delay(500);
}
```
**把你自己卡的卡号抄下来**,下一步要用。

### 阶段 2 — 白名单 + LED
加两个 LED。认识的卡→绿灯,陌生卡→红灯。把 `WHITELIST` 换成你阶段1抄到的卡号。
```cpp
#define GREEN 32
#define RED 33
String WHITELIST[] = {"a1b2c3d4"};   // <- 换成你的卡号
bool allowed(String uid){ for(auto&w:WHITELIST) if(w==uid) return true; return false; }
// setup 里加: pinMode(GREEN,OUTPUT); pinMode(RED,OUTPUT);
// 读到卡后:
//   if(allowed(uid)){ digitalWrite(GREEN,HIGH); delay(1500); digitalWrite(GREEN,LOW); }
//   else          { digitalWrite(RED,HIGH);   delay(1500); digitalWrite(RED,LOW); }
```

### 阶段 3 — 舵机开锁
认识的卡 → 舵机转到"开"(90°)几秒 → 回"锁"(0°)。
```cpp
#include <ESP32Servo.h>
Servo lock;
// setup: lock.attach(13); lock.write(0);   // 0°=锁
// 认卡通过时:
//   lock.write(90); delay(3000); lock.write(0);   // 开3秒后自动锁
```

### 阶段 4 — 蜂鸣器报警
陌生卡 → 红灯 + 蜂鸣响两声。
```cpp
#define BUZZ 25
// setup: pinMode(BUZZ,OUTPUT);
// 陌生卡时:
//   for(int i=0;i<2;i++){ digitalWrite(BUZZ,HIGH); delay(120); digitalWrite(BUZZ,LOW); delay(120);}
```

### 阶段 5 — RTC 记录时间(可选)
接 DS3231(I2C)。每次开门在串口打印时间戳(以后可存进 flash/SD)。
```cpp
#include <RTClib.h>
RTC_DS3231 rtc;
// setup: rtc.begin(); // 首次运行设一次时间: rtc.adjust(DateTime(F(__DATE__),F(__TIME__)));
// 开门时: DateTime n=rtc.now();
//   Serial.printf("开门 %02d:%02d:%02d\n", n.hour(), n.minute(), n.second());
```

### 阶段 6 — WiFi 推手机(可选)
ESP32 连你家 WiFi,起一个小网页显示"最近开门时间/卡号"。用 `WiFi.h` + `WebServer.h`,手机浏览器输 ESP32 的 IP 就能看。这步做好了,屏幕就"搬到手机上"了。

### ⭐ 智能版(阶段 5+6 合并升级,推荐):`firmware/stage5_smart`
阶段 1–4 实机跑通后,直接烧 `firmware/stage5_smart`,一次获得:
- **NTP 网络对时**:连 WiFi 自动拿准确时间,**DS3231 RTC 模块可以不买了**;断网也照常开门(门禁逻辑不依赖 WiFi)。
- **主卡录卡模式**:刷"主卡"进入录卡状态,15 秒内刷任意新卡即永久加入白名单(存 NVS 闪存,断电不丢)——**加新卡不用改代码重烧**。
- **防暴力破解**:连错 5 次 → 锁定 30 秒,期间任何卡都无效并报警。
- **手机网页面板**:显示当前状态、白名单数量、最近 20 条进出记录,还有"远程开门"按钮。
配置只有两处:文件顶部的 WiFi 账号密码、主卡卡号(`MASTER_UID`)。

---

## 5. 整合逻辑(阶段 1–4 合起来)
```
loop:
  读卡 → 得到 uid
  if 白名单: 绿灯亮 + 舵机开(3秒)+ 自动回锁
  else:      红灯亮 + 蜂鸣两声
```

## 5.5 命令行烧录(可选,本机 arduino-cli 已配好)
不想用 Arduino IDE 也行,本机已装好 arduino-cli + ESP32 核心 + 所有库,终端直接:
```bash
cd ~/Projects/esp32-access-control/firmware
arduino-cli compile --fqbn esp32:esp32:esp32 stage1_read_uid          # 编译
arduino-cli board list                                                # 找板子端口(如 /dev/cu.usbserial-0001)
arduino-cli upload -p /dev/cu.usbserial-0001 --fqbn esp32:esp32:esp32 stage1_read_uid   # 烧录
arduino-cli monitor -p /dev/cu.usbserial-0001 -c baudrate=115200      # 看串口输出
```
> 烧录若卡在 "Connecting...",按住板上 **BOOT** 键再点烧录,连上后松开。

## 6. 排错
- **读不到卡**:检查 RC522 是不是接 **3.3V**;SS/RST 引脚对不对;SPI.begin 参数顺序。
- **舵机不动/抖**:舵机吃电流,别只靠 ESP32 的 3.3V 供;用 5V(VIN)给舵机,**共地**。
- **一直红灯**:白名单卡号大小写/格式和阶段1打印的要一致。
- **ESP32 一直重启**:多半是舵机瞬间拉电流导致掉压,舵机单独 5V 供电。

## 7. 安全
- 全程 5V/3.3V,**这个项目不用 24V**,安全。
- 舵机"锁舌"只是演示;真装门锁涉及机械和安全,先当桌面模型玩。
